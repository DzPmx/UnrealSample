#include "FoliageBakerProjectedMaterialBake.h"

#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace UE::FoliageBaker::ProjectedMaterialBake
{
	namespace
	{
		bool ComputeBarycentric3D(
			const PlaneCover::FSourceTriangle& Triangle,
			const FVector& Point,
			double& OutA,
			double& OutB,
			double& OutC)
		{
			const FVector V0 = Triangle.Vertices[1] - Triangle.Vertices[0];
			const FVector V1 = Triangle.Vertices[2] - Triangle.Vertices[0];
			const FVector V2 = Point - Triangle.Vertices[0];
			const double D00 = FVector::DotProduct(V0, V0);
			const double D01 = FVector::DotProduct(V0, V1);
			const double D11 = FVector::DotProduct(V1, V1);
			const double D20 = FVector::DotProduct(V2, V0);
			const double D21 = FVector::DotProduct(V2, V1);
			const double Denominator = D00 * D11 - D01 * D01;
			if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}

			OutB = (D11 * D20 - D01 * D21) / Denominator;
			OutC = (D00 * D21 - D01 * D20) / Denominator;
			OutA = 1.0 - OutB - OutC;
			return true;
		}

		struct FVertexBasis
		{
			FVector Normal = FVector::UpVector;
			FVector Tangent = FVector::ForwardVector;
			float BinormalSign = 1.0f;
		};

		FVector DeriveTangentForNormal(const FVector& InNormal)
		{
			const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			const FVector ReferenceAxis = FMath::Abs(Normal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
			FVector Tangent = FVector::CrossProduct(ReferenceAxis, Normal).GetSafeNormal();
			if (Tangent.IsNearlyZero())
			{
				Tangent = FVector::RightVector;
			}
			return Tangent;
		}

		FVertexBasis MakeVertexBasis(
			const PlaneCover::FSourceTriangle& Triangle,
			const double W0,
			const double W1,
			const double W2)
		{
			FVertexBasis Result;
			FVector Normal = Triangle.VertexNormals[0] * W0
				+ Triangle.VertexNormals[1] * W1
				+ Triangle.VertexNormals[2] * W2;
			if (!Normal.Normalize())
			{
				Normal = Triangle.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			}

			FVector Tangent = FVector::ForwardVector;
			float BinormalSign = 1.0f;
			if (Triangle.bHasTangents)
			{
				Tangent = Triangle.VertexTangents[0] * W0
					+ Triangle.VertexTangents[1] * W1
					+ Triangle.VertexTangents[2] * W2;
				BinormalSign = (Triangle.BinormalSigns[0] * W0
					+ Triangle.BinormalSigns[1] * W1
					+ Triangle.BinormalSigns[2] * W2) < 0.0 ? -1.0f : 1.0f;
			}
			else
			{
				Tangent = DeriveTangentForNormal(Normal);
			}

			Tangent = Tangent - Normal * FVector::DotProduct(Tangent, Normal);
			if (!Tangent.Normalize())
			{
				Tangent = DeriveTangentForNormal(Normal);
			}

			Result.Normal = Normal;
			Result.Tangent = Tangent;
			Result.BinormalSign = BinormalSign;
			return Result;
		}

		int32 GetSourceMeshMaxUVChannelCount(const TArray<PlaneCover::FSourceTriangle>& Triangles)
		{
			int32 ChannelCount = 1;
			for (const PlaneCover::FSourceTriangle& Triangle : Triangles)
			{
				ChannelCount = FMath::Max(ChannelCount, Triangle.NumUVChannels);
			}
			return FMath::Clamp(ChannelCount, 1, PlaneCover::MaxMaterialBakeUVChannels);
		}

		double ComputeTriangleCaptureDepth(
			const PlaneCover::FSourceTriangle& Triangle,
			const FVector& CaptureRayDirection)
		{
			const FVector Center = (Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
			return FVector::DotProduct(Center, CaptureRayDirection);
		}

		double ComputeProjectionCaptureDepth(
			const PlaneCover::FCrackReductionProjection& Projection,
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const FVector& CaptureRayDirection)
		{
			if (!Projection.ClippedPolygon.IsEmpty())
			{
				FVector Center = FVector::ZeroVector;
				for (const FVector& Point : Projection.ClippedPolygon)
				{
					Center += Point;
				}
				Center /= static_cast<double>(Projection.ClippedPolygon.Num());
				return FVector::DotProduct(Center, CaptureRayDirection);
			}

			return Triangles.IsValidIndex(Projection.TriangleIndex)
				? ComputeTriangleCaptureDepth(Triangles[Projection.TriangleIndex], CaptureRayDirection)
				: TNumericLimits<double>::Lowest();
		}

		void SortBakeFragmentsFarToNear(
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const FVector& CaptureRayDirection,
			TArray<int32>& TriangleIndices,
			TArray<PlaneCover::FCrackReductionProjection>& CrackReductionProjections)
		{
			TriangleIndices.Sort(
				[&Triangles, &CaptureRayDirection](const int32 A, const int32 B)
				{
					const double DepthA = Triangles.IsValidIndex(A)
						? ComputeTriangleCaptureDepth(Triangles[A], CaptureRayDirection)
						: TNumericLimits<double>::Lowest();
					const double DepthB = Triangles.IsValidIndex(B)
						? ComputeTriangleCaptureDepth(Triangles[B], CaptureRayDirection)
						: TNumericLimits<double>::Lowest();
					if (!FMath::IsNearlyEqual(DepthA, DepthB, 1.0e-6))
					{
						return DepthA > DepthB;
					}
					return A < B;
				});

			CrackReductionProjections.Sort(
				[&Triangles, &CaptureRayDirection](
					const PlaneCover::FCrackReductionProjection& A,
					const PlaneCover::FCrackReductionProjection& B)
				{
					const double DepthA = ComputeProjectionCaptureDepth(A, Triangles, CaptureRayDirection);
					const double DepthB = ComputeProjectionCaptureDepth(B, Triangles, CaptureRayDirection);
					if (!FMath::IsNearlyEqual(DepthA, DepthB, 1.0e-6))
					{
						return DepthA > DepthB;
					}
					return A.TriangleIndex < B.TriangleIndex;
				});
		}

		bool BuildBakeMeshDescription(
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const TArray<PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FPlaneSideBakeParams& Params,
			const bool bEffectiveReverseWinding,
			FMeshDescription& OutMeshDescription,
			TArray<FVector2D>& OutCustomTextureCoordinates,
			int32& OutMatchingTriangleCount,
			TArray<int32>& OutRasterSourceTriangleIndices)
		{
			OutMatchingTriangleCount = 0;
			OutCustomTextureCoordinates.Reset();
			OutRasterSourceTriangleIndices.Reset();
			OutMeshDescription.Empty();
			FStaticMeshAttributes(OutMeshDescription).Register();

			const int32 DesiredUVChannels = FMath::Clamp(
				Params.NumSourceUVChannels > 0
					? Params.NumSourceUVChannels
					: GetSourceMeshMaxUVChannelCount(Triangles),
				1,
				PlaneCover::MaxMaterialBakeUVChannels);
			const bool bFlipTextureV = Params.AtlasVConvention == PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;

			FStaticMeshAttributes Attributes(OutMeshDescription);
			TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
			TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
			TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			VertexInstanceUVs.SetNumChannels(DesiredUVChannels);

			const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
			PolygonGroupMaterialSlotNames[PolygonGroupID] = FName(TEXT("BillboardBakeSlot"));
			const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
			const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);

			auto AppendTriangleGeometry = [
				bEffectiveReverseWinding,
				bFlipTextureV,
				DesiredUVChannels,
				&OutCustomTextureCoordinates,
				&OutMeshDescription,
				&OutRasterSourceTriangleIndices,
				&Params,
				&PlaneInfo,
				PolygonGroupID,
				UExtent,
				VExtent,
				&VertexInstanceBinormalSigns,
				&VertexInstanceColors,
				&VertexInstanceNormals,
				&VertexInstanceTangents,
				&VertexInstanceUVs,
				&VertexPositions](
				const int32 SourceTriangleIndex,
				const PlaneCover::FSourceTriangle& Triangle,
				const FVector Positions[3]) -> bool
			{
				if (Params.MaterialIndexFilter.IsSet()
					&& Triangle.MaterialIndex != Params.MaterialIndexFilter.GetValue())
				{
					return false;
				}
				if (Triangle.Area <= 0.0
					|| FVector::CrossProduct(Positions[1] - Positions[0], Positions[2] - Positions[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
				{
					return false;
				}

				double Weights[3][3] = {};
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					if (!ComputeBarycentric3D(
							Triangle,
							Positions[Corner],
							Weights[Corner][0],
							Weights[Corner][1],
							Weights[Corner][2]))
					{
						return false;
					}
				}

				FVertexInstanceID VertexInstanceIDs[3];
				FVector2D CustomUVs[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const double W0 = Weights[Corner][0];
					const double W1 = Weights[Corner][1];
					const double W2 = Weights[Corner][2];
					const float W0f = static_cast<float>(W0);
					const float W1f = static_cast<float>(W1);
					const float W2f = static_cast<float>(W2);
					const FVertexBasis Basis = MakeVertexBasis(Triangle, W0, W1, W2);

					const FVertexID VertexID = OutMeshDescription.CreateVertex();
					VertexPositions[VertexID] = FVector3f(Positions[Corner]);
					VertexInstanceIDs[Corner] = OutMeshDescription.CreateVertexInstance(VertexID);

					const FVector2f FallbackUV = Triangle.bHasUVs
						? Triangle.UVs[0] * W0f + Triangle.UVs[1] * W1f + Triangle.UVs[2] * W2f
						: FVector2f::ZeroVector;
					for (int32 UVChannel = 0; UVChannel < DesiredUVChannels; ++UVChannel)
					{
						const FVector2f SourceUV = (Triangle.bHasUVs && UVChannel < Triangle.NumUVChannels)
							? Triangle.UVChannels[UVChannel][0] * W0f
								+ Triangle.UVChannels[UVChannel][1] * W1f
								+ Triangle.UVChannels[UVChannel][2] * W2f
							: FallbackUV;
						VertexInstanceUVs.Set(VertexInstanceIDs[Corner], UVChannel, SourceUV);
					}

					VertexInstanceNormals[VertexInstanceIDs[Corner]] = FVector3f(Basis.Normal);
					VertexInstanceTangents[VertexInstanceIDs[Corner]] = FVector3f(Basis.Tangent);
					VertexInstanceBinormalSigns[VertexInstanceIDs[Corner]] = Basis.BinormalSign;
					VertexInstanceColors[VertexInstanceIDs[Corner]] = Triangle.bHasVertexColors
						? Triangle.VertexColors[0] * W0f + Triangle.VertexColors[1] * W1f + Triangle.VertexColors[2] * W2f
						: FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

					const FVector Projected = PlaneCover::ProjectPointToPlane(
						Positions[Corner], PlaneInfo.Normal, PlaneInfo.Rho);
					const double UFraction = (FVector::DotProduct(Projected, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
					const double PlaneVFraction = (FVector::DotProduct(Projected, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
					const double TileVFraction = bFlipTextureV ? 1.0 - PlaneVFraction : PlaneVFraction;
					CustomUVs[Corner] = FVector2D(UFraction, TileVFraction);
				}

				if (bEffectiveReverseWinding)
				{
					const FVertexInstanceID ReversedVertexInstanceIDs[3] =
					{
						VertexInstanceIDs[0], VertexInstanceIDs[2], VertexInstanceIDs[1]
					};
					OutCustomTextureCoordinates.Add(CustomUVs[0]);
					OutCustomTextureCoordinates.Add(CustomUVs[2]);
					OutCustomTextureCoordinates.Add(CustomUVs[1]);
					OutMeshDescription.CreateTriangle(PolygonGroupID, ReversedVertexInstanceIDs);
				}
				else
				{
					OutCustomTextureCoordinates.Add(CustomUVs[0]);
					OutCustomTextureCoordinates.Add(CustomUVs[1]);
					OutCustomTextureCoordinates.Add(CustomUVs[2]);
					OutMeshDescription.CreateTriangle(PolygonGroupID, VertexInstanceIDs);
				}
				OutRasterSourceTriangleIndices.Add(SourceTriangleIndex);
				return true;
			};

			for (const int32 TriangleIndex : TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				const PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
				const FVector Positions[3] = { Triangle.Vertices[0], Triangle.Vertices[1], Triangle.Vertices[2] };
				if (AppendTriangleGeometry(TriangleIndex, Triangle, Positions))
				{
					++OutMatchingTriangleCount;
				}
			}

			for (const PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
			{
				if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
				{
					continue;
				}
				const PlaneCover::FSourceTriangle& Triangle = Triangles[Projection.TriangleIndex];
				for (int32 PolygonVertexIndex = 1; PolygonVertexIndex + 1 < Projection.ClippedPolygon.Num(); ++PolygonVertexIndex)
				{
					const FVector Positions[3] =
					{
						Projection.ClippedPolygon[0],
						Projection.ClippedPolygon[PolygonVertexIndex],
						Projection.ClippedPolygon[PolygonVertexIndex + 1]
					};
					if (AppendTriangleGeometry(Projection.TriangleIndex, Triangle, Positions))
					{
						++OutMatchingTriangleCount;
					}
				}
			}
			return OutMatchingTriangleCount > 0;
		}

	}

	bool ComputeGpuWinnerBarycentric2D(
		const FVector2D& Point,
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		double& OutA,
		double& OutB,
		double& OutC)
	{
		const double Denominator = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		OutA = ((B.Y - C.Y) * (Point.X - C.X) + (C.X - B.X) * (Point.Y - C.Y)) / Denominator;
		OutB = ((C.Y - A.Y) * (Point.X - C.X) + (A.X - C.X) * (Point.Y - C.Y)) / Denominator;
		OutC = 1.0 - OutA - OutB;
		return FMath::IsFinite(OutA) && FMath::IsFinite(OutB) && FMath::IsFinite(OutC);
	}

	bool BuildPlaneSideBakeInputs(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FPlaneSideBakeParams& Params,
		FMeshDescription& OutMeshDescription,
		TArray<FVector2D>& OutCustomTextureCoordinates,
		int32& OutMatchingTriangleCount,
		FString& OutError,
		TArray<int32>& OutRasterSourceTriangleIndices)
	{
		OutMatchingTriangleCount = 0;
		OutCustomTextureCoordinates.Reset();
		OutError.Reset();
		OutRasterSourceTriangleIndices.Reset();
		if (FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
			|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
		{
			OutError = TEXT("Projected material bake plane footprint is degenerate.");
			return false;
		}

		TArray<int32> SortedTriangleIndices = TriangleIndices;
		TArray<PlaneCover::FCrackReductionProjection> SortedCrackReductionProjections = CrackReductionProjections;
		SortBakeFragmentsFarToNear(
			Triangles,
			Params.CaptureRayDirection,
			SortedTriangleIndices,
			SortedCrackReductionProjections);

		const bool bFlipTextureV = Params.AtlasVConvention == PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
		const bool bPlaneFrameMirrored = FVector::DotProduct(
			FVector::CrossProduct(PlaneInfo.AxisU, PlaneInfo.AxisV),
			PlaneInfo.Normal) < 0.0;
		const bool bEffectiveReverseWinding =
			(Params.bBackSide != bFlipTextureV) != bPlaneFrameMirrored;
		if (!BuildBakeMeshDescription(
				Triangles,
				SortedTriangleIndices,
				SortedCrackReductionProjections,
				PlaneInfo,
				Params,
				bEffectiveReverseWinding,
				OutMeshDescription,
				OutCustomTextureCoordinates,
				OutMatchingTriangleCount,
				OutRasterSourceTriangleIndices))
		{
			OutError = FString::Printf(
				TEXT("Projected material bake found no triangles for material %d."),
				Params.MaterialIndexFilter.IsSet()
					? Params.MaterialIndexFilter.GetValue()
					: INDEX_NONE);
			return false;
		}

		return true;
	}

	FColor EncodeObjectSpaceNormalToColor(const FVector& InNormal, const uint8 Alpha)
	{
		FVector Normal = InNormal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}
		return FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.X * 0.5 + 0.5) * 255.0), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.Y * 0.5 + 0.5) * 255.0), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.Z * 0.5 + 0.5) * 255.0), 0, 255)),
			Alpha);
	}

}
