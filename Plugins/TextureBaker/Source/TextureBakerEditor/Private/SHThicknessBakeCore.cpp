#include "SHThicknessBakeCore.h"

#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshIndexMappings.h"
#include "DynamicMeshEditor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Image/ImageBuilder.h"
#include "Intersection/IntrRay3Triangle3.h"
#include "MeshDescriptionHelper.h"
#include "MeshElementRemappings.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Sampling/MeshBakerCommon.h"
#include "Sampling/MeshMapBaker.h"
#include "Sampling/MeshMapEvaluator.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "XAtlasWrapper.h"

namespace SHThicknessBaker
{
namespace
{

using namespace UE::Geometry;

// Assumption / needs confirmation: map one Unity unit to one meter
// (100 UE centimeters) for the referenced tool's 0.011/0.01/300 values.
constexpr double UnityCameraNormalOffsetCm = 1.1;
constexpr double UnityCameraNearClipCm = 1.0;
constexpr double UnityCameraFarClipCm = 30000.0;
constexpr double RayDistanceRelativeTolerance = 1.0e-6;

struct FDirectionSample
{
	FVector3d TangentDirection;
	FVector4d AffineProjectionXYZ0;
};

struct FTangentFrameVertex
{
	FVector3d Tangent = FVector3d::Zero();
	FVector3d Normal = FVector3d::Zero();
	double BinormalSign = 1.0;
};

struct FTriangleTangentFrame
{
	FTangentFrameVertex Vertices[3];
};

struct FUVTriangle
{
	int32 TriangleID = INDEX_NONE;
	FVector2d Vertices[3];
	FVector2d Min = FVector2d::Zero();
	FVector2d Max = FVector2d::Zero();
};

bool IsFiniteUV(const FVector2f& UV)
{
	return FMath::IsFinite(UV.X) && FMath::IsFinite(UV.Y);
}

bool IsFiniteVector(const FVector3f& Vector)
{
	return FMath::IsFinite(Vector.X)
		&& FMath::IsFinite(Vector.Y)
		&& FMath::IsFinite(Vector.Z);
}

FText GetUVChannelText(const int32 UVChannel)
{
	return FText::Format(
		NSLOCTEXT("SHThicknessBaker", "UVChannelName", "UV{0}"),
		FText::AsNumber(UVChannel));
}

FIntVector MakeSortedTriangleKey(
	FVertexID A,
	FVertexID B,
	FVertexID C);

bool IsDegenerateSourceTriangle(
	const FMeshDescription& MeshDescription,
	const TVertexAttributesConstRef<FVector3f>& Positions,
	FTriangleID TriangleID);

bool ValidateRenderTangentBasis(
	const FMeshDescription& MeshDescription,
	const bool bRequireTangents,
	FText& OutError)
{
	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexInstanceAttributesConstRef<FVector3f> Normals =
		Attributes.GetVertexInstanceNormals();
	const TVertexInstanceAttributesConstRef<FVector3f> Tangents =
		Attributes.GetVertexInstanceTangents();
	const TVertexInstanceAttributesConstRef<float> BinormalSigns =
		Attributes.GetVertexInstanceBinormalSigns();
	const TVertexAttributesConstRef<FVector3f> Positions =
		Attributes.GetVertexPositions();
	if (!Normals.IsValid()
		|| !Positions.IsValid()
		|| (bRequireTangents
			&& (!Tangents.IsValid() || !BinormalSigns.IsValid())))
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"MissingRenderTangentBasis",
			"UE did not produce the vertex-instance normal/tangent data required for LOD0 after applying its Build Settings.");
		return false;
	}

	TSet<FVertexInstanceID> ReferencedVertexInstances;
	ReferencedVertexInstances.Reserve(
		MeshDescription.Triangles().Num() * 3);
	TSet<FIntVector> UniqueTriangles;
	UniqueTriangles.Reserve(MeshDescription.Triangles().Num());
	for (const FTriangleID TriangleID :
		MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexID> TriangleVertices =
			MeshDescription.GetTriangleVertices(TriangleID);
		const FIntVector TriangleKey = MakeSortedTriangleKey(
			TriangleVertices[0],
			TriangleVertices[1],
			TriangleVertices[2]);
		if (IsDegenerateSourceTriangle(
				MeshDescription,
				Positions,
				TriangleID)
			|| UniqueTriangles.Contains(TriangleKey))
		{
			continue;
		}
		UniqueTriangles.Add(TriangleKey);

		for (const FVertexInstanceID VertexInstanceID :
			MeshDescription.GetTriangleVertexInstances(TriangleID))
		{
			if (ReferencedVertexInstances.Contains(VertexInstanceID))
			{
				continue;
			}
			ReferencedVertexInstances.Add(VertexInstanceID);

			const FVector3f Normal = Normals[VertexInstanceID];
			const FVector3f Tangent = bRequireTangents
				? Tangents[VertexInstanceID]
				: FVector3f(1.0f, 0.0f, 0.0f);
			const float BinormalSign = bRequireTangents
				? BinormalSigns[VertexInstanceID]
				: 1.0f;
			FText Reason;
			if (!IsFiniteVector(Normal))
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderNormalNonFinite",
					"the normal is non-finite");
			}
			else if (bRequireTangents && !IsFiniteVector(Tangent))
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderTangentNonFinite",
					"the tangent is non-finite");
			}
			else if (bRequireTangents && !FMath::IsFinite(BinormalSign))
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderBinormalSignNonFinite",
					"the binormal sign is non-finite");
			}
			else if (Normal.IsNearlyZero())
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderNormalZero",
					"the normal is zero");
			}
			else if (bRequireTangents && Tangent.IsNearlyZero())
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderTangentZero",
					"the tangent is zero");
			}
			else if (bRequireTangents && FMath::IsNearlyZero(BinormalSign))
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderBinormalSignZero",
					"the binormal sign is zero");
			}
			else if (bRequireTangents
				&& FVector3f::CrossProduct(Normal, Tangent).IsNearlyZero())
			{
				Reason = NSLOCTEXT(
					"SHThicknessBaker",
					"RenderNormalTangentParallel",
					"the normal and tangent are parallel");
			}

			if (!Reason.IsEmpty())
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"InvalidRenderTangentBasis",
						"LOD0 contains an invalid render tangent basis after applying its Build Settings at referenced vertex instance {0}: {1}."),
					FText::AsNumber(VertexInstanceID.GetValue()),
					Reason);
				return false;
			}
		}
	}
	return true;
}

bool PrepareRenderTangentBasis(
	UStaticMesh& SourceMesh,
	const FMeshBuildSettings& SourceBuildSettings,
	FMeshDescription& InOutRenderMeshDescription,
	const bool bRequireTangents,
	FText& OutError)
{
	if (InOutRenderMeshDescription.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		InOutRenderMeshDescription.Compact(Remappings);
	}

	FMeshBuildSettings RenderBuildSettings = SourceBuildSettings;
	// Lightmap layout runs after TBN generation and is intentionally omitted:
	// no generated UV may leak into the transient render-equivalent working
	// mesh used by Texture Baker.
	RenderBuildSettings.bGenerateLightmapUVs = false;
	FMeshDescriptionHelper MeshDescriptionHelper(
		&RenderBuildSettings);
	MeshDescriptionHelper.SetupRenderMeshDescription(
		&SourceMesh,
		InOutRenderMeshDescription,
		false,
		bRequireTangents);

	if (InOutRenderMeshDescription.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		InOutRenderMeshDescription.Compact(Remappings);
	}

	return ValidateRenderTangentBasis(
		InOutRenderMeshDescription,
		bRequireTangents,
		OutError);
}

bool PrepareRenderTangentBasis(
	USkeletalMesh& SourceMesh,
	const FSkeletalMeshBuildSettings& BuildSettings,
	FMeshDescription& InOutRenderMeshDescription,
	FText& OutError)
{
	if (InOutRenderMeshDescription.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		InOutRenderMeshDescription.Compact(Remappings);
	}

	FStaticMeshOperations::ValidateAndFixData(
		InOutRenderMeshDescription,
		SourceMesh.GetPathName());
	const float ComparisonThreshold =
		BuildSettings.bRemoveDegenerates
			? UE_SMALL_NUMBER
			: 0.0f;
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(
		InOutRenderMeshDescription,
		ComparisonThreshold,
		*SourceMesh.GetPathName());

	EComputeNTBsFlags ComputeOptions =
		EComputeNTBsFlags::BlendOverlappingNormals;
	ComputeOptions |= BuildSettings.bComputeWeightedNormals
		? EComputeNTBsFlags::WeightedNTBs
		: EComputeNTBsFlags::None;
	ComputeOptions |= BuildSettings.bRecomputeNormals
		? EComputeNTBsFlags::Normals
		: EComputeNTBsFlags::None;
	ComputeOptions |= BuildSettings.bRecomputeTangents
		? EComputeNTBsFlags::Tangents
		: EComputeNTBsFlags::None;
	ComputeOptions |= (BuildSettings.bUseMikkTSpace
				&& (BuildSettings.bRecomputeNormals
					|| BuildSettings.bRecomputeTangents))
		? EComputeNTBsFlags::UseMikkTSpace
		: EComputeNTBsFlags::None;
	ComputeOptions |= BuildSettings.bRemoveDegenerates
		? EComputeNTBsFlags::IgnoreDegenerateTriangles
		: EComputeNTBsFlags::None;
	FStaticMeshOperations::ComputeTangentsAndNormals(
		InOutRenderMeshDescription,
		ComputeOptions);

	if (InOutRenderMeshDescription.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		InOutRenderMeshDescription.Compact(Remappings);
	}

	return ValidateRenderTangentBasis(
		InOutRenderMeshDescription,
		true,
		OutError);
}

bool SynchronizeExistingSharedBakeUV(
	FMeshDescription& MeshDescription,
	const int32 BakeUVChannel,
	FText& OutError)
{
	if (MeshDescription.GetNumUVElementChannels() <= BakeUVChannel
		|| MeshDescription.UVs(BakeUVChannel).GetArraySize() == 0)
	{
		return true;
	}

	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> InstanceUVs =
		Attributes.GetVertexInstanceUVs();
	if (!InstanceUVs.IsValid()
		|| InstanceUVs.GetNumChannels() <= BakeUVChannel)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"SharedBakeUVMissingInstanceData",
			"Texture Baker cannot synchronize the shared Bake UV representation because its vertex-instance data is missing.");
		return false;
	}

	MeshDescription.SuspendUVIndexing();
	MeshDescription.UVs(BakeUVChannel).Reset(
		MeshDescription.VertexInstances().Num());
	TUVAttributesRef<FVector2f> SharedUVCoordinates =
		MeshDescription.UVAttributes(BakeUVChannel)
			.GetAttributesRef<FVector2f>(
				MeshAttribute::UV::UVCoordinate);
	check(SharedUVCoordinates.IsValid());

	TMap<FVertexInstanceID, FUVID> VertexInstanceToSharedUV;
	VertexInstanceToSharedUV.Reserve(
		MeshDescription.VertexInstances().Num());
	TMap<FVertexID, TArray<TPair<FVector2f, FUVID>>>
		SharedUVsByVertex;
	for (const FVertexInstanceID VertexInstanceID :
		MeshDescription.VertexInstances().GetElementIDs())
	{
		const FVertexID VertexID =
			MeshDescription.GetVertexInstanceVertex(
				VertexInstanceID);
		const FVector2f InstanceUV = InstanceUVs.Get(
			VertexInstanceID,
			BakeUVChannel);
		TArray<TPair<FVector2f, FUVID>>& VertexUVs =
			SharedUVsByVertex.FindOrAdd(VertexID);
		FUVID SharedUVID(INDEX_NONE);
		for (const TPair<FVector2f, FUVID>& VertexUV :
			VertexUVs)
		{
			if (VertexUV.Key == InstanceUV)
			{
				SharedUVID = VertexUV.Value;
				break;
			}
		}
		if (SharedUVID.GetValue() == INDEX_NONE)
		{
			SharedUVID =
				MeshDescription.CreateUV(BakeUVChannel);
			SharedUVCoordinates.Set(
				SharedUVID,
				InstanceUV);
			VertexUVs.Emplace(
				InstanceUV,
				SharedUVID);
		}
		VertexInstanceToSharedUV.Add(
			VertexInstanceID,
			SharedUVID);
	}

	TArray<FUVID, TInlineAllocator<3>> TriangleUVs;
	TriangleUVs.SetNumUninitialized(3);
	for (const FTriangleID TriangleID :
		MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			TriangleUVs[CornerIndex] =
				VertexInstanceToSharedUV.FindChecked(
					VertexInstances[CornerIndex]);
		}
		MeshDescription.SetTriangleUVIndices(
			TriangleID,
			TriangleUVs,
			BakeUVChannel);
	}
	MeshDescription.ResumeUVIndexing();

	for (const FTriangleID TriangleID :
		MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		const TArrayView<FUVID> SharedTriangleUVs =
			MeshDescription.GetTriangleUVIndices(
				TriangleID,
				BakeUVChannel);
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			const FUVID SharedUVID =
				SharedTriangleUVs[CornerIndex];
			if (!MeshDescription.IsUVValid(
					SharedUVID,
					BakeUVChannel)
				|| SharedUVCoordinates.Get(SharedUVID)
					!= InstanceUVs.Get(
						VertexInstances[CornerIndex],
						BakeUVChannel))
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"SharedBakeUVSynchronizationFailed",
						"Texture Baker could not synchronize the shared Bake UV at triangle {0}, corner {1}."),
					FText::AsNumber(TriangleID.GetValue()),
					FText::AsNumber(CornerIndex));
				return false;
			}
		}
	}
	return true;
}

double Cross2D(const FVector2d& A, const FVector2d& B)
{
	return A.X * B.Y - A.Y * B.X;
}

FIntVector MakeSortedTriangleKey(
	const FVertexID A,
	const FVertexID B,
	const FVertexID C)
{
	int32 Values[3]{
		A.GetValue(),
		B.GetValue(),
		C.GetValue()
	};
	if (Values[0] > Values[1])
	{
		Swap(Values[0], Values[1]);
	}
	if (Values[1] > Values[2])
	{
		Swap(Values[1], Values[2]);
	}
	if (Values[0] > Values[1])
	{
		Swap(Values[0], Values[1]);
	}
	return FIntVector(Values[0], Values[1], Values[2]);
}

bool IsDegenerateSourceTriangle(
	const FMeshDescription& MeshDescription,
	const TVertexAttributesConstRef<FVector3f>& Positions,
	const FTriangleID TriangleID)
{
	const TArrayView<const FVertexID> Vertices =
		MeshDescription.GetTriangleVertices(TriangleID);
	if (Vertices[0] == Vertices[1]
		|| Vertices[1] == Vertices[2]
		|| Vertices[2] == Vertices[0])
	{
		return true;
	}

	const FVector3f A = Positions[Vertices[0]];
	const FVector3f B = Positions[Vertices[1]];
	const FVector3f C = Positions[Vertices[2]];
	return FVector3f::CrossProduct(B - A, C - A).SizeSquared() <= 0.0f;
}

double ComputeUVIntersectionArea(const FUVTriangle& Subject, const FUVTriangle& Clip)
{
	TArray<FVector2d, TInlineAllocator<8>> Polygon;
	Polygon.Add(Subject.Vertices[0]);
	Polygon.Add(Subject.Vertices[1]);
	Polygon.Add(Subject.Vertices[2]);

	const double ClipWinding = Cross2D(
		Clip.Vertices[1] - Clip.Vertices[0],
		Clip.Vertices[2] - Clip.Vertices[0]);
	const double WindingSign = ClipWinding >= 0.0 ? 1.0 : -1.0;
	constexpr double ClipEpsilon = 1.0e-12;

	for (int32 EdgeIndex = 0; EdgeIndex < 3 && !Polygon.IsEmpty(); ++EdgeIndex)
	{
		const FVector2d EdgeStart = Clip.Vertices[EdgeIndex];
		const FVector2d EdgeEnd = Clip.Vertices[(EdgeIndex + 1) % 3];
		const FVector2d Edge = EdgeEnd - EdgeStart;
		const auto SignedDistance = [EdgeStart, Edge, WindingSign](const FVector2d& Point)
		{
			return WindingSign * Cross2D(Edge, Point - EdgeStart);
		};

		TArray<FVector2d, TInlineAllocator<8>> Input = MoveTemp(Polygon);
		Polygon.Reset();
		FVector2d Previous = Input.Last();
		double PreviousDistance = SignedDistance(Previous);
		bool bPreviousInside = PreviousDistance >= -ClipEpsilon;

		for (const FVector2d& Current : Input)
		{
			const double CurrentDistance = SignedDistance(Current);
			const bool bCurrentInside = CurrentDistance >= -ClipEpsilon;
			if (bCurrentInside != bPreviousInside)
			{
				const double Denominator = PreviousDistance - CurrentDistance;
				if (FMath::Abs(Denominator) > 1.0e-18)
				{
					const double Alpha = PreviousDistance / Denominator;
					Polygon.Add(Previous + (Current - Previous) * Alpha);
				}
			}
			if (bCurrentInside)
			{
				Polygon.Add(Current);
			}
			Previous = Current;
			PreviousDistance = CurrentDistance;
			bPreviousInside = bCurrentInside;
		}
	}

	if (Polygon.Num() < 3)
	{
		return 0.0;
	}

	double AreaTwice = 0.0;
	for (int32 VertexIndex = 0; VertexIndex < Polygon.Num(); ++VertexIndex)
	{
		AreaTwice += Cross2D(
			Polygon[VertexIndex],
			Polygon[(VertexIndex + 1) % Polygon.Num()]);
	}
	return FMath::Abs(AreaTwice) * 0.5;
}

bool ValidateBakeUV(
	const FMeshDescription& MeshDescription,
	const int32 BakeUVChannel,
	const int32 TextureResolution,
	int32* OutSubTexelOverlapCount,
	FText& OutError)
{
	if (OutSubTexelOverlapCount != nullptr)
	{
		*OutSubTexelOverlapCount = 0;
	}

	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	const TVertexAttributesConstRef<FVector3f> Positions =
		Attributes.GetVertexPositions();
	if (!UVs.IsValid() || UVs.GetNumChannels() <= BakeUVChannel)
	{
		OutError = FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"BakeUVMissing",
				"LOD0 does not contain {0}."),
			GetUVChannelText(BakeUVChannel));
		return false;
	}
	if (!Positions.IsValid())
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"MissingPositionsForUVValidation",
			"LOD0 MeshDescription has no vertex positions.");
		return false;
	}

	constexpr double RangeEpsilon = 1.0e-5;
	constexpr double MinUVAreaTwice = 1.0e-12;
	TArray<FUVTriangle> Triangles;
	Triangles.Reserve(MeshDescription.Triangles().Num());
	TSet<FIntVector> UniqueTriangles;
	UniqueTriangles.Reserve(MeshDescription.Triangles().Num());
	for (const FTriangleID TriangleID : MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexID> TriangleVertices =
			MeshDescription.GetTriangleVertices(TriangleID);
		const FIntVector TriangleKey = MakeSortedTriangleKey(
			TriangleVertices[0],
			TriangleVertices[1],
			TriangleVertices[2]);
		if (IsDegenerateSourceTriangle(
				MeshDescription,
				Positions,
				TriangleID)
			|| UniqueTriangles.Contains(TriangleKey))
		{
			continue;
		}
		UniqueTriangles.Add(TriangleKey);

		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		const FVector2d A(UVs.Get(VertexInstances[0], BakeUVChannel));
		const FVector2d B(UVs.Get(VertexInstances[1], BakeUVChannel));
		const FVector2d C(UVs.Get(VertexInstances[2], BakeUVChannel));

		if (!IsFiniteUV(FVector2f(A)) || !IsFiniteUV(FVector2f(B)) || !IsFiniteUV(FVector2f(C)))
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"BakeUVNonFinite",
					"{0} contains a non-finite value at triangle {1}."),
				GetUVChannelText(BakeUVChannel),
				FText::AsNumber(TriangleID.GetValue()));
			return false;
		}

		const auto IsInUnitSquare = [RangeEpsilon](const FVector2d& UV)
		{
			return UV.X >= -RangeEpsilon && UV.X <= 1.0 + RangeEpsilon
				&& UV.Y >= -RangeEpsilon && UV.Y <= 1.0 + RangeEpsilon;
		};
		if (!IsInUnitSquare(A) || !IsInUnitSquare(B) || !IsInUnitSquare(C))
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"BakeUVOutOfRange",
					"{0} must be inside [0,1]. Triangle {1} is out of range."),
				GetUVChannelText(BakeUVChannel),
				FText::AsNumber(TriangleID.GetValue()));
			return false;
		}

		if (FMath::Abs(Cross2D(B - A, C - A)) <= MinUVAreaTwice)
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"BakeUVDegenerate",
					"{0} contains a degenerate triangle at triangle {1}."),
				GetUVChannelText(BakeUVChannel),
				FText::AsNumber(TriangleID.GetValue()));
			return false;
		}

		FUVTriangle& Triangle = Triangles.AddDefaulted_GetRef();
		Triangle.TriangleID = TriangleID.GetValue();
		Triangle.Vertices[0] = A;
		Triangle.Vertices[1] = B;
		Triangle.Vertices[2] = C;
		Triangle.Min = FVector2d(
			FMath::Min3(A.X, B.X, C.X),
			FMath::Min3(A.Y, B.Y, C.Y));
		Triangle.Max = FVector2d(
			FMath::Max3(A.X, B.X, C.X),
			FMath::Max3(A.Y, B.Y, C.Y));
	}

	// Exact positive-area triangle intersection with a sweep-line active set.
	// Shared chart boundaries have zero intersection area and remain valid.
	Triangles.Sort([](const FUVTriangle& Left, const FUVTriangle& Right)
	{
		return Left.Min.X < Right.Min.X;
	});
	TArray<int32> ActiveTriangles;
	constexpr double BoundsEpsilon = 1.0e-12;
	constexpr double MinNumericalOverlapArea = 1.0e-12;
	const double MinHardOverlapArea =
		0.25 / FMath::Square(static_cast<double>(TextureResolution));
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		const FUVTriangle& Triangle = Triangles[TriangleIndex];
		ActiveTriangles.RemoveAllSwap(
			[&Triangles, &Triangle, BoundsEpsilon](const int32 ActiveIndex)
			{
				return Triangles[ActiveIndex].Max.X <= Triangle.Min.X + BoundsEpsilon;
			},
			EAllowShrinking::No);

		for (const int32 ActiveIndex : ActiveTriangles)
		{
			const FUVTriangle& Other = Triangles[ActiveIndex];
			if (Other.Max.Y <= Triangle.Min.Y + BoundsEpsilon
				|| Triangle.Max.Y <= Other.Min.Y + BoundsEpsilon)
			{
				continue;
			}

			const double IntersectionArea =
				ComputeUVIntersectionArea(Triangle, Other);
			if (IntersectionArea > MinHardOverlapArea)
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"BakeUVOverlap",
						"{0} has overlap larger than one quarter of a mip-0 texel between triangles {1} and {2}."),
					GetUVChannelText(BakeUVChannel),
					FText::AsNumber(Other.TriangleID),
					FText::AsNumber(Triangle.TriangleID));
				return false;
			}
			if (OutSubTexelOverlapCount != nullptr
				&& IntersectionArea > MinNumericalOverlapArea)
			{
				++(*OutSubTexelOverlapCount);
			}
		}
		ActiveTriangles.Add(TriangleIndex);
	}

	return true;
}

bool InspectSourceTrianglesForBake(
	const FMeshDescription& MeshDescription,
	const FVector& BuildScale,
	int32& OutDegenerateTriangleCount,
	int32& OutDuplicateTriangleCount,
	FText& OutError)
{
	OutDegenerateTriangleCount = 0;
	OutDuplicateTriangleCount = 0;

	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	if (!Positions.IsValid())
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"MissingPositions",
			"LOD0 MeshDescription has no vertex positions.");
		return false;
	}

	const FVector3f BuildScaleFloat(BuildScale);
	TSet<FIntVector> UniqueTriangles;
	UniqueTriangles.Reserve(MeshDescription.Triangles().Num());
	for (const FTriangleID TriangleID : MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		FVertexID VertexIDs[3];
		FVector3d BakedPositions[3];
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			VertexIDs[CornerIndex] =
				MeshDescription.GetVertexInstanceVertex(VertexInstances[CornerIndex]);
			const FVector3f Position = Positions[VertexIDs[CornerIndex]];
			BakedPositions[CornerIndex] =
				FVector3d(Position * BuildScaleFloat);
			if (!FMath::IsFinite(BakedPositions[CornerIndex].X)
				|| !FMath::IsFinite(BakedPositions[CornerIndex].Y)
				|| !FMath::IsFinite(BakedPositions[CornerIndex].Z))
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"NonFinitePosition",
						"LOD0 contains a non-finite position at triangle {0}."),
					FText::AsNumber(TriangleID.GetValue()));
				return false;
			}
		}

		if (VertexIDs[0] == VertexIDs[1]
			|| VertexIDs[1] == VertexIDs[2]
			|| VertexIDs[2] == VertexIDs[0]
			|| (BakedPositions[1] - BakedPositions[0])
				.Cross(BakedPositions[2] - BakedPositions[0])
				.SquaredLength() <= 0.0)
		{
			++OutDegenerateTriangleCount;
			continue;
		}

		const FIntVector TriangleKey = MakeSortedTriangleKey(
			VertexIDs[0],
			VertexIDs[1],
			VertexIDs[2]);
		if (UniqueTriangles.Contains(TriangleKey))
		{
			++OutDuplicateTriangleCount;
			continue;
		}
		UniqueTriangles.Add(TriangleKey);
	}
	return true;
}

void TransformDirectionOverlay(
	FDynamicMeshNormalOverlay* Overlay,
	const FVector3f& Scale,
	const bool bDivide)
{
	if (Overlay == nullptr)
	{
		return;
	}

	for (int32 ElementID = 0; ElementID < Overlay->MaxElementID(); ++ElementID)
	{
		if (!Overlay->IsElement(ElementID))
		{
			continue;
		}
		FVector3f Direction = Overlay->GetElement(ElementID);
		Direction = bDivide
			? Direction / Scale
			: Direction * Scale;
		Direction.Normalize();
		Overlay->SetElement(ElementID, Direction);
	}
}

void ApplyBuildScale(
	FDynamicMesh3& Mesh,
	const FVector& BuildScale,
	const bool bUseLegacyTangentScaling)
{
	const FVector3f BuildScaleFloat(BuildScale);
	for (const int32 VertexID : Mesh.VertexIndicesItr())
	{
		const FVector3f Position(Mesh.GetVertex(VertexID));
		Mesh.SetVertex(
			VertexID,
			FVector3d(Position * BuildScaleFloat));
	}

	if (!Mesh.HasAttributes())
	{
		return;
	}

	TransformDirectionOverlay(
		Mesh.Attributes()->PrimaryNormals(),
		BuildScaleFloat,
		true);

	if (Mesh.Attributes()->HasTangentSpace())
	{
		// Modern StaticMesh builds transform T/B with BuildScale. Legacy
		// assets retain the old inverse-scale behavior. N always uses the
		// inverse-transpose scale.
		TransformDirectionOverlay(
			Mesh.Attributes()->PrimaryTangents(),
			BuildScaleFloat,
			bUseLegacyTangentScaling);
		TransformDirectionOverlay(
			Mesh.Attributes()->PrimaryBiTangents(),
			BuildScaleFloat,
			bUseLegacyTangentScaling);
	}
}

void RemoveZeroAreaTriangles(FDynamicMesh3& Mesh)
{
	TArray<int32> TrianglesToRemove;
	for (const int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		FVector3d A;
		FVector3d B;
		FVector3d C;
		Mesh.GetTriVertices(TriangleID, A, B, C);
		if ((B - A).Cross(C - A).SquaredLength() <= 0.0)
		{
			TrianglesToRemove.Add(TriangleID);
		}
	}
	for (const int32 TriangleID : TrianglesToRemove)
	{
		const EMeshResult Result =
			Mesh.RemoveTriangle(TriangleID, true, false);
		ensure(Result == EMeshResult::Ok);
	}
}

bool ValidateTangentSpaceBakeMesh(
	const FDynamicMesh3& Mesh,
	const int32 BakeUVChannel,
	int32& OutUncoveredTangentSeamCount,
	FText& OutError)
{
	OutUncoveredTangentSeamCount = 0;

	if (!Mesh.HasAttributes() || !Mesh.Attributes()->HasTangentSpace())
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"DynamicMeshTangentBasisMissing",
			"The LOD0 tangent basis was lost while creating the bake mesh.");
		return false;
	}

	const FDynamicMeshNormalOverlay* Normals =
		Mesh.Attributes()->PrimaryNormals();
	const FDynamicMeshNormalOverlay* Tangents =
		Mesh.Attributes()->PrimaryTangents();
	const FDynamicMeshNormalOverlay* Bitangents =
		Mesh.Attributes()->PrimaryBiTangents();
	const FDynamicMeshUVOverlay* BakeUVs =
		Mesh.Attributes()->GetUVLayer(BakeUVChannel);
	if (BakeUVs == nullptr)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"DynamicMeshBakeUVMissingForTangentValidation",
			"The selected Bake UV was lost while creating the tangent-space bake mesh.");
		return false;
	}

	for (const int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		if (!Normals->IsSetTriangle(TriangleID)
			|| !Tangents->IsSetTriangle(TriangleID)
			|| !Bitangents->IsSetTriangle(TriangleID))
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"UnsetTangentTriangle",
					"LOD0 tangent data is missing at triangle {0}."),
				FText::AsNumber(TriangleID));
			return false;
		}

		int32 TriangleHandedness = 0;
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			FVector3f Normal;
			FVector3f Tangent;
			FVector3f Bitangent;
			Normals->GetTriElement(TriangleID, CornerIndex, Normal);
			Tangents->GetTriElement(TriangleID, CornerIndex, Tangent);
			Bitangents->GetTriElement(TriangleID, CornerIndex, Bitangent);
			const FVector3f Cross = FVector3f::CrossProduct(Normal, Tangent);
			if (!IsFiniteVector(Normal)
				|| !IsFiniteVector(Tangent)
				|| !IsFiniteVector(Bitangent)
				|| Normal.IsNearlyZero()
				|| Tangent.IsNearlyZero()
				|| Bitangent.IsNearlyZero()
				|| Cross.IsNearlyZero()
				|| FMath::Abs(FVector3f::DotProduct(
					Cross.GetSafeNormal(),
					Bitangent.GetSafeNormal())) <= 1.0e-4f)
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"InvalidDynamicTangentBasis",
						"LOD0 contains an invalid render tangent basis at triangle {0}, corner {1}."),
					FText::AsNumber(TriangleID),
					FText::AsNumber(CornerIndex));
				return false;
			}

			const int32 CornerHandedness =
				FVector3f::DotProduct(
					Tangent,
					FVector3f::CrossProduct(Bitangent, Normal)) < 0.0f
					? -1
					: 1;
			if (TriangleHandedness == 0)
			{
				TriangleHandedness = CornerHandedness;
			}
			else if (TriangleHandedness != CornerHandedness)
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"MixedTriangleTangentHandedness",
						"LOD0 render tangent handedness changes inside triangle {0}. The interpolated shader tangent frame becomes singular, so this triangle cannot be baked correctly."),
					FText::AsNumber(TriangleID));
				return false;
			}
		}
	}

	const auto FindTriangleCorner = [&Mesh](
		const int32 TriangleID,
		const int32 VertexID) -> int32
	{
		const FIndex3i Triangle = Mesh.GetTriangle(TriangleID);
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			if (Triangle[CornerIndex] == VertexID)
			{
				return CornerIndex;
			}
		}
		return INDEX_NONE;
	};
	const auto DirectionsDiffer = [](
		const FVector3f& Left,
		const FVector3f& Right)
	{
		return FVector3f::DotProduct(
			Left.GetSafeNormal(),
			Right.GetSafeNormal()) < 0.9999f;
	};

	for (const int32 EdgeID : Mesh.EdgeIndicesItr())
	{
		if (BakeUVs->IsSeamEdge(EdgeID))
		{
			continue;
		}

		const FIndex2i EdgeTriangles = Mesh.GetEdgeT(EdgeID);
		if (EdgeTriangles.A == FDynamicMesh3::InvalidID
			|| EdgeTriangles.B == FDynamicMesh3::InvalidID)
		{
			continue;
		}

		const FIndex2i EdgeVertices = Mesh.GetEdgeV(EdgeID);
		bool bMeaningfulBasisChange = false;
		for (const int32 VertexID : { EdgeVertices.A, EdgeVertices.B })
		{
			const int32 CornerA =
				FindTriangleCorner(EdgeTriangles.A, VertexID);
			const int32 CornerB =
				FindTriangleCorner(EdgeTriangles.B, VertexID);
			check(CornerA != INDEX_NONE && CornerB != INDEX_NONE);

			FVector3f NormalA;
			FVector3f NormalB;
			FVector3f TangentA;
			FVector3f TangentB;
			FVector3f BitangentA;
			FVector3f BitangentB;
			Normals->GetTriElement(
				EdgeTriangles.A,
				CornerA,
				NormalA);
			Normals->GetTriElement(
				EdgeTriangles.B,
				CornerB,
				NormalB);
			Tangents->GetTriElement(
				EdgeTriangles.A,
				CornerA,
				TangentA);
			Tangents->GetTriElement(
				EdgeTriangles.B,
				CornerB,
				TangentB);
			Bitangents->GetTriElement(
				EdgeTriangles.A,
				CornerA,
				BitangentA);
			Bitangents->GetTriElement(
				EdgeTriangles.B,
				CornerB,
				BitangentB);
			if (DirectionsDiffer(NormalA, NormalB)
				|| DirectionsDiffer(TangentA, TangentB)
				|| DirectionsDiffer(BitangentA, BitangentB))
			{
				bMeaningfulBasisChange = true;
				break;
			}
		}
		if (bMeaningfulBasisChange)
		{
			++OutUncoveredTangentSeamCount;
		}
	}

	return true;
}

bool ValidateRayTraceGeometry(
	const FDynamicMesh3& Mesh,
	double& OutDiagonal,
	FText& OutError)
{
	if (Mesh.TriangleCount() == 0 || Mesh.VertexCount() == 0)
	{
		OutError = NSLOCTEXT("SHThicknessBaker", "EmptyMesh", "LOD0 has no triangles.");
		return false;
	}

	FAxisAlignedBox3d Bounds = FAxisAlignedBox3d::Empty();
	for (const int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		Bounds.Contain(Mesh.GetTriBounds(TriangleID));
	}
	OutDiagonal = Bounds.DiagonalLength();
	if (!FMath::IsFinite(OutDiagonal) || OutDiagonal <= UE_DOUBLE_SMALL_NUMBER)
	{
		OutError = NSLOCTEXT("SHThicknessBaker", "ZeroBounds", "LOD0 has zero or invalid bounds after Build Scale.");
		return false;
	}

	const double MinAreaTwice = FMath::Square(OutDiagonal) * 1.0e-16;
	for (const int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		FVector3d A;
		FVector3d B;
		FVector3d C;
		Mesh.GetTriVertices(TriangleID, A, B, C);
		if ((B - A).Cross(C - A).SquaredLength() <= FMath::Square(MinAreaTwice))
		{
			OutError = FText::Format(
				NSLOCTEXT("SHThicknessBaker", "DegenerateGeometry", "LOD0 contains a degenerate triangle at triangle {0}."),
				FText::AsNumber(TriangleID));
			return false;
		}
	}

	return true;
}

bool ValidateConvertedBakeUV(
	const FMeshDescription& MeshDescription,
	const FMeshDescriptionToDynamicMesh& Converter,
	const FDynamicMesh3& Mesh,
	const int32 BakeUVChannel,
	FText& OutError)
{
	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> SourceUVs =
		Attributes.GetVertexInstanceUVs();
	const FDynamicMeshUVOverlay* ConvertedUVs =
		Mesh.HasAttributes()
			? Mesh.Attributes()->GetUVLayer(BakeUVChannel)
			: nullptr;
	if (!SourceUVs.IsValid()
		|| SourceUVs.GetNumChannels() <= BakeUVChannel
		|| ConvertedUVs == nullptr)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"ConvertedBakeUVMissing",
			"The bake mesh conversion did not preserve the selected Bake UV.");
		return false;
	}

	for (const int32 DynamicTriangleID : Mesh.TriangleIndicesItr())
	{
		if (!Converter.TriIDMap.IsValidIndex(DynamicTriangleID)
			|| !MeshDescription.IsTriangleValid(
				Converter.TriIDMap[DynamicTriangleID])
			|| !ConvertedUVs->IsSetTriangle(DynamicTriangleID))
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"ConvertedBakeTriangleMappingInvalid",
				"The bake mesh conversion returned an invalid triangle or Bake UV mapping.");
			return false;
		}

		const FTriangleID SourceTriangleID =
			Converter.TriIDMap[DynamicTriangleID];
		const TArrayView<const FVertexInstanceID> SourceVertexInstances =
			MeshDescription.GetTriangleVertexInstances(SourceTriangleID);
		const FIndex3i DynamicTriangle =
			Mesh.GetTriangle(DynamicTriangleID);
		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			const int32 DynamicVertexID = DynamicTriangle[CornerIndex];
			const FVertexID SourceVertexID =
				MeshDescription.GetVertexInstanceVertex(
					SourceVertexInstances[CornerIndex]);
			if (!Converter.VertIDMap.IsValidIndex(DynamicVertexID)
				|| Converter.VertIDMap[DynamicVertexID]
					!= SourceVertexID)
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"ConvertedBakeCornerMappingInvalid",
						"The bake mesh conversion changed triangle {0} corner ordering."),
					FText::AsNumber(SourceTriangleID.GetValue()));
				return false;
			}

			FVector2f ConvertedUV;
			ConvertedUVs->GetTriElement(
				DynamicTriangleID,
				CornerIndex,
				ConvertedUV);
			const FVector2f SourceUV = SourceUVs.Get(
				SourceVertexInstances[CornerIndex],
				BakeUVChannel);
			if (ConvertedUV != SourceUV)
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"ConvertedBakeUVMismatch",
						"The bake mesh conversion did not preserve {0} at triangle {1}, corner {2}."),
					GetUVChannelText(BakeUVChannel),
					FText::AsNumber(SourceTriangleID.GetValue()),
					FText::AsNumber(CornerIndex));
				return false;
			}
		}
	}
	return true;
}

bool ConvertMeshDescription(
	const FMeshDescription& MeshDescription,
	const FVector& BuildScale,
	const bool bUseLegacyTangentScaling,
	const bool bCopyTangents,
	const int32 BakeUVChannel,
	TUniquePtr<FDynamicMesh3>& OutMesh,
	double& OutDiagonal,
	FText& OutError)
{
	OutMesh = MakeUnique<FDynamicMesh3>();
	FMeshDescriptionToDynamicMesh Converter;
	Converter.bUseCompactedPolygonGroupIDValues = true;
	Converter.Convert(&MeshDescription, *OutMesh, bCopyTangents);
	ApplyBuildScale(*OutMesh, BuildScale, bUseLegacyTangentScaling);
	RemoveZeroAreaTriangles(*OutMesh);
	if (!ValidateConvertedBakeUV(
			MeshDescription,
			Converter,
			*OutMesh,
			BakeUVChannel,
			OutError))
		{
			return false;
		}
	return ValidateRayTraceGeometry(
		*OutMesh,
		OutDiagonal,
		OutError);
}

bool GenerateBakeUV(
	FMeshDescription& MeshDescription,
	const FVector& BuildScale,
	const FBakeSettings& Settings,
	const int32 BakeUVChannel,
	FText& OutError)
{
	FStaticMeshAttributes Attributes(MeshDescription);
	const FStaticMeshConstAttributes ConstAttributes(MeshDescription);
	const TVertexAttributesConstRef<FVector3f> Positions =
		ConstAttributes.GetVertexPositions();

	TMap<FVertexID, int32> DenseVertexIndices;
	TArray<FVector3f> VertexBuffer;
	VertexBuffer.Reserve(MeshDescription.Vertices().Num());

	TArray<int32> IndexBuffer;
	TArray<FVertexInstanceID> CornerVertexInstances;
	IndexBuffer.Reserve(MeshDescription.Triangles().Num() * 3);
	CornerVertexInstances.Reserve(MeshDescription.Triangles().Num() * 3);
	TSet<FIntVector> UniqueTriangles;
	UniqueTriangles.Reserve(MeshDescription.Triangles().Num());
	for (const FTriangleID TriangleID : MeshDescription.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexID> TriangleVertices =
			MeshDescription.GetTriangleVertices(TriangleID);
		const FIntVector TriangleKey = MakeSortedTriangleKey(
			TriangleVertices[0],
			TriangleVertices[1],
			TriangleVertices[2]);
		if (IsDegenerateSourceTriangle(
				MeshDescription,
				Positions,
				TriangleID)
			|| UniqueTriangles.Contains(TriangleKey))
		{
			continue;
		}
		UniqueTriangles.Add(TriangleKey);

		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		for (const FVertexInstanceID VertexInstanceID : VertexInstances)
		{
			const FVertexID VertexID = MeshDescription.GetVertexInstanceVertex(VertexInstanceID);
			int32* ExistingDenseIndex =
				DenseVertexIndices.Find(VertexID);
			int32 DenseIndex;
			if (ExistingDenseIndex != nullptr)
			{
				DenseIndex = *ExistingDenseIndex;
			}
			else
			{
				const FVector3f SourcePosition = Positions[VertexID];
				const FVector3f BakedPosition(
					SourcePosition.X * static_cast<float>(BuildScale.X),
					SourcePosition.Y * static_cast<float>(BuildScale.Y),
					SourcePosition.Z * static_cast<float>(BuildScale.Z));
				DenseIndex = VertexBuffer.Add(BakedPosition);
				DenseVertexIndices.Add(VertexID, DenseIndex);
			}
			IndexBuffer.Add(DenseIndex);
			CornerVertexInstances.Add(VertexInstanceID);
		}
	}

	XAtlasWrapper::XAtlasChartOptions ChartOptions;
	XAtlasWrapper::XAtlasPackOptions PackOptions;
	PackOptions.Resolution =
		GetTextureResolution(Settings.TextureResolution);
	PackOptions.Padding = Settings.PaddingSize;
	// UE's wrapper does not expose xatlas sub-atlas indices. Supplying both a
	// fixed resolution and a non-zero texel density permits multiple
	// sub-atlases, whose normalized UVs then overlap. Let xatlas estimate the
	// density so it packs one expandable atlas around the target resolution.
	PackOptions.TexelsPerUnit = 0.0f;
	PackOptions.bBilinear = true;
	PackOptions.bBlockAlign = true;

	TArray<FVector2D> UVVertexBuffer;
	TArray<int32> UVIndexBuffer;
	TArray<int32> VertexRemapArray;
	if (!XAtlasWrapper::ComputeUVs(
			IndexBuffer,
			VertexBuffer,
			ChartOptions,
			PackOptions,
			UVVertexBuffer,
			UVIndexBuffer,
			VertexRemapArray))
	{
		OutError = FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"XAtlasFailed",
				"XAtlas failed to generate {0}."),
			GetUVChannelText(BakeUVChannel));
		return false;
	}

	if (UVIndexBuffer.Num() != CornerVertexInstances.Num())
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"XAtlasIndexMismatch",
			"XAtlas returned an unexpected corner count.");
		return false;
	}

	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	if (!UVs.IsValid() || UVs.GetNumChannels() < 1)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"UnexpectedUVCount",
			"Bake UV generation requires UV0 to exist.");
		return false;
	}
	if (UVs.GetNumChannels() <= BakeUVChannel)
	{
		UVs.SetNumChannels(BakeUVChannel + 1);
	}

	TMap<FVertexInstanceID, FVector2f> GeneratedUVs;
	GeneratedUVs.Reserve(CornerVertexInstances.Num());
	for (int32 CornerIndex = 0; CornerIndex < CornerVertexInstances.Num(); ++CornerIndex)
	{
		const int32 UVVertexIndex = UVIndexBuffer[CornerIndex];
		if (!UVVertexBuffer.IsValidIndex(UVVertexIndex))
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"XAtlasInvalidIndex",
				"XAtlas returned an invalid UV vertex index.");
			return false;
		}
		if (!VertexRemapArray.IsValidIndex(UVVertexIndex)
			|| VertexRemapArray[UVVertexIndex] != IndexBuffer[CornerIndex])
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"XAtlasCornerOrderMismatch",
				"XAtlas reordered a triangle corner unexpectedly; the Bake UV was not written.");
			return false;
		}
		const FVector2f GeneratedUV(UVVertexBuffer[UVVertexIndex]);
		const FVertexInstanceID VertexInstanceID =
			CornerVertexInstances[CornerIndex];
		if (const FVector2f* ExistingGeneratedUV =
			GeneratedUVs.Find(VertexInstanceID))
		{
			if (!ExistingGeneratedUV->Equals(GeneratedUV, 1.0e-6f))
			{
				OutError = NSLOCTEXT(
					"SHThicknessBaker",
					"XAtlasVertexInstanceSplit",
					"XAtlas requires a UV seam inside a shared MeshDescription vertex instance. The current Texture Baker UV writer cannot represent that unwrap without changing source topology.");
				return false;
			}
		}
		else
		{
			GeneratedUVs.Add(VertexInstanceID, GeneratedUV);
		}
	}
	for (const TPair<FVertexInstanceID, FVector2f>& GeneratedPair : GeneratedUVs)
	{
		UVs.Set(
			GeneratedPair.Key,
			BakeUVChannel,
			GeneratedPair.Value);
	}

	return true;
}

TArray<FDirectionSample> BuildDirectionSet(const int32 DirectionCount)
{
	const int32 HalfCount = DirectionCount / 2;
	TArray<FDirectionSample> Result;
	Result.Reserve(DirectionCount);
	const double GoldenAngle = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));

	for (int32 Index = 0; Index < HalfCount; ++Index)
	{
		const double Z = (static_cast<double>(Index) + 0.5) / HalfCount;
		const double Radius = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		const double Phi = GoldenAngle * Index;
		const FVector3d Direction(
			Radius * FMath::Cos(Phi),
			Radius * FMath::Sin(Phi),
			Z);

		for (const double Sign : { 1.0, -1.0 })
		{
			const FVector3d SignedDirection = Direction * Sign;
			FDirectionSample& Sample = Result.AddDefaulted_GetRef();
			Sample.TangentDirection = SignedDirection;
			Sample.AffineProjectionXYZ0 = FVector4d(
				3.0 * SignedDirection.X,
				3.0 * SignedDirection.Y,
				3.0 * SignedDirection.Z,
				1.0);
		}
	}

	return Result;
}

class FSHThicknessMapEvaluator final : public FMeshMapEvaluator
{
public:
	FSHThicknessMapEvaluator(
		const FDynamicMesh3& InSurfaceMesh,
		const FDynamicMesh3& InRayMesh,
		const FDynamicMeshAABBTree3& InSpatial,
		const double InThicknessScale,
		const int32 InDirectionCount,
		const ECoefficientSpace InCoefficientSpace,
		std::atomic<bool>& InCancelRequested,
		std::atomic<int64>& InInvalidRayCount)
		: SurfaceMesh(InSurfaceMesh)
		, RayMesh(InRayMesh)
		, Spatial(InSpatial)
		, ThicknessScale(InThicknessScale)
		, Directions(BuildDirectionSet(InDirectionCount))
		, CoefficientSpace(InCoefficientSpace)
		, CancelRequested(InCancelRequested)
		, InvalidRayCount(InInvalidRayCount)
	{
		check(SurfaceMesh.HasAttributes());
		const FDynamicMeshNormalOverlay* Normals =
			SurfaceMesh.Attributes()->PrimaryNormals();
		const FDynamicMeshNormalOverlay* Tangents =
			SurfaceMesh.Attributes()->PrimaryTangents();
		const FDynamicMeshNormalOverlay* Bitangents =
			SurfaceMesh.Attributes()->PrimaryBiTangents();
		check(Normals);
		check(
			CoefficientSpace != ECoefficientSpace::Tangent
				|| (Tangents && Bitangents));

		TangentFrames.SetNum(SurfaceMesh.MaxTriangleID());
		for (const int32 TriangleID : SurfaceMesh.TriangleIndicesItr())
		{
			FTriangleTangentFrame& Frame = TangentFrames[TriangleID];
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				FVector3f NormalFloat;
				Normals->GetTriElement(TriangleID, CornerIndex, NormalFloat);
				FVector3d Normal(NormalFloat);
				Normal.Normalize();

				FTangentFrameVertex& Vertex = Frame.Vertices[CornerIndex];
				Vertex.Normal = Normal;
				if (CoefficientSpace == ECoefficientSpace::Tangent)
				{
					FVector3f TangentFloat;
					FVector3f BitangentFloat;
					Tangents->GetTriElement(
						TriangleID,
						CornerIndex,
						TangentFloat);
					Bitangents->GetTriElement(
						TriangleID,
						CornerIndex,
						BitangentFloat);

					FVector3d Tangent(TangentFloat);
					FVector3d ReferenceBitangent(BitangentFloat);
					Tangent.Normalize();
					ReferenceBitangent.Normalize();
					const double BinormalSign =
						Tangent.Dot(
							ReferenceBitangent.Cross(Normal)) < 0.0
							? -1.0
							: 1.0;

					// LocalVertexFactory reconstructs Y and then corrects X in
					// the vertex shader. The pixel path interpolates X, Z, and
					// sign, then reconstructs Y from those interpolants.
					const FVector3d ShaderBitangent =
						Normal.Cross(Tangent) * BinormalSign;
					Vertex.Tangent =
						ShaderBitangent.Cross(Normal)
							* BinormalSign;
					Vertex.BinormalSign = BinormalSign;
				}
			}
		}
	}

	virtual void Setup(const FMeshBaseBaker& Baker, FEvaluationContext& Context) override
	{
		Context.Evaluate = &EvaluateSample;
		Context.EvaluateDefault = &EvaluateDefault;
		Context.EvaluateColor = &EvaluateColor;
		Context.EvalData = this;
		Context.AccumulateMode = EAccumulateMode::Add;
		Context.DataLayout = DataLayout();
	}

	virtual const TArray<EComponents>& DataLayout() const override
	{
		static const TArray<EComponents> Layout{ EComponents::Float4 };
		return Layout;
	}

	virtual EMeshMapEvaluatorType Type() const override
	{
		// FMeshMapBaker does not switch on this tag. Property is the closest
		// built-in category for this custom, linearly filtered Float4 field.
		return EMeshMapEvaluatorType::Property;
	}

private:
	bool GetSampleTangentFrame(
		const FCorrespondenceSample& Sample,
		FVector3d& OutTangent,
		FVector3d& OutBitangent,
		FVector3d& OutNormal) const
	{
		const int32 TriangleID = Sample.BaseSample.TriangleIndex;
		if (!SurfaceMesh.IsTriangle(TriangleID)
			|| !TangentFrames.IsValidIndex(TriangleID))
		{
			return false;
		}

		const FVector3d BaryCoords = Sample.BaseSample.BaryCoords;
		const FTriangleTangentFrame& Frame = TangentFrames[TriangleID];
		OutTangent =
			Frame.Vertices[0].Tangent * BaryCoords.X
			+ Frame.Vertices[1].Tangent * BaryCoords.Y
			+ Frame.Vertices[2].Tangent * BaryCoords.Z;
		OutNormal =
			Frame.Vertices[0].Normal * BaryCoords.X
			+ Frame.Vertices[1].Normal * BaryCoords.Y
			+ Frame.Vertices[2].Normal * BaryCoords.Z;
		const double BinormalSign =
			Frame.Vertices[0].BinormalSign * BaryCoords.X
			+ Frame.Vertices[1].BinormalSign * BaryCoords.Y
			+ Frame.Vertices[2].BinormalSign * BaryCoords.Z;
		OutBitangent =
			OutNormal.Cross(OutTangent) * BinormalSign;

		const bool bNormalValid =
			FMath::IsFinite(OutNormal.X)
			&& FMath::IsFinite(OutNormal.Y)
			&& FMath::IsFinite(OutNormal.Z)
			&& OutNormal.SquaredLength() > UE_DOUBLE_SMALL_NUMBER;
		if (CoefficientSpace == ECoefficientSpace::Local)
		{
			return bNormalValid;
		}

		const double Determinant =
			OutTangent.Dot(OutBitangent.Cross(OutNormal));
		return bNormalValid
			&& FMath::IsFinite(OutTangent.X)
			&& FMath::IsFinite(OutTangent.Y)
			&& FMath::IsFinite(OutTangent.Z)
			&& FMath::IsFinite(OutBitangent.X)
			&& FMath::IsFinite(OutBitangent.Y)
			&& FMath::IsFinite(OutBitangent.Z)
			&& FMath::IsFinite(BinormalSign)
			&& FMath::Abs(BinormalSign) > 1.0e-8
			&& FMath::IsFinite(Determinant)
			&& FMath::Abs(Determinant) > 1.0e-8;
	}

	bool ComputeFarthestHitDistance(
		const FVector3d& SurfacePoint,
		const FVector3d& SurfaceNormal,
		const FVector3d& Direction,
		TArray<MeshIntersection::FHitIntersectionResult>& Hits,
		double& OutDistance) const
	{
		OutDistance = 0.0;
		const double NormalLengthSquared = SurfaceNormal.SquaredLength();
		if (!FMath::IsFinite(NormalLengthSquared)
			|| NormalLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		const FVector3d OffsetNormal =
			SurfaceNormal / FMath::Sqrt(NormalLengthSquared);
		const FVector3d Origin =
			SurfacePoint - OffsetNormal * UnityCameraNormalOffsetCm;

		// Unity renders a 90-degree cubemap with near/far clip planes. For a
		// unit ray, cubemap face depth is t * max(abs(Direction)).
		const double MajorAxis = FMath::Max(
			FMath::Abs(Direction.X),
			FMath::Max(FMath::Abs(Direction.Y), FMath::Abs(Direction.Z)));
		if (!FMath::IsFinite(MajorAxis)
			|| MajorAxis <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		const double NearDistance = UnityCameraNearClipCm / MajorAxis;
		const double FarDistance = UnityCameraFarClipCm / MajorAxis;
		const double QueryMaxDistance = FMath::Min(
			ThicknessScale * (1.0 + RayDistanceRelativeTolerance),
			FarDistance);
		if (QueryMaxDistance < NearDistance)
		{
			return true;
		}

		Hits.Reset();
		const FWatertightRay3d Ray(Origin, Direction);
		FDynamicMeshAABBTree3::FQueryOptions QueryOptions(QueryMaxDistance);
		if (!Spatial.FindAllHitTriangles(Ray, Hits, QueryOptions))
		{
			return true;
		}

		for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
		{
			if (!RayMesh.IsTriangle(Hit.TriangleId)
				|| !FMath::IsFinite(Hit.Distance))
			{
				return false;
			}
			if (Hit.Distance >= NearDistance
				&& Hit.Distance
					<= QueryMaxDistance * (1.0 + RayDistanceRelativeTolerance))
			{
				OutDistance = FMath::Max(OutDistance, Hit.Distance);
			}
		}

		return FMath::IsFinite(OutDistance)
			&& OutDistance >= 0.0
			&& OutDistance
				<= ThicknessScale * (1.0 + RayDistanceRelativeTolerance);
	}

	FVector4f ComputeCoefficients(const FCorrespondenceSample& Sample) const
	{
		if (CancelRequested.load(std::memory_order_relaxed))
		{
			return FVector4f::Zero();
		}

		const int32 TriangleID = Sample.BaseSample.TriangleIndex;
		if (!SurfaceMesh.IsTriangle(TriangleID))
		{
			InvalidRayCount.fetch_add(1, std::memory_order_relaxed);
			return FVector4f::Zero();
		}

		FVector3d Tangent;
		FVector3d Bitangent;
		FVector3d Normal;
		if (!GetSampleTangentFrame(Sample, Tangent, Bitangent, Normal))
		{
			InvalidRayCount.fetch_add(1, std::memory_order_relaxed);
			return FVector4f::Zero();
		}

		FVector4d Coefficients = FVector4d::Zero();
		const double ProjectionWeight = 1.0 / Directions.Num();
		TArray<MeshIntersection::FHitIntersectionResult> Hits;
		for (const FDirectionSample& DirectionSample : Directions)
		{
			if (CancelRequested.load(std::memory_order_relaxed)
				|| InvalidRayCount.load(std::memory_order_relaxed) > 0)
			{
				return FVector4f::Zero();
			}

			// This is the inverse of the captured shader's normalized
			// inverse-TBN direction transform, including interpolated skew.
			FVector3d ObjectDirection =
				DirectionSample.TangentDirection;
			if (CoefficientSpace == ECoefficientSpace::Tangent)
			{
				ObjectDirection =
					Tangent * DirectionSample.TangentDirection.X
					+ Bitangent * DirectionSample.TangentDirection.Y
					+ Normal * DirectionSample.TangentDirection.Z;
			}
			const double ObjectDirectionLengthSquared = ObjectDirection.SquaredLength();
			if (!FMath::IsFinite(ObjectDirectionLengthSquared)
				|| ObjectDirectionLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
			{
				InvalidRayCount.fetch_add(1, std::memory_order_relaxed);
				return FVector4f::Zero();
			}
			ObjectDirection /= FMath::Sqrt(ObjectDirectionLengthSquared);

			double FarthestHitDistance = 0.0;
			if (!ComputeFarthestHitDistance(
				Sample.BaseSample.SurfacePoint,
				Normal,
				ObjectDirection,
				Hits,
				FarthestHitDistance))
			{
				InvalidRayCount.fetch_add(1, std::memory_order_relaxed);
				return FVector4f::Zero();
			}

			const double NormalizedThickness =
				FarthestHitDistance / ThicknessScale;
			Coefficients += DirectionSample.AffineProjectionXYZ0
				* (NormalizedThickness * ProjectionWeight);
		}

		if (!FMath::IsFinite(Coefficients.X)
			|| !FMath::IsFinite(Coefficients.Y)
			|| !FMath::IsFinite(Coefficients.Z)
			|| !FMath::IsFinite(Coefficients.W))
		{
			InvalidRayCount.fetch_add(1, std::memory_order_relaxed);
			return FVector4f::Zero();
		}
		return FVector4f(Coefficients);
	}

	static void EvaluateSample(float*& Out, const FCorrespondenceSample& Sample, void* EvalData)
	{
		const FSHThicknessMapEvaluator* Evaluator =
			static_cast<const FSHThicknessMapEvaluator*>(EvalData);
		WriteToBuffer(Out, Evaluator->ComputeCoefficients(Sample));
	}

	static void EvaluateDefault(float*& Out, void* EvalData)
	{
		WriteToBuffer(Out, FVector4f::Zero());
	}

	static void EvaluateColor(const int DataIndex, float*& In, FVector4f& Out, void* EvalData)
	{
		Out = FVector4f(In[0], In[1], In[2], In[3]);
		In += 4;
	}

	const FDynamicMesh3& SurfaceMesh;
	const FDynamicMesh3& RayMesh;
	const FDynamicMeshAABBTree3& Spatial;
	double ThicknessScale = 0.0;
	TArray<FDirectionSample> Directions;
	TArray<FTriangleTangentFrame> TangentFrames;
	ECoefficientSpace CoefficientSpace = ECoefficientSpace::Tangent;
	std::atomic<bool>& CancelRequested;
	std::atomic<int64>& InvalidRayCount;
};

} // namespace

FBakeJob::FBakeJob(FBakePreparation&& InPreparation)
	: Preparation(MoveTemp(InPreparation))
{
}

void FBakeJob::RequestCancel()
{
	bCancelRequested.store(true, std::memory_order_relaxed);
}

FString FBakeJob::GetStatusText() const
{
	switch (Stage.load())
	{
	case EJobStage::Ready:
		return TEXT("Preparing SH thickness bake...");
	case EJobStage::Baking:
		return FString::Printf(
			TEXT("CPU baking SH thickness... %lld surface samples"),
			ProcessedSurfaceSamples.load(std::memory_order_relaxed));
	case EJobStage::CollectingSamples:
		return FString::Printf(
			TEXT("Collecting GPU surface samples... %lld samples"),
			ProcessedSurfaceSamples.load(std::memory_order_relaxed));
	case EJobStage::GPUComputing:
		return FString::Printf(
			TEXT("GPU baking SH thickness... %lld / %lld samples"),
			ProcessedSurfaceSamples.load(std::memory_order_relaxed),
			TotalSurfaceSamples.load(std::memory_order_relaxed));
	case EJobStage::Filtering:
		return TEXT("Filtering GPU coefficient samples into the texture...");
	case EJobStage::Encoding:
		return TEXT("Collecting coefficient texture...");
	case EJobStage::Succeeded:
		return TEXT("SH thickness bake completed.");
	case EJobStage::Cancelled:
		return TEXT("SH thickness bake cancelled.");
	case EJobStage::Failed:
		return Error.IsEmpty() ? TEXT("SH thickness bake failed.") : Error;
	default:
		return TEXT("SH thickness bake...");
	}
}

void FBakeJob::RunCPU()
{
	if (bCancelRequested.load(std::memory_order_relaxed))
	{
		Stage.store(EJobStage::Cancelled);
		return;
	}

	check(Preparation.CombinedDynamicMesh);
	check(!Preparation.Targets.IsEmpty());
	Stage.store(EJobStage::Baking);

	FDynamicMesh3& RayMesh = *Preparation.CombinedDynamicMesh;
	const FDynamicMeshAABBTree3 RaySpatial(&RayMesh, true);
	TArray<TArray64<FVector4f>> CoefficientImages;
	CoefficientImages.Reserve(Preparation.Targets.Num());

	for (const FBakeTargetPreparation& Target : Preparation.Targets)
	{
		check(Target.DynamicMesh);
		FDynamicMesh3& SurfaceMesh = *Target.DynamicMesh;
		const FDynamicMeshAABBTree3 SurfaceSpatial(&SurfaceMesh, true);
		FMeshBakerDynamicMeshSampler DetailSampler(
			&SurfaceMesh,
			&SurfaceSpatial);

		FMeshMapBaker Baker;
		Baker.SetTargetMesh(&SurfaceMesh);
		Baker.SetTargetMeshUVLayer(Preparation.Settings.BakeUVChannel);
		Baker.SetDetailSampler(&DetailSampler);
		Baker.SetCorrespondenceStrategy(
			FMeshBaseBaker::ECorrespondenceStrategy::Identity);
		Baker.SetDimensions(FImageDimensions(
			GetTextureResolution(
				Preparation.Settings.TextureResolution),
			GetTextureResolution(
				Preparation.Settings.TextureResolution)));
		Baker.SetSamplesPerPixel(Preparation.Settings.SamplesPerPixel);
		Baker.SetFilter(FMeshMapBaker::EBakeFilterType::BSpline);
		Baker.SetGutterEnabled(Preparation.Settings.PaddingSize > 0);
		Baker.SetGutterSize(Preparation.Settings.PaddingSize);
		Baker.CancelF = [this]()
		{
			return bCancelRequested.load(std::memory_order_relaxed)
				|| InvalidRayCount.load(std::memory_order_relaxed) > 0;
		};
		Baker.InteriorSampleCallback =
			[this](
				const bool bSampleValid,
				const FMeshMapEvaluator::FCorrespondenceSample& Sample,
				const FVector2d& UV,
				const FVector2i& ImageCoordinates)
			{
				if (bSampleValid)
				{
					ProcessedSurfaceSamples.fetch_add(
						1,
						std::memory_order_relaxed);
				}
			};

		const TSharedPtr<FSHThicknessMapEvaluator, ESPMode::ThreadSafe>
			Evaluator =
				MakeShared<FSHThicknessMapEvaluator, ESPMode::ThreadSafe>(
					SurfaceMesh,
					RayMesh,
					RaySpatial,
					Preparation.ThicknessScaleCm,
					Preparation.Settings.DirectionCount,
					Preparation.Settings.CoefficientSpace,
					bCancelRequested,
					InvalidRayCount);
		const int32 EvaluatorIndex = Baker.AddEvaluator(Evaluator);
		Baker.Bake();

		if (bCancelRequested.load(std::memory_order_relaxed))
		{
			Stage.store(EJobStage::Cancelled);
			return;
		}

		const int64 InvalidCount =
			InvalidRayCount.load(std::memory_order_relaxed);
		if (InvalidCount > 0)
		{
			Error = FString::Printf(
				TEXT("Unity-style farthest-hit thickness evaluation failed for %lld rays while baking %s. No changes were committed."),
				InvalidCount,
				*GetNameSafe(Target.SourceMesh.Get()));
			Stage.store(EJobStage::Failed);
			return;
		}

		const TArrayView<TUniquePtr<TImageBuilder<FVector4f>>> Results =
			Baker.GetBakeResults(EvaluatorIndex);
		if (Results.Num() != 1 || !Results[0])
		{
			Error = FString::Printf(
				TEXT("FMeshMapBaker returned no coefficient image for %s."),
				*GetNameSafe(Target.SourceMesh.Get()));
			Stage.store(EJobStage::Failed);
			return;
		}

		const TConstArrayView64<FVector4f> ImageBuffer =
			Results[0]->GetImageBuffer();
		TArray64<FVector4f>& CoefficientImage =
			CoefficientImages.AddDefaulted_GetRef();
		CoefficientImage.Append(ImageBuffer.GetData(), ImageBuffer.Num());
	}

	if (bCancelRequested.load(std::memory_order_relaxed))
	{
		Stage.store(EJobStage::Cancelled);
		return;
	}

	Stage.store(EJobStage::Encoding);
	if (!EncodeCoefficientImagesRGBA8(
		CoefficientImages,
		Preparation.Settings.bRemapCoefficientRange,
		EncodedRGBA))
	{
		Error = TEXT(
			"A filtered coefficient image contains a non-finite value.");
		Stage.store(EJobStage::Failed);
		return;
	}
	if (bCancelRequested.load(std::memory_order_relaxed))
	{
		EncodedRGBA.Reset();
		Stage.store(EJobStage::Cancelled);
		return;
	}
	Stage.store(EJobStage::Succeeded);
}

bool ApplyPreparedBakeUV(
	const FMeshDescription& PreparedMeshDescription,
	FMeshDescription& InOutTargetMeshDescription,
	const int32 BakeUVChannel,
	FText& OutError)
{
	if (ComputeMeshDescriptionTopologyHash(
			PreparedMeshDescription)
		!= ComputeMeshDescriptionTopologyHash(
			InOutTargetMeshDescription))
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"PreparedBakeUVTopologyMismatch",
			"The source mesh topology no longer matches the prepared Bake UV. No changes were committed.");
		return false;
	}

	const FStaticMeshConstAttributes PreparedAttributes(
		PreparedMeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> PreparedUVs =
		PreparedAttributes.GetVertexInstanceUVs();
	FStaticMeshAttributes TargetAttributes(
		InOutTargetMeshDescription);
	TVertexInstanceAttributesRef<FVector2f> TargetUVs =
		TargetAttributes.GetVertexInstanceUVs();
	if (!PreparedUVs.IsValid()
		|| PreparedUVs.GetNumChannels() <= BakeUVChannel
		|| !TargetUVs.IsValid()
		|| TargetUVs.GetNumChannels() < 1)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"PreparedBakeUVMissing",
			"The prepared or current source mesh no longer contains the selected UV data required by the bake.");
		return false;
	}

	if (TargetUVs.GetNumChannels() <= BakeUVChannel)
	{
		TargetUVs.SetNumChannels(BakeUVChannel + 1);
	}
	for (const FVertexInstanceID VertexInstanceID :
		InOutTargetMeshDescription.VertexInstances().GetElementIDs())
	{
		if (!PreparedMeshDescription.IsVertexInstanceValid(
			VertexInstanceID))
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"PreparedBakeUVVertexInstanceMismatch",
				"The prepared Bake UV does not contain every current source vertex instance.");
			return false;
		}
		TargetUVs.Set(
			VertexInstanceID,
			BakeUVChannel,
			PreparedUVs.Get(
				VertexInstanceID,
				BakeUVChannel));
	}

	if (!SynchronizeExistingSharedBakeUV(
		InOutTargetMeshDescription,
		BakeUVChannel,
		OutError))
	{
		return false;
	}
	return true;
}

static bool PrepareBakeTarget(
	UObject& SourceMesh,
	const FBakeSettings& Settings,
	FBakeTargetPreparation& OutTarget,
	FText& OutError)
{
	check(IsInGameThread());

	UStaticMesh* StaticMesh = Cast<UStaticMesh>(&SourceMesh);
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(&SourceMesh);
	if (StaticMesh == nullptr && SkeletalMesh == nullptr)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"UnsupportedSourceMeshType",
			"Source mesh must be a StaticMesh or SkeletalMesh.");
		return false;
	}
	if (Settings.CoefficientSpace != ECoefficientSpace::Tangent
		&& Settings.CoefficientSpace != ECoefficientSpace::Local)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidCoefficientSpace",
			"Bake space must be Tangent Space or Local Space.");
		return false;
	}
	if (SkeletalMesh != nullptr
		&& Settings.CoefficientSpace != ECoefficientSpace::Tangent)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"SkeletalMeshLocalSpaceUnsupported",
			"SkeletalMesh currently supports Tangent Space only. Local Space is available for StaticMesh assets.");
		return false;
	}
	if (Settings.BakeUVChannel != 0 && Settings.BakeUVChannel != 1)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidBakeUVChannel",
			"Bake UV channel must be UV0 or UV1.");
		return false;
	}
	switch (Settings.TextureResolution)
	{
	case ETextureResolution::Size256:
	case ETextureResolution::Size512:
	case ETextureResolution::Size1024:
	case ETextureResolution::Size2048:
		break;
	default:
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidResolution",
			"Texture resolution must be 256, 512, 1024, or 2048.");
		return false;
	}
	const int32 TextureResolution =
		GetTextureResolution(Settings.TextureResolution);
	if (Settings.DirectionCount < 8
		|| Settings.DirectionCount > 256
		|| (Settings.DirectionCount & 1) != 0)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidDirectionCount",
			"Direction count must be an even number between 8 and 256.");
		return false;
	}
	const int32 SampleGridDimension =
		FMath::RoundToInt(FMath::Sqrt(static_cast<float>(Settings.SamplesPerPixel)));
	if (Settings.SamplesPerPixel < 1
		|| Settings.SamplesPerPixel > 16
		|| SampleGridDimension * SampleGridDimension != Settings.SamplesPerPixel)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidSamplesPerPixel",
			"Samples per pixel must be one of 1, 4, 9, or 16.");
		return false;
	}
	if (Settings.PaddingSize < 0 || Settings.PaddingSize > 64)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"InvalidPadding",
			"Padding size must be between 0 and 64 texels.");
		return false;
	}
	if (Settings.PaddingSize * 2 >= TextureResolution)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"PaddingTooLargeForResolution",
			"Padding must be less than half of the texture resolution.");
		return false;
	}
	const int64 EstimatedRayCount =
		static_cast<int64>(TextureResolution)
		* TextureResolution
		* Settings.SamplesPerPixel
		* Settings.DirectionCount;
	if (Settings.bRegenerateBakeUV
		&& SourceMesh.GetPathName().StartsWith(TEXT("/Engine/")))
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"EngineAssetReadOnly",
			"Texture Baker cannot regenerate UVs directly on a built-in /Engine mesh asset. Disable UV regeneration to bake it read-only.");
		return false;
	}

	if (StaticMesh != nullptr)
	{
		if (!StaticMesh->IsSourceModelValid(0)
			|| !StaticMesh->IsMeshDescriptionValid(0))
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"MissingStaticLOD0Source",
				"The selected StaticMesh has no editable LOD0 MeshDescription.");
			return false;
		}
		if (StaticMesh->IsReductionActive(0))
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"StaticLOD0ReductionActive",
				"Texture Baker currently supports only unreduced StaticMesh source LOD0 geometry. Disable LOD0 reduction before baking.");
			return false;
		}
		if (StaticMesh->IsNaniteEnabled())
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"StaticNaniteUnsupported",
				"Texture Baker currently supports the standard non-Nanite StaticMesh LOD0 render path only.");
			return false;
		}
	}
	else
	{
		const FSkeletalMeshLODInfo* LODInfo =
			SkeletalMesh->GetLODInfo(0);
		if (!SkeletalMesh->IsSourceModelValid(0)
			|| !SkeletalMesh->HasMeshDescription(0)
			|| LODInfo == nullptr)
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"MissingSkeletalLOD0Source",
				"The selected SkeletalMesh has no editable LOD0 MeshDescription.");
			return false;
		}
		if (LODInfo->bHasBeenSimplified)
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"SkeletalLOD0ReductionActive",
				"Texture Baker currently supports only unreduced SkeletalMesh source LOD0 geometry.");
			return false;
		}
		if (SkeletalMesh->IsNaniteEnabled())
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"SkeletalNaniteUnsupported",
				"Texture Baker currently supports the standard non-Nanite SkeletalMesh LOD0 render path only.");
			return false;
		}
	}

	FMeshDescription CommitMeshDescription;
	const bool bCloneSucceeded = StaticMesh != nullptr
		? StaticMesh->CloneMeshDescription(0, CommitMeshDescription)
		: SkeletalMesh->CloneMeshDescription(0, CommitMeshDescription);
	if (!bCloneSucceeded)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"CloneLOD0Failed",
			"Failed to clone the selected mesh LOD0.");
		return false;
	}

	FMeshBuildSettings StaticBuildSettings;
	FSkeletalMeshBuildSettings SkeletalBuildSettings;
	FVector BuildScale = FVector::OneVector;
	bool bUseLegacyTangentScaling = false;
	if (StaticMesh != nullptr)
	{
		StaticBuildSettings =
			StaticMesh->GetSourceModel(0).BuildSettings;
		BuildScale = StaticBuildSettings.BuildScale3D;
		bUseLegacyTangentScaling =
			StaticMesh->GetLegacyTangentScaling();
	}
	else
	{
		SkeletalBuildSettings =
			SkeletalMesh->GetLODInfo(0)->BuildSettings;
	}
	if (!FMath::IsFinite(BuildScale.X)
		|| !FMath::IsFinite(BuildScale.Y)
		|| !FMath::IsFinite(BuildScale.Z)
		|| FMath::IsNearlyZero(BuildScale.X)
		|| FMath::IsNearlyZero(BuildScale.Y)
		|| FMath::IsNearlyZero(BuildScale.Z))
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"ZeroBuildScale",
			"LOD0 Build Scale contains a non-finite or zero axis.");
		return false;
	}

	int32 DegenerateSourceTriangleCount = 0;
	int32 DuplicateSourceTriangleCount = 0;
	if (!InspectSourceTrianglesForBake(
		CommitMeshDescription,
		BuildScale,
		DegenerateSourceTriangleCount,
		DuplicateSourceTriangleCount,
		OutError))
	{
		return false;
	}

	const FStaticMeshConstAttributes ConstAttributes(CommitMeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> ExistingUVs =
		ConstAttributes.GetVertexInstanceUVs();
	const int32 UVChannelCount = ExistingUVs.IsValid() ? ExistingUVs.GetNumChannels() : 0;
	const int32 BakeUVChannel = Settings.BakeUVChannel;
	const FText BakeUVText = GetUVChannelText(BakeUVChannel);
	if (UVChannelCount < 1)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"UV0Missing",
			"Texture Baker currently requires UV0 because Unreal's render tangent build may read it.");
		return false;
	}
	const bool bHasBakeUV = UVChannelCount > BakeUVChannel;
	if (!Settings.bRegenerateBakeUV && !bHasBakeUV)
	{
		OutError = FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"ExistingBakeUVMissing",
				"{0} does not exist. Enable XAtlas regeneration to add it, or select an existing channel."),
			BakeUVText);
		return false;
	}

	bool bDisableLightmapGeneration = false;
	if (StaticMesh != nullptr)
	{
		const int32 FinalUVChannelCount =
			Settings.bRegenerateBakeUV && !bHasBakeUV
				? BakeUVChannel + 1
				: UVChannelCount;
		// SetupRenderMeshDescription falls back to UV0 when the configured
		// lightmap source is outside the final UV channel range.
		const int32 EffectiveLightmapSourceUV =
			StaticBuildSettings.SrcLightmapIndex
					>= FinalUVChannelCount
				? 0
				: StaticBuildSettings.SrcLightmapIndex;
		if (!Settings.bRegenerateBakeUV
			&& StaticBuildSettings.bGenerateLightmapUVs
			&& StaticBuildSettings.DstLightmapIndex
				== BakeUVChannel)
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"ReusedBakeUVOverwrittenByLightmapBuild",
					"LOD0 automatic lightmap UV generation writes to {0}, so the existing source layout is not the final rendered layout. Disable automatic lightmap UV generation or enable XAtlas regeneration."),
				BakeUVText);
			return false;
		}
		if (Settings.bRegenerateBakeUV
			&& StaticBuildSettings.bGenerateLightmapUVs
			&& EffectiveLightmapSourceUV == BakeUVChannel
			&& StaticBuildSettings.DstLightmapIndex
				!= BakeUVChannel)
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"BakeUVFeedsGeneratedLightmapUV",
					"LOD0 automatic lightmap UV generation uses {0} as its source for another channel. Regenerating {0} would also change that generated channel. Choose another lightmap source, disable automatic generation, or reuse the existing Bake UV."),
				BakeUVText);
			return false;
		}
		if (Settings.bRegenerateBakeUV
			&& StaticBuildSettings.bGenerateLightmapUVs
			&& UVChannelCount == BakeUVChannel
			&& StaticBuildSettings.DstLightmapIndex
				> BakeUVChannel)
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"AddingUV1ChangesGeneratedLightmapDestination",
				"LOD0 currently contains only UV0 while automatic lightmap UV generation requests a destination above UV1. Adding UV1 would change which empty channel Unreal selects as the generated lightmap destination. Set the lightmap destination to UV1 or disable automatic lightmap UV generation before baking.");
			return false;
		}
		bDisableLightmapGeneration =
			Settings.bRegenerateBakeUV
			&& StaticBuildSettings.bGenerateLightmapUVs
			&& StaticBuildSettings.DstLightmapIndex
				== BakeUVChannel;
	}

	const FSHAHash SourceMeshDescriptionHash =
		FStaticMeshOperations::ComputeSHAHash(CommitMeshDescription, true);
	const FSHAHash SourceMeshTopologyHash =
		ComputeMeshDescriptionTopologyHash(CommitMeshDescription);

	OutTarget = FBakeTargetPreparation();
	OutTarget.SourceMesh = &SourceMesh;
	OutTarget.bSourceIsSkeletalMesh =
		SkeletalMesh != nullptr;
	OutTarget.SourceMeshDescriptionHash = SourceMeshDescriptionHash;
	OutTarget.SourceMeshTopologyHash = SourceMeshTopologyHash;
	OutTarget.SourceBuildSettings = StaticBuildSettings;
	OutTarget.SourceSkeletalBuildSettings =
		SkeletalBuildSettings;
	OutTarget.bDisableLightmapGeneration =
		bDisableLightmapGeneration;
	OutTarget.bUseLegacyTangentScaling = bUseLegacyTangentScaling;
	if (EstimatedRayCount > 50000000)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"LargeRayBudgetWarning",
				"This bake requests up to approximately {0} thickness rays before accounting for empty UV texels and may take a long time."),
			FText::AsNumber(EstimatedRayCount)));
	}
	if (DegenerateSourceTriangleCount > 0)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"DegenerateSourceTriangleWarning",
				"LOD0 contains {0} zero-area source triangles. They do not contribute to ray intersections or UV validation."),
			FText::AsNumber(DegenerateSourceTriangleCount)));
	}
	if (DuplicateSourceTriangleCount > 0)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"DuplicateSourceTriangleWarning",
				"LOD0 contains {0} duplicate source triangles. The working DynamicMesh keeps one copy because duplicates do not change farthest-hit distance; duplicate faces using separate UV regions are not guaranteed to receive independent texture coverage."),
			FText::AsNumber(DuplicateSourceTriangleCount)));
	}
	if (Settings.PaddingSize == 0)
	{
		OutTarget.Warnings.Add(NSLOCTEXT(
			"SHThicknessBaker",
			"ZeroPaddingWarning",
			"Padding is zero. BC7 blocks and mip filtering can mix coefficient texels with unrelated charts or the zero background."));
	}
	const int32 SourceModelCount = StaticMesh != nullptr
		? StaticMesh->GetNumSourceModels()
		: SkeletalMesh->GetLODNum();
	if (SourceModelCount > 1)
	{
		OutTarget.Warnings.Add(NSLOCTEXT(
			"SHThicknessBaker",
			"AdditionalLODsWarning",
			"This mesh has additional LODs. Texture Baker uses or replaces the selected UV on LOD0 only; the generated texture and UV contract are valid for LOD0 only."));
	}
	if (SkeletalMesh != nullptr)
	{
		OutTarget.Warnings.Add(NSLOCTEXT(
			"SHThicknessBaker",
			"SkeletalReferencePoseWarning",
			"SkeletalMesh thickness is baked from the LOD0 reference pose. Tangent-space directions follow the skinned tangent basis at runtime, but the baked thickness values do not change as joints bend."));
	}
	if (Settings.bRegenerateBakeUV)
	{
		if (bHasBakeUV)
		{
			OutTarget.Warnings.Add(FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"ReplaceExistingBakeUVWarning",
					"{0} already exists. After a successful bake, XAtlas will replace that channel directly on the selected source mesh."),
				BakeUVText));
		}
		else
		{
			OutTarget.Warnings.Add(FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"AddBakeUVWarning",
					"{0} is missing. After a successful bake, XAtlas will add that channel directly to the selected source mesh."),
				BakeUVText));
		}
		if (BakeUVChannel == 0)
		{
			OutTarget.Warnings.Add(NSLOCTEXT(
				"SHThicknessBaker",
				"ReplaceUV0MaterialWarning",
				"Replacing UV0 changes the coordinates normally used by Base Color, Normal, and other material textures. It can also change the final MikkTSpace tangent basis."));
		}
	}
	else
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"ReuseExistingBakeUVWarning",
				"The existing {0} layout will be used and that UV channel will not be modified. Padding dilates the output texture but cannot add spacing between existing UV charts."),
			BakeUVText));
	}
	if (bDisableLightmapGeneration)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"DisableLightmapGenerationWarning",
				"LOD0 automatic lightmap UV generation currently writes to {0}. Texture Baker will disable that setting so it cannot overwrite the XAtlas result."),
			BakeUVText));
	}

	if (StaticMesh != nullptr
		&& StaticMesh->GetLightMapCoordinateIndex()
			== BakeUVChannel)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"LightmapCoordinateWarning",
				"The StaticMesh Light Map Coordinate Index selects {0}. That channel will be shared with SH thickness data; verify that this is intentional."),
			BakeUVText));
	}

	int32 SubTexelOverlapCount = 0;
	if (Settings.bRegenerateBakeUV)
	{
		if (!GenerateBakeUV(
				CommitMeshDescription,
				BuildScale,
				Settings,
				BakeUVChannel,
				OutError)
			|| !SynchronizeExistingSharedBakeUV(
				CommitMeshDescription,
				BakeUVChannel,
				OutError))
		{
			return false;
		}
	}

	// Rebuild the final render normal/basis from the exact UV state that will
	// be committed. Tangent Space needs the full TBN; Local Space needs only
	// the render normal used to offset the ray origin.
	FMeshDescription RenderMeshDescription(
		CommitMeshDescription);
	const bool bRequireTangents =
		Settings.CoefficientSpace == ECoefficientSpace::Tangent;
	const bool bPreparedRenderBasis = StaticMesh != nullptr
		? PrepareRenderTangentBasis(
			*StaticMesh,
			StaticBuildSettings,
			RenderMeshDescription,
			bRequireTangents,
			OutError)
		: PrepareRenderTangentBasis(
			*SkeletalMesh,
			SkeletalBuildSettings,
			RenderMeshDescription,
			OutError);
	if (!bPreparedRenderBasis
		|| !ValidateBakeUV(
			RenderMeshDescription,
			BakeUVChannel,
			TextureResolution,
			&SubTexelOverlapCount,
			OutError))
	{
		return false;
	}
	if (SubTexelOverlapCount > 0)
	{
		OutTarget.Warnings.Add(FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"SubTexelUVOverlapWarning",
				"{0} contains {1} triangle pairs with overlap smaller than one quarter of a mip-0 texel. The bake remains allowed, but inspect the result if those slivers are visible."),
			BakeUVText,
			FText::AsNumber(SubTexelOverlapCount)));
	}

	// MeshDescription may also carry legacy/shared UV element channels.
	// FMeshDescriptionToDynamicMesh prefers those channels globally when they
	// are populated. Remove only the transient shared representation so every
	// DynamicMesh UV layer is copied from render vertex-instance attributes.
	RenderMeshDescription.SetNumUVChannels(0);

	TUniquePtr<FDynamicMesh3> BakeMesh;
	double BakeDiagonal = 0.0;
	if (!ConvertMeshDescription(
		RenderMeshDescription,
		BuildScale,
		bUseLegacyTangentScaling,
		bRequireTangents,
		BakeUVChannel,
		BakeMesh,
		BakeDiagonal,
		OutError))
	{
		return false;
	}
	(void)BakeDiagonal;
	if (!BakeMesh->HasAttributes()
		|| BakeMesh->Attributes()->NumUVLayers() <= BakeUVChannel
		|| BakeMesh->Attributes()->GetUVLayer(BakeUVChannel) == nullptr)
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"DynamicMeshBakeUVMissing",
			"The selected Bake UV was lost while converting LOD0 to the bake mesh.");
		return false;
	}
	int32 UncoveredTangentSeamCount = 0;
	if (bRequireTangents
		&& !ValidateTangentSpaceBakeMesh(
			*BakeMesh,
			BakeUVChannel,
			UncoveredTangentSeamCount,
			OutError))
	{
		return false;
	}
	if (!BakeMesh->IsClosed())
	{
		OutTarget.Warnings.Add(NSLOCTEXT(
			"SHThicknessBaker",
			"OpenOrSplitTopologyWarning",
			"LOD0 is open or contains non-manifold splits. The Unity-style farthest-hit kernel accepts this geometry; directions with no qualifying hit bake as zero."));
	}
	if (UncoveredTangentSeamCount > 0)
	{
		const FText SeamWarning = Settings.bRegenerateBakeUV
			? FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"GeneratedUVCrossesTangentSeamWarning",
					"Generated {0} remains continuous across {1} materially different render tangent-basis seam edges. UE's XAtlas wrapper cannot force those seams; filtering can produce local artifacts near them, but the bake remains valid elsewhere."),
				BakeUVText,
				FText::AsNumber(UncoveredTangentSeamCount))
			: FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"ExistingUVCrossesTangentSeamWarning",
					"Existing {0} remains continuous across {1} materially different render tangent-basis seam edges. Filtering can mix coefficients expressed in different tangent frames near those edges, but the bake remains allowed."),
				BakeUVText,
				FText::AsNumber(UncoveredTangentSeamCount));
		OutTarget.Warnings.Add(SeamWarning);
	}

	OutTarget.MeshDescription = MoveTemp(CommitMeshDescription);
	OutTarget.DynamicMesh = MoveTemp(BakeMesh);
	return true;
}

bool PrepareBake(
	const TConstArrayView<UObject*> SourceMeshes,
	const FBakeSettings& Settings,
	FBakePreparation& OutPreparation,
	FText& OutError)
{
	check(IsInGameThread());

	if (SourceMeshes.IsEmpty())
	{
		OutError = NSLOCTEXT(
			"SHThicknessBaker",
			"NoSourceMeshes",
			"Add at least one StaticMesh or SkeletalMesh source.");
		return false;
	}

	FBakePreparation Preparation;
	Preparation.Settings = Settings;
	Preparation.Targets.Reserve(SourceMeshes.Num());

	TSet<UObject*> UniqueSources;
	for (UObject* SourceMesh : SourceMeshes)
	{
		if (SourceMesh == nullptr)
		{
			OutError = NSLOCTEXT(
				"SHThicknessBaker",
				"NullSourceMesh",
				"Every source slot must contain a StaticMesh or SkeletalMesh.");
			return false;
		}
		if (UniqueSources.Contains(SourceMesh))
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"DuplicateSourceMesh",
					"Source mesh {0} is listed more than once. Each source asset can produce only one output texture per group bake."),
				FText::FromString(SourceMesh->GetName()));
			return false;
		}
		UniqueSources.Add(SourceMesh);

		FBakeTargetPreparation& Target =
			Preparation.Targets.AddDefaulted_GetRef();
		if (!PrepareBakeTarget(
				*SourceMesh,
				Settings,
				Target,
				OutError))
		{
			OutError = FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"PrepareTargetFailed",
					"{0}: {1}"),
				FText::FromString(SourceMesh->GetName()),
				OutError);
			return false;
		}

		for (const FText& Warning : Target.Warnings)
		{
			Preparation.Warnings.Add(FText::Format(
				NSLOCTEXT(
					"SHThicknessBaker",
					"TargetWarning",
					"{0}: {1}"),
				FText::FromString(SourceMesh->GetName()),
				Warning));
		}
	}

	for (int32 TargetIndex = 0;
		TargetIndex < Preparation.Targets.Num();
		++TargetIndex)
	{
		FBakeTargetPreparation& Target =
			Preparation.Targets[TargetIndex];
		check(Target.DynamicMesh);
		Target.CombinedTriangleIDs.Init(
			INDEX_NONE,
			Target.DynamicMesh->MaxTriangleID());

		if (TargetIndex == 0)
		{
			Preparation.CombinedDynamicMesh =
				MakeUnique<FDynamicMesh3>(*Target.DynamicMesh);
			for (const int32 TriangleID :
				Target.DynamicMesh->TriangleIndicesItr())
			{
				check(
					Preparation.CombinedDynamicMesh->IsTriangle(
						TriangleID));
				Target.CombinedTriangleIDs[TriangleID] = TriangleID;
			}
			continue;
		}

		FMeshIndexMappings IndexMappings;
		FDynamicMeshEditor Editor(
			Preparation.CombinedDynamicMesh.Get());
		Editor.AppendMesh(
			Target.DynamicMesh.Get(),
			IndexMappings);
		for (const int32 TriangleID :
			Target.DynamicMesh->TriangleIndicesItr())
		{
			const int32 CombinedTriangleID =
				IndexMappings.GetNewTriangle(TriangleID);
			if (!Preparation.CombinedDynamicMesh->IsTriangle(
					CombinedTriangleID))
			{
				OutError = FText::Format(
					NSLOCTEXT(
						"SHThicknessBaker",
						"CombinedTriangleMappingFailed",
						"Failed to combine source mesh {0} into the shared ray geometry."),
					FText::FromString(
						GetNameSafe(Target.SourceMesh.Get())));
				return false;
			}
			Target.CombinedTriangleIDs[TriangleID] =
				CombinedTriangleID;
		}
	}

	check(Preparation.CombinedDynamicMesh);
	double CombinedDiagonal = 0.0;
	if (!ValidateRayTraceGeometry(
			*Preparation.CombinedDynamicMesh,
			CombinedDiagonal,
			OutError))
	{
		OutError = FText::Format(
			NSLOCTEXT(
				"SHThicknessBaker",
				"CombinedGeometryInvalid",
				"Combined source geometry is invalid: {0}"),
			OutError);
		return false;
	}
	Preparation.ThicknessScaleCm =
		CombinedDiagonal + UnityCameraNormalOffsetCm;

	if (Preparation.Targets.Num() > 1)
	{
		Preparation.Warnings.Insert(
			NSLOCTEXT(
				"SHThicknessBaker",
				"IdentityLocalGroupWarning",
				"All source meshes are combined using their authored asset-local positions with identity transforms. The group shares one bounds normalization scale and, when enabled, one coefficient Remap gain; each source still writes its own texture."),
			0);
	}

	OutPreparation = MoveTemp(Preparation);
	return true;
}

bool EncodeCoefficientImagesRGBA8(
	const TArray<TArray64<FVector4f>>& CoefficientImages,
	const bool bRemapCoefficientRange,
	TArray<TArray64<uint8>>& OutRGBAImages)
{
	float CoefficientGain = 1.0f;
	float MaximumDirectionalCoefficient = 0.0f;
	float MaximumConstantCoefficient = 0.0f;
	for (const TArray64<FVector4f>& Coefficients : CoefficientImages)
	{
		for (const FVector4f& Coefficient : Coefficients)
		{
			if (!FMath::IsFinite(Coefficient.X)
				|| !FMath::IsFinite(Coefficient.Y)
				|| !FMath::IsFinite(Coefficient.Z)
				|| !FMath::IsFinite(Coefficient.W))
			{
				OutRGBAImages.Reset();
				return false;
			}

			if (bRemapCoefficientRange)
			{
				MaximumDirectionalCoefficient = FMath::Max(
					MaximumDirectionalCoefficient,
					FMath::Max3(
						FMath::Abs(Coefficient.X),
						FMath::Abs(Coefficient.Y),
						FMath::Abs(Coefficient.Z)));
				MaximumConstantCoefficient = FMath::Max(
					MaximumConstantCoefficient,
					Coefficient.W);
			}
		}
	}

	if (bRemapCoefficientRange
		&& (MaximumDirectionalCoefficient > UE_SMALL_NUMBER
			|| MaximumConstantCoefficient > UE_SMALL_NUMBER))
	{
		// One gain preserves C0 + dot(Cxyz, D) up to a uniform scale.
		constexpr float DirectionalTarget = 0.8f;
		constexpr float ConstantTarget = 0.9f;
		if (MaximumDirectionalCoefficient > UE_SMALL_NUMBER
			&& MaximumConstantCoefficient > UE_SMALL_NUMBER)
		{
			CoefficientGain = FMath::Min(
				DirectionalTarget / MaximumDirectionalCoefficient,
				ConstantTarget / MaximumConstantCoefficient);
		}
		else if (MaximumDirectionalCoefficient > UE_SMALL_NUMBER)
		{
			CoefficientGain =
				DirectionalTarget / MaximumDirectionalCoefficient;
		}
		else
		{
			CoefficientGain =
				ConstantTarget / MaximumConstantCoefficient;
		}
	}

	OutRGBAImages.Reset(CoefficientImages.Num());
	for (const TArray64<FVector4f>& Coefficients : CoefficientImages)
	{
		TArray64<uint8>& OutRGBA =
			OutRGBAImages.AddDefaulted_GetRef();
		OutRGBA.SetNumUninitialized(Coefficients.Num() * 4);

		for (int64 PixelIndex = 0;
			PixelIndex < Coefficients.Num();
			++PixelIndex)
		{
			const FVector4f Coefficient =
				Coefficients[PixelIndex] * CoefficientGain;
			const auto EncodeSigned = [](const float Value)
			{
				return static_cast<uint8>(FMath::Clamp(
					FMath::RoundToInt(
						(Value * 0.5f + 0.5f) * 255.0f),
					0,
					255));
			};

			OutRGBA[PixelIndex * 4 + 0] =
				EncodeSigned(Coefficient.X);
			OutRGBA[PixelIndex * 4 + 1] =
				EncodeSigned(Coefficient.Y);
			OutRGBA[PixelIndex * 4 + 2] =
				EncodeSigned(Coefficient.Z);
			OutRGBA[PixelIndex * 4 + 3] =
				static_cast<uint8>(FMath::Clamp(
				FMath::RoundToInt(Coefficient.W * 255.0f),
				0,
				255));
		}
	}
	return true;
}

FSHAHash ComputeMeshDescriptionTopologyHash(
	const FMeshDescription& MeshDescription)
{
	FSHA1 Hasher;
	const int32 VertexInstanceCount = MeshDescription.VertexInstances().Num();
	const int32 TriangleCount = MeshDescription.Triangles().Num();
	Hasher.Update(VertexInstanceCount);
	Hasher.Update(TriangleCount);

	for (const FVertexInstanceID VertexInstanceID :
		MeshDescription.VertexInstances().GetElementIDs())
	{
		const int32 VertexInstanceValue = VertexInstanceID.GetValue();
		const int32 VertexValue =
			MeshDescription.GetVertexInstanceVertex(VertexInstanceID).GetValue();
		Hasher.Update(VertexInstanceValue);
		Hasher.Update(VertexValue);
	}

	for (const FTriangleID TriangleID :
		MeshDescription.Triangles().GetElementIDs())
	{
		const int32 TriangleValue = TriangleID.GetValue();
		Hasher.Update(TriangleValue);
		const TArrayView<const FVertexInstanceID> VertexInstances =
			MeshDescription.GetTriangleVertexInstances(TriangleID);
		for (const FVertexInstanceID VertexInstanceID : VertexInstances)
		{
			const int32 VertexInstanceValue = VertexInstanceID.GetValue();
			Hasher.Update(VertexInstanceValue);
		}
	}

	return Hasher.Finalize();
}

} // namespace SHThicknessBaker
