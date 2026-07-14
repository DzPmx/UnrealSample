#include "FoliageBakerImpostorBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerImpostorSettings.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerPlaneCover.h"
#include "Containers/Set.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Math/RotationMatrix.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerImpostor, Log, All);

namespace
{
	using EOpacityMaskChannel = UE::FoliageBaker::MaterialResolver::EOpacityMaskChannel;
	using FMaterialBakeData = UE::FoliageBaker::MaterialResolver::FMaterialBakeData;
	using FMaterialOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;
	using FMaterialScalarBakeData = UE::FoliageBaker::MaterialResolver::FMaterialScalarBakeData;
	using FSourceTriangle = UE::FoliageBaker::PlaneCover::FSourceTriangle;
	using UE::FoliageBaker::MaterialResolver::SampleOpacityMaskValue;
	constexpr int32 ImpostorProjectionGuardPixels = 2;

	struct FImpostorCaptureView
	{
		FVector ViewDirection = FVector::ForwardVector;
		FVector CaptureRayDirection = -FVector::ForwardVector;
		FVector AxisU = FVector::RightVector;
		FVector AxisV = FVector::UpVector;
		FIntPoint TilePixelMin = FIntPoint::ZeroValue;
		FIntPoint TileSize = FIntPoint::ZeroValue;
		double ProjectionHalfExtentU = 1.0;
		double ProjectionHalfExtentV = 1.0;
		FIntPoint GridIndex = FIntPoint::ZeroValue;
	};

	struct FVisibleFragment
	{
		FVector2f Barycentric01 = FVector2f::ZeroVector;
		float CaptureDepth = TNumericLimits<float>::Max();
		int32 TriangleIndex = INDEX_NONE;

		bool IsValid() const
		{
			return TriangleIndex != INDEX_NONE;
		}
	};

	struct FNormalBasis
	{
		FVector Normal = FVector::UpVector;
		FVector Tangent = FVector::ForwardVector;
		float BinormalSign = 1.0f;
		float OutputNormalSign = 1.0f;
		bool bValid = false;
	};

	struct FImpostorBakeStats : UE::FoliageBaker::MaterialResolver::FMaterialResolveStats
	{
		int32 AtlasWidth = 0;
		int32 AtlasHeight = 0;
		int32 TileResolution = 0;
		int32 ViewCount = 0;
		int32 PaintedPixels = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 OpacityRejectedPixels = 0;
	};

	struct FImpostorBakeData
	{
		FBoxSphereBounds SourceBounds = FBoxSphereBounds(ForceInitToZero);
		double SharedCaptureHalfExtent = 1.0;
		TArray<FVector> SourceVertices;
		TArray<FSourceTriangle> Triangles;
		TArray<FImpostorCaptureView> Views;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> TileInfos;
		TArray<FColor> BaseColorPixels;
		TArray<FColor> NormalDepthPixels;
		TArray<FColor> MixPixels;
		TArray<float> CoverageValues;
		FImpostorBakeStats Stats;
	};

	bool ComputeSourceBounds(
		const TArray<FSourceTriangle>& Triangles,
		TArray<FVector>& OutVertices,
		FBoxSphereBounds& OutBounds)
	{
		TSet<FVector> UniqueVertices;
		UniqueVertices.Reserve(Triangles.Num());
		for (const FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				UniqueVertices.Add(Vertex);
			}
		}
		if (UniqueVertices.Num() == 0)
		{
			return false;
		}
		OutVertices.Reset(UniqueVertices.Num());
		for (const FVector& Vertex : UniqueVertices)
		{
			OutVertices.Add(Vertex);
		}
		OutBounds = FBoxSphereBounds(OutVertices.GetData(), static_cast<uint32>(OutVertices.Num()));
		return true;
	}

	bool ComputeBarycentric2D(
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
		return OutA >= -1.0e-5 && OutB >= -1.0e-5 && OutC >= -1.0e-5;
	}

	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	FColor EncodeObjectSpaceNormal(const FVector& InNormal, const uint8 Alpha)
	{
		const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		return FColor(
			UnitFloatToByte(static_cast<float>(Normal.X * 0.5 + 0.5)),
			UnitFloatToByte(static_cast<float>(Normal.Y * 0.5 + 0.5)),
			UnitFloatToByte(static_cast<float>(Normal.Z * 0.5 + 0.5)),
			Alpha);
	}

	FVector DecodeNormalColor(const FColor& Color)
	{
		return FVector(
			static_cast<double>(Color.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.B) / 255.0 * 2.0 - 1.0).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	FVector DeriveTangent(const FVector& InNormal)
	{
		const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const FVector ReferenceAxis = FMath::Abs(Normal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
		return FVector::CrossProduct(ReferenceAxis, Normal).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::RightVector);
	}

	FNormalBasis MakeNormalBasis(
		const FSourceTriangle& Triangle,
		const double W0,
		const double W1,
		const double W2,
		const bool bFlipOutputNormal)
	{
		FNormalBasis Result;
		FVector Normal = Triangle.VertexNormals[0] * W0
			+ Triangle.VertexNormals[1] * W1
			+ Triangle.VertexNormals[2] * W2;
		if (!Normal.Normalize())
		{
			Normal = Triangle.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		}

		FVector Tangent = Triangle.bHasTangents
			? Triangle.VertexTangents[0] * W0 + Triangle.VertexTangents[1] * W1 + Triangle.VertexTangents[2] * W2
			: DeriveTangent(Normal);
		Tangent = Tangent - Normal * FVector::DotProduct(Tangent, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = DeriveTangent(Normal);
		}

		Result.Normal = Normal;
		Result.Tangent = Tangent;
		Result.BinormalSign = Triangle.bHasTangents
			&& Triangle.BinormalSigns[0] * W0 + Triangle.BinormalSigns[1] * W1 + Triangle.BinormalSigns[2] * W2 < 0.0
			? -1.0f
			: 1.0f;
		Result.OutputNormalSign = bFlipOutputNormal ? -1.0f : 1.0f;
		Result.bValid = true;
		return Result;
	}

	FColor EncodeTangentNormalToObjectSpace(const FColor& RawNormal, const FNormalBasis& Basis, const uint8 Alpha)
	{
		if (!Basis.bValid)
		{
			return EncodeObjectSpaceNormal(FVector::UpVector, Alpha);
		}
		const FVector TangentSpaceNormal = DecodeNormalColor(RawNormal);
		const FVector Normal = Basis.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const FVector Tangent = Basis.Tangent.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, DeriveTangent(Normal));
		const FVector Binormal = FVector::CrossProduct(Normal, Tangent).GetSafeNormal() * Basis.BinormalSign;
		const FVector ObjectNormal = (Tangent * TangentSpaceNormal.X
			+ Binormal * TangentSpaceNormal.Y
			+ Normal * TangentSpaceNormal.Z).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Normal)
			* static_cast<double>(Basis.OutputNormalSign);
		return EncodeObjectSpaceNormal(ObjectNormal, Alpha);
	}

	float SampleMaterialScalar(const FMaterialScalarBakeData& BakeData, const FVector2f& UV)
	{
		if (!BakeData.bHasReadableTexture || !BakeData.Texture.IsValid())
		{
			return FMath::Clamp(BakeData.Constant, 0.0f, 1.0f);
		}
		const FLinearColor Sample = BakeData.Texture.Sample(UV);
		if (BakeData.bUseLuminance)
		{
			return FMath::Clamp(FMath::Max3(Sample.R, Sample.G, Sample.B), 0.0f, 1.0f);
		}
		switch (BakeData.Channel)
		{
		case EOpacityMaskChannel::Green: return FMath::Clamp(Sample.G, 0.0f, 1.0f);
		case EOpacityMaskChannel::Blue: return FMath::Clamp(Sample.B, 0.0f, 1.0f);
		case EOpacityMaskChannel::Alpha: return FMath::Clamp(Sample.A, 0.0f, 1.0f);
		case EOpacityMaskChannel::Red:
		default: return FMath::Clamp(Sample.R, 0.0f, 1.0f);
		}
	}

	FVector DecodeHemiOctahedralDirection(const FVector2D& Encoded)
	{
		const FVector2D Octahedron(
			(Encoded.X + Encoded.Y) * 0.5,
			(Encoded.X - Encoded.Y) * 0.5);
		return FVector(
			Octahedron.X,
			Octahedron.Y,
			1.0 - FMath::Abs(Octahedron.X) - FMath::Abs(Octahedron.Y)).GetSafeNormal();
	}

	FVector DecodeFullOctahedralDirection(const FVector2D& Encoded)
	{
		FVector Direction(
			Encoded.X,
			Encoded.Y,
			1.0 - FMath::Abs(Encoded.X) - FMath::Abs(Encoded.Y));
		if (Direction.Z < 0.0)
		{
			const double OldX = Direction.X;
			Direction.X = (1.0 - FMath::Abs(Direction.Y)) * (OldX >= 0.0 ? 1.0 : -1.0);
			Direction.Y = (1.0 - FMath::Abs(OldX)) * (Direction.Y >= 0.0 ? 1.0 : -1.0);
		}
		return Direction.GetSafeNormal();
	}

	double ComputeSharedCaptureHalfExtent(
		const TArray<FVector>& SourceVertices,
		const FBoxSphereBounds& SourceBounds,
		const TArray<FImpostorCaptureView>& Views,
		const int32 TileResolution)
	{
		double UnpaddedHalfExtent = 0.0;
		for (const FVector& Vertex : SourceVertices)
		{
			const FVector LocalPosition = Vertex - SourceBounds.Origin;
			for (const FImpostorCaptureView& View : Views)
			{
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.AxisU)));
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.AxisV)));
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.CaptureRayDirection)));
			}
		}

		const int32 UsableTileResolution = FMath::Max(
			TileResolution - ImpostorProjectionGuardPixels * 2,
			1);
		const double GuardScale = static_cast<double>(TileResolution)
			/ static_cast<double>(UsableTileResolution);
		const double SourceSphereRadius = FMath::Max(
			static_cast<double>(SourceBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);
		return FMath::Min(
			SourceSphereRadius,
			FMath::Max(UnpaddedHalfExtent * GuardScale, UE_DOUBLE_SMALL_NUMBER));
	}

	void BuildCaptureViews(
		const UFoliageBakerImpostorSettings& Settings,
		const FBoxSphereBounds& SourceBounds,
		const TArray<FVector>& SourceVertices,
		TArray<FImpostorCaptureView>& OutViews,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& OutTileInfos,
		FImpostorBakeStats& OutStats,
		double& OutSharedCaptureHalfExtent)
	{
		const int32 GridSize = FMath::Clamp(Settings.FrameGridSize, 3, 8);
		const int32 MaxAtlasResolution = FMath::Clamp(Settings.TextureResolution, 256, 4096);
		const int32 TileResolution = FMath::Max(4, (MaxAtlasResolution / GridSize) & ~3);

		OutStats.TileResolution = TileResolution;
		OutStats.AtlasWidth = TileResolution * GridSize;
		OutStats.AtlasHeight = TileResolution * GridSize;
		OutStats.ViewCount = GridSize * GridSize;
		OutViews.Reset(OutStats.ViewCount);
		OutTileInfos.Reset(OutStats.ViewCount);

		for (int32 GridY = 0; GridY < GridSize; ++GridY)
		{
			for (int32 GridX = 0; GridX < GridSize; ++GridX)
			{
				const FVector2D Encoded(
					-1.0 + 2.0 * static_cast<double>(GridX) / static_cast<double>(GridSize - 1),
					-1.0 + 2.0 * static_cast<double>(GridY) / static_cast<double>(GridSize - 1));
				FImpostorCaptureView& View = OutViews.AddDefaulted_GetRef();
				View.GridIndex = FIntPoint(GridX, GridY);
				View.ViewDirection = Settings.Coverage == EFoliageBakerImpostorCoverage::FullSphere
					? DecodeFullOctahedralDirection(Encoded)
					: DecodeHemiOctahedralDirection(Encoded);
				View.CaptureRayDirection = -View.ViewDirection;
				const FRotationMatrix CaptureRotation(View.CaptureRayDirection.Rotation());
				View.AxisU = CaptureRotation.GetScaledAxis(EAxis::Y);
				View.AxisV = CaptureRotation.GetScaledAxis(EAxis::Z);
				View.TilePixelMin = FIntPoint(GridX * TileResolution, GridY * TileResolution);
				View.TileSize = FIntPoint(TileResolution, TileResolution);

				UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& TileInfo = OutTileInfos.AddDefaulted_GetRef();
				TileInfo.AtlasPixelMin = View.TilePixelMin;
				TileInfo.AtlasTileSize = View.TileSize;
				TileInfo.AtlasTileResolution = TileResolution;
			}
		}

		OutSharedCaptureHalfExtent = ComputeSharedCaptureHalfExtent(
			SourceVertices,
			SourceBounds,
			OutViews,
			TileResolution);
		for (FImpostorCaptureView& View : OutViews)
		{
			View.ProjectionHalfExtentU = OutSharedCaptureHalfExtent;
			View.ProjectionHalfExtentV = OutSharedCaptureHalfExtent;
		}
	}

	bool BakeViewAtlas(
		const UStaticMesh& SourceStaticMesh,
		const int32 SourceLODIndex,
		const UFoliageBakerImpostorSettings& Settings,
		FImpostorBakeData& InOutData,
		FString& OutError)
	{
		const int32 PixelCount = InOutData.Stats.AtlasWidth * InOutData.Stats.AtlasHeight;
		if (PixelCount <= 0 || InOutData.Views.IsEmpty())
		{
			OutError = TEXT("The Impostor capture grid is empty.");
			return false;
		}

		InOutData.BaseColorPixels.Init(FColor(0, 0, 0, 0), PixelCount);
		InOutData.NormalDepthPixels.Init(EncodeObjectSpaceNormal(FVector::UpVector, UnitFloatToByte(0.5f)), PixelCount);
		if (Settings.bBakeMix)
		{
			InOutData.MixPixels.Init(FColor(255, 128, 0, 0), PixelCount);
		}
		else
		{
			InOutData.MixPixels.Reset();
		}

		TBitArray<> CoverageMask;
		CoverageMask.Init(false, PixelCount);
		InOutData.CoverageValues.Init(0.0f, PixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, PixelCount);
		FMaterialOutputSelection OutputSelection;
		OutputSelection.bBaseColorOpacity = true;
		OutputSelection.bNormalMask = Settings.bBakeNormalDepth;
		OutputSelection.bMix = Settings.bBakeMix;
		const TArray<FMaterialBakeData> MaterialData = UE::FoliageBaker::MaterialResolver::ResolveMaterialBakeData(
			SourceStaticMesh,
			SourceLODIndex,
			InOutData.SourceBounds,
			InOutData.Triangles,
			OutputSelection,
			FMath::Clamp(Settings.SourceMaterialBakeResolution, 256, 4096),
			true,
			InOutData.Stats);
		if (MaterialData.IsEmpty())
		{
			OutError = TEXT("No source material data could be resolved for the selected LOD.");
			return false;
		}

		const double SharedCaptureHalfExtent = FMath::Max(InOutData.SharedCaptureHalfExtent, UE_DOUBLE_SMALL_NUMBER);
		const FVector SharedCenter = InOutData.SourceBounds.Origin;
		for (const FImpostorCaptureView& View : InOutData.Views)
		{
			const int32 TilePixelCount = View.TileSize.X * View.TileSize.Y;
			TArray<FVisibleFragment> VisibleFragments;
			VisibleFragments.SetNum(TilePixelCount);
			for (int32 TriangleIndex = 0; TriangleIndex < InOutData.Triangles.Num(); ++TriangleIndex)
			{
				const FSourceTriangle& Triangle = InOutData.Triangles[TriangleIndex];
				if (Triangle.Area <= 0.0)
				{
					continue;
				}
				const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialData.Num() - 1);
				if (!MaterialData.IsValidIndex(MaterialIndex))
				{
					continue;
				}
				const FMaterialBakeData& BakeData = MaterialData[MaterialIndex];
				++InOutData.Stats.RasterizedTriangleReferences;

				FVector2D ProjectedPoints[3];
				for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
				{
					const FVector LocalPosition = Triangle.Vertices[VertexIndex] - SharedCenter;
					const double U = 0.5 + FVector::DotProduct(LocalPosition, View.AxisU) / (2.0 * View.ProjectionHalfExtentU);
					const double V = 0.5 - FVector::DotProduct(LocalPosition, View.AxisV) / (2.0 * View.ProjectionHalfExtentV);
					ProjectedPoints[VertexIndex] = FVector2D(U * View.TileSize.X, V * View.TileSize.Y);
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, View.TileSize.X - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, View.TileSize.X - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, View.TileSize.Y - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, View.TileSize.Y - 1);

				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						double W0 = 0.0;
						double W1 = 0.0;
						double W2 = 0.0;
						if (!ComputeBarycentric2D(FVector2D(X + 0.5, Y + 0.5), ProjectedPoints[0], ProjectedPoints[1], ProjectedPoints[2], W0, W1, W2))
						{
							continue;
						}

						const FVector2f SourceUV = Triangle.bHasUVs
							? Triangle.UVs[0] * static_cast<float>(W0)
								+ Triangle.UVs[1] * static_cast<float>(W1)
								+ Triangle.UVs[2] * static_cast<float>(W2)
							: FVector2f::ZeroVector;
						if (Triangle.bHasUVs
							&& BakeData.bUseTextureAlphaAsOpacity
							&& BakeData.bHasReadableOpacityMaskTexture
							&& BakeData.OpacityMaskTexture.IsValid())
						{
							const float Opacity = SampleOpacityMaskValue(BakeData.OpacityMaskTexture, SourceUV, BakeData.OpacityMaskChannel);
							if (Opacity < BakeData.OpacityMaskClipValue)
							{
								++InOutData.Stats.OpacityRejectedPixels;
								continue;
							}
						}

						const FVector SourcePoint = Triangle.Vertices[0] * W0 + Triangle.Vertices[1] * W1 + Triangle.Vertices[2] * W2;
						const float CaptureDepth = static_cast<float>(FVector::DotProduct(SourcePoint - SharedCenter, View.CaptureRayDirection));
						const int32 PixelIndex = Y * View.TileSize.X + X;
						if (CaptureDepth > VisibleFragments[PixelIndex].CaptureDepth + 1.0e-6f)
						{
							continue;
						}

						FVisibleFragment& Fragment = VisibleFragments[PixelIndex];
						Fragment.Barycentric01 = FVector2f(static_cast<float>(W0), static_cast<float>(W1));
						Fragment.CaptureDepth = CaptureDepth;
						Fragment.TriangleIndex = TriangleIndex;
					}
				}
			}

			for (int32 LocalY = 0; LocalY < View.TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = View.TilePixelMin.Y + LocalY;
				for (int32 LocalX = 0; LocalX < View.TileSize.X; ++LocalX)
				{
					const int32 TileIndex = LocalY * View.TileSize.X + LocalX;
					const FVisibleFragment& Fragment = VisibleFragments[TileIndex];
					if (!Fragment.IsValid() || !InOutData.Triangles.IsValidIndex(Fragment.TriangleIndex))
					{
						continue;
					}
					const int32 AtlasX = View.TilePixelMin.X + LocalX;
					const int32 AtlasIndex = AtlasY * InOutData.Stats.AtlasWidth + AtlasX;
					const FSourceTriangle& Triangle = InOutData.Triangles[Fragment.TriangleIndex];
					const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialData.Num() - 1);
					if (!MaterialData.IsValidIndex(MaterialIndex))
					{
						continue;
					}
					const FMaterialBakeData& BakeData = MaterialData[MaterialIndex];
					const double W0 = Fragment.Barycentric01.X;
					const double W1 = Fragment.Barycentric01.Y;
					const double W2 = 1.0 - W0 - W1;
					const FVector2f SourceUV = Triangle.bHasUVs
						? Triangle.UVs[0] * static_cast<float>(W0)
							+ Triangle.UVs[1] * static_cast<float>(W1)
							+ Triangle.UVs[2] * static_cast<float>(W2)
						: FVector2f::ZeroVector;

					if (Settings.bBakeBaseColorSdf)
					{
						const FLinearColor BaseColor = BakeData.bHasReadableBaseColorTexture
							? BakeData.BaseColorTexture.Sample(SourceUV)
							: BakeData.BaseColor;
						FColor Color = BaseColor.ToFColorSRGB();
						Color.A = 255;
						InOutData.BaseColorPixels[AtlasIndex] = Color;
					}
					CoverageMask[AtlasIndex] = true;
					InOutData.CoverageValues[AtlasIndex] = 1.0f;

					if (Settings.bBakeNormalDepth)
					{
						const float LinearDepth = FMath::Clamp(static_cast<float>((Fragment.CaptureDepth + SharedCaptureHalfExtent) / (2.0 * SharedCaptureHalfExtent)), 0.0f, 1.0f);
						const uint8 EncodedDepth = UnitFloatToByte(LinearDepth);
						const bool bFlipNormal = BakeData.bTwoSided
							&& BakeData.bSourceTangentSpaceNormal
							&& FVector::DotProduct(Triangle.Normal, View.CaptureRayDirection) < 0.0;
						const FNormalBasis Basis = MakeNormalBasis(Triangle, W0, W1, W2, bFlipNormal);
						InOutData.NormalDepthPixels[AtlasIndex] = BakeData.bHasReadableNormalTexture
							? EncodeTangentNormalToObjectSpace(BakeData.NormalTexture.SampleRawColor(SourceUV), Basis, EncodedDepth)
							: EncodeObjectSpaceNormal(Basis.Normal * static_cast<double>(Basis.OutputNormalSign), EncodedDepth);
						NormalCoverage[AtlasIndex] = true;
					}

					if (Settings.bBakeMix)
					{
						InOutData.MixPixels[AtlasIndex] = FColor(
							UnitFloatToByte(SampleMaterialScalar(BakeData.AmbientOcclusion, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Roughness, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Metallic, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Emission, SourceUV)));
					}
					++InOutData.Stats.PaintedPixels;
				}
			}
		}

		if (Settings.bBakeBaseColorSdf)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.BaseColorPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos);
			UE::FoliageBaker::Atlas::WriteUnionSdfToAlpha(
				InOutData.BaseColorPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				CoverageMask,
				FMath::Clamp(Settings.OpacitySdfRangePixels, 1, 64));
		}
		else
		{
			InOutData.BaseColorPixels.Reset();
		}

		if (Settings.bBakeNormalDepth)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.NormalDepthPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				&NormalCoverage,
				false);
			for (int32 PixelIndex = 0; PixelIndex < InOutData.NormalDepthPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage[PixelIndex])
				{
					InOutData.NormalDepthPixels[PixelIndex].A = UnitFloatToByte(0.5f);
				}
			}
			UE::FoliageBaker::Atlas::NormalizeEncodedObjectSpaceNormals(InOutData.NormalDepthPixels);
		}
		else
		{
			InOutData.NormalDepthPixels.Reset();
		}

		if (Settings.bBakeMix)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.MixPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				&CoverageMask,
				true);
		}
		return InOutData.Stats.PaintedPixels > 0;
	}

	constexpr int32 ImpostorCutoutTraceResolution = 16;
	constexpr int32 ImpostorCutoutOutlineVertexCount = 8;
	constexpr double ImpostorCutoutTraceThreshold = 0.25;

	struct FCutoutTraceAnchors
	{
		FVector2D X = FVector2D::ZeroVector;
		FVector2D Y = FVector2D::ZeroVector;
	};

	struct FCutoutTraceSegment
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
	};

	void BuildCutoutTraceValues(
		const TArray<FImpostorCaptureView>& Views,
		const TArray<float>& CoverageValues,
		const int32 AtlasWidth,
		TArray<double>& OutValues)
	{
		OutValues.Init(0.0, ImpostorCutoutTraceResolution * ImpostorCutoutTraceResolution);
		for (const FImpostorCaptureView& View : Views)
		{
			for (int32 TraceY = 0; TraceY < ImpostorCutoutTraceResolution; ++TraceY)
			{
				const double CellMinY = static_cast<double>(TraceY) * View.TileSize.Y / ImpostorCutoutTraceResolution;
				const double CellMaxY = static_cast<double>(TraceY + 1) * View.TileSize.Y / ImpostorCutoutTraceResolution;
				const int32 MinY = FMath::FloorToInt(CellMinY);
				const int32 MaxY = FMath::CeilToInt(CellMaxY);
				for (int32 TraceX = 0; TraceX < ImpostorCutoutTraceResolution; ++TraceX)
				{
					const double CellMinX = static_cast<double>(TraceX) * View.TileSize.X / ImpostorCutoutTraceResolution;
					const double CellMaxX = static_cast<double>(TraceX + 1) * View.TileSize.X / ImpostorCutoutTraceResolution;
					const int32 MinX = FMath::FloorToInt(CellMinX);
					const int32 MaxX = FMath::CeilToInt(CellMaxX);
					double CoverageSum = 0.0;
					double CoverageWeight = 0.0;
					for (int32 Y = MinY; Y < MaxY; ++Y)
					{
						const double WeightY = FMath::Max(
							FMath::Min(static_cast<double>(Y + 1), CellMaxY)
								- FMath::Max(static_cast<double>(Y), CellMinY),
							0.0);
						const int32 AtlasY = View.TilePixelMin.Y + FMath::Clamp(Y, 0, View.TileSize.Y - 1);
						for (int32 X = MinX; X < MaxX; ++X)
						{
							const double WeightX = FMath::Max(
								FMath::Min(static_cast<double>(X + 1), CellMaxX)
									- FMath::Max(static_cast<double>(X), CellMinX),
								0.0);
							const double Weight = WeightX * WeightY;
							const int32 AtlasX = View.TilePixelMin.X + FMath::Clamp(X, 0, View.TileSize.X - 1);
							const int32 AtlasIndex = AtlasY * AtlasWidth + AtlasX;
							if (CoverageValues.IsValidIndex(AtlasIndex))
							{
								CoverageSum += CoverageValues[AtlasIndex] * Weight;
								CoverageWeight += Weight;
							}
						}
					}
					const int32 TraceIndex = TraceY * ImpostorCutoutTraceResolution + TraceX;
					const double FilteredCoverage = CoverageWeight > 0.0 ? CoverageSum / CoverageWeight : 0.0;
					OutValues[TraceIndex] = FMath::Clamp(
						OutValues[TraceIndex] + FilteredCoverage,
						0.0,
						1.0);
				}
			}
		}
	}

	double SampleCutoutTrace(
		const TArray<double>& TraceValues,
		const FIntPoint SamplePoint,
		const FIntPoint Quadrant)
	{
		const int32 X = Quadrant.X == 0
			? SamplePoint.X
			: ImpostorCutoutTraceResolution - 1 - SamplePoint.X;
		const int32 Y = Quadrant.Y == 0
			? SamplePoint.Y
			: ImpostorCutoutTraceResolution - 1 - SamplePoint.Y;
		return TraceValues[Y * ImpostorCutoutTraceResolution + X];
	}

	FCutoutTraceAnchors FindCutoutTraceAnchors(
		const TArray<double>& TraceValues,
		const FIntPoint Quadrant)
	{
		FCutoutTraceAnchors Result;
		double CurrentSample = 0.0;
		for (int32 Y = 0; Y < 7; ++Y)
		{
			for (int32 X = 0; X < 15; ++X)
			{
				CurrentSample = SampleCutoutTrace(TraceValues, FIntPoint(X, Y), Quadrant);
				if (CurrentSample > ImpostorCutoutTraceThreshold)
				{
					Result.X = FVector2D(X, Y);
					break;
				}
			}
			if (CurrentSample > ImpostorCutoutTraceThreshold)
			{
				break;
			}
		}

		CurrentSample = 0.0;
		for (int32 X = 0; X < 7; ++X)
		{
			for (int32 Y = 0; Y < 15; ++Y)
			{
				CurrentSample = SampleCutoutTrace(TraceValues, FIntPoint(X, Y), Quadrant);
				if (CurrentSample > ImpostorCutoutTraceThreshold)
				{
					Result.Y = FVector2D(X, Y);
					break;
				}
			}
			if (CurrentSample > ImpostorCutoutTraceThreshold)
			{
				break;
			}
		}
		return Result;
	}

	FVector2D TransformCutoutTracePoint(const FVector2D& Point, const FIntPoint Quadrant)
	{
		FVector2D Result = Point;
		if (Quadrant.X != 0)
		{
			Result.X = ImpostorCutoutTraceResolution - 1.0 - Result.X;
		}
		if (Quadrant.Y != 0)
		{
			Result.Y = ImpostorCutoutTraceResolution - 1.0 - Result.Y;
		}
		Result += FVector2D(Quadrant);
		return Result / ImpostorCutoutTraceResolution;
	}

	FCutoutTraceSegment TraceCutoutQuadrant(
		const TArray<double>& TraceValues,
		const FIntPoint Quadrant)
	{
		const FCutoutTraceAnchors Anchors = FindCutoutTraceAnchors(TraceValues, Quadrant);
		const FVector2D VertexA = Anchors.Y;
		const FVector2D VertexB = Anchors.X;
		if (VertexA == VertexB)
		{
			return {
				TransformCutoutTracePoint(VertexA, Quadrant),
				TransformCutoutTracePoint(VertexB, Quadrant)
			};
		}

		double MaximumSlopes[ImpostorCutoutOutlineVertexCount] = {};
		FCutoutTraceSegment MaximumSlopeSegments[ImpostorCutoutOutlineVertexCount];
		const int32 SliceCount = FMath::Clamp(
			FMath::TruncToInt(VertexA.Y - VertexB.Y),
			0,
			ImpostorCutoutOutlineVertexCount);
		for (int32 X = FMath::TruncToInt(VertexA.X) + 1; X <= FMath::TruncToInt(VertexB.X); ++X)
		{
			for (int32 Y = 0; Y <= FMath::TruncToInt(VertexA.Y); ++Y)
			{
				if (SampleCutoutTrace(TraceValues, FIntPoint(X, Y), Quadrant) <= ImpostorCutoutTraceThreshold)
				{
					continue;
				}
				const FVector2D CurrentVertex(X, Y);
				for (int32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
				{
					const double Rise = VertexA.Y - SliceIndex - CurrentVertex.Y;
					const double Run = CurrentVertex.X - VertexA.X;
					const double Slope = Rise / Run;
					if (Slope > MaximumSlopes[SliceIndex])
					{
						const double XIntercept = (VertexA.Y - VertexB.Y - SliceIndex) / Slope;
						MaximumSlopes[SliceIndex] = Slope;
						MaximumSlopeSegments[SliceIndex].A = VertexA - FVector2D(0.0, SliceIndex);
						MaximumSlopeSegments[SliceIndex].B = FVector2D(VertexA.X + XIntercept, VertexB.Y);
					}
				}
				break;
			}
		}

		double MaximumArea = 0.0;
		int32 LargestSegmentIndex = 0;
		for (int32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
		{
			const FCutoutTraceSegment& Segment = MaximumSlopeSegments[SliceIndex];
			const double Area = ((Segment.B.X - Segment.A.X) * (Segment.A.Y - Segment.B.Y)) / 2.0;
			if (Area > MaximumArea)
			{
				MaximumArea = Area;
				LargestSegmentIndex = SliceIndex;
			}
		}
		return {
			TransformCutoutTracePoint(MaximumSlopeSegments[LargestSegmentIndex].A, Quadrant),
			TransformCutoutTracePoint(MaximumSlopeSegments[LargestSegmentIndex].B, Quadrant)
		};
	}

	bool BuildImpostorBakerOutline(
		const TArray<double>& TraceValues,
		TArray<FVector2D>& OutOutline)
	{
		static const FIntPoint Quadrants[] = {
			FIntPoint(0, 0),
			FIntPoint(1, 0),
			FIntPoint(1, 1),
			FIntPoint(0, 1)
		};
		OutOutline.Reset(ImpostorCutoutOutlineVertexCount);
		for (const FIntPoint Quadrant : Quadrants)
		{
			const FCutoutTraceSegment Segment = TraceCutoutQuadrant(TraceValues, Quadrant);
			OutOutline.Add(Segment.A);
			OutOutline.Add(Segment.B);
		}
		OutOutline.Sort([](const FVector2D& A, const FVector2D& B)
		{
			return FMath::Atan2(A.Y - 0.5, A.X - 0.5)
				> FMath::Atan2(B.Y - 0.5, B.X - 0.5);
		});
		return OutOutline.Num() == ImpostorCutoutOutlineVertexCount;
	}

	bool BuildCutoutMeshDescription(
		const FBoxSphereBounds& SourceBounds,
		const double SharedCaptureHalfExtent,
		const TArray<FImpostorCaptureView>& Views,
		const TArray<float>& CoverageValues,
		const int32 AtlasWidth,
		const int32 FrameGridSize,
		FMeshDescription& OutMeshDescription,
		FString& OutError)
	{
		OutMeshDescription.Empty();
		if (Views.IsEmpty() || CoverageValues.IsEmpty() || AtlasWidth <= 0)
		{
			OutError = TEXT("The captured coverage is empty and cannot produce an Impostor cutout.");
			return false;
		}
		TArray<double> TraceValues;
		BuildCutoutTraceValues(Views, CoverageValues, AtlasWidth, TraceValues);
		TArray<FVector2D> Outline;
		if (!BuildImpostorBakerOutline(TraceValues, Outline))
		{
			OutError = TEXT("The combined Impostor coverage cannot produce the UE ImpostorBaker cutout.");
			return false;
		}

		FStaticMeshAttributes Attributes(OutMeshDescription);
		Attributes.Register();
		Attributes.RegisterTriangleNormalAndTangentAttributes();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TTriangleAttributesRef<FVector3f> TriangleNormals = Attributes.GetTriangleNormals();
		TTriangleAttributesRef<FVector3f> TriangleTangents = Attributes.GetTriangleTangents();
		TTriangleAttributesRef<FVector3f> TriangleBinormals = Attributes.GetTriangleBinormals();
		TEdgeAttributesRef<bool> EdgeHardnesses = Attributes.GetEdgeHardnesses();
		TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		VertexInstanceUVs.SetNumChannels(1);
		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		MaterialSlotNames[PolygonGroupID] = TEXT("ImpostorProxy");
		const FVector Center = SourceBounds.Origin;
		const double HalfExtent = FMath::Max(SharedCaptureHalfExtent, UE_DOUBLE_SMALL_NUMBER);
		const double ClampedFrameGridSize = FMath::Clamp(FrameGridSize, 3, 8);
		const auto EncodeStoredUV = [ClampedFrameGridSize](const FVector2D& CutoutUV)
		{
			const double U = (FMath::Clamp(CutoutUV.X, 0.0, 1.0) + 0.001) * 0.995;
			const double V = (FMath::Clamp(CutoutUV.Y, 0.0, 1.0) + 0.001) * 0.995;
			return FVector2f(
				static_cast<float>((U / ClampedFrameGridSize) / 10.0),
				static_cast<float>((V / ClampedFrameGridSize) / 10.0));
		};
		const FVector FaceNormal = FVector::UpVector;
		const FVector FaceTangent = FVector::ForwardVector;
		const FVector FaceBinormal = FVector::CrossProduct(FaceNormal, FaceTangent).GetSafeNormal();
		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Outline.Num() + 1);
		const FVertexID CenterVertexID = OutMeshDescription.CreateVertex();
		VertexPositions[CenterVertexID] = FVector3f(Center);
		VertexIDs.Add(CenterVertexID);
		for (const FVector2D& UV : Outline)
		{
			const FVertexID VertexID = OutMeshDescription.CreateVertex();
			VertexPositions[VertexID] = FVector3f(Center + FVector(
				((UV.X - 0.5) * 2.0 * HalfExtent) / 10.0,
				((UV.Y - 0.5) * 2.0 * HalfExtent) / 10.0,
				0.0));
			VertexIDs.Add(VertexID);
		}
		for (int32 OutlineIndex = 0; OutlineIndex < Outline.Num(); ++OutlineIndex)
		{
			const int32 NextOutlineIndex = (OutlineIndex + 1) % Outline.Num();
			const int32 CornerIndices[3] = { NextOutlineIndex + 1, OutlineIndex + 1, 0 };
			TArray<FVertexInstanceID> VertexInstanceIDs;
			VertexInstanceIDs.Reserve(3);
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const int32 VertexIndex = CornerIndices[CornerIndex];
				const FVertexID VertexID = VertexIDs[VertexIndex];
				const FVertexInstanceID VertexInstanceID = OutMeshDescription.CreateVertexInstance(VertexID);
				VertexInstanceIDs.Add(VertexInstanceID);
				const FVector2D CutoutUV = VertexIndex == 0
					? FVector2D(0.5, 0.5)
					: Outline[VertexIndex - 1];
				VertexInstanceUVs.Set(VertexInstanceID, 0, EncodeStoredUV(CutoutUV));
				VertexInstanceNormals[VertexInstanceID] = FVector3f(FaceNormal);
				VertexInstanceTangents[VertexInstanceID] = FVector3f(FaceTangent);
				VertexInstanceBinormalSigns[VertexInstanceID] = 1.0f;
			}

			TArray<FEdgeID> NewEdges;
			const FPolygonID PolygonID = OutMeshDescription.CreatePolygon(PolygonGroupID, VertexInstanceIDs, &NewEdges);
			for (const FEdgeID EdgeID : NewEdges)
			{
				EdgeHardnesses[EdgeID] = true;
			}
			for (const FTriangleID TriangleID : OutMeshDescription.GetPolygonTriangles(PolygonID))
			{
				TriangleNormals[TriangleID] = FVector3f(FaceNormal);
				TriangleTangents[TriangleID] = FVector3f(FaceTangent);
				TriangleBinormals[TriangleID] = FVector3f(FaceBinormal);
			}
		}

		if (OutMeshDescription.Vertices().Num() != Outline.Num() + 1
			|| OutMeshDescription.Triangles().Num() != Outline.Num())
		{
			OutError = TEXT("The Impostor cutout mesh did not produce the expected triangles.");
			return false;
		}
		return true;
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const UFoliageBakerImpostorSettings& Settings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = MeshOutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = MeshOutputSelection.ReplaceLODIndex;
		Params.SourceLODIndex = Settings.SourceLODIndex;
		Params.DesiredUVChannelCount = 1;
		Params.MaterialSlotName = TEXT("ImpostorProxy");
		Params.bRecomputeNormals = true;
		Params.bRecomputeTangents = true;
		Params.BaseLODModel = 0;
		Params.RebuildLODMetadataKey = TEXT("FoliageBaker.ImpostorLOD");
		return Params;
	}

	UTexture2D* CreateTexture(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& Transaction,
		const UFoliageBakerImpostorSettings& Settings,
		const FString& Suffix,
		const TArray<FColor>& Pixels,
		const FImpostorBakeStats& Stats,
		const TextureCompressionSettings Compression,
		const TextureGroup LODGroup,
		const bool bSRGB,
		const FColor MipBackgroundColor,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = Settings.TextureOutputFolderName;
		Params.AssetNamePrefix = Settings.TextureNamePrefix;
		Params.AssetNameSuffix = Suffix;
		Params.Width = Stats.AtlasWidth;
		Params.Height = Stats.AtlasHeight;
		Params.CompressionSettings = Compression;
		Params.LODGroup = LODGroup;
		Params.bSRGB = bSRGB;
		Params.AlphaCoverageThreshold = 0.0f;
		Params.MipBackgroundColor = MipBackgroundColor;
		Params.bNormalizeMipNormals = LODGroup == TEXTUREGROUP_WorldNormalMap;
		const int32 TileResolution = FMath::Max(1, Stats.TileResolution);
		for (int32 TileY = 0; TileY < Stats.AtlasHeight; TileY += TileResolution)
		{
			for (int32 TileX = 0; TileX < Stats.AtlasWidth; TileX += TileResolution)
			{
				Params.MipTileRects.Add(FIntRect(
					FIntPoint(TileX, TileY),
					FIntPoint(
						FMath::Min(TileX + TileResolution, Stats.AtlasWidth),
						FMath::Min(TileY + TileResolution, Stats.AtlasHeight))));
			}
		}
		Params.EmptyPixelsError = TEXT("No Impostor atlas pixels were generated.");
		return FFoliageBakerAssetBuilder::CreateTextureAsset(SourceStaticMesh, Transaction, Params, Pixels, OutError);
	}

	bool ValidateParameterNames(const UFoliageBakerImpostorSettings& Settings, FString& OutError)
	{
		TSet<FName> TextureParameterNames;
		auto ValidateTextureName = [&](const bool bEnabled, const FName Name, const TCHAR* Label)
		{
			if (!bEnabled)
			{
				return true;
			}
			if (Name.IsNone())
			{
				OutError = FString::Printf(TEXT("%s texture parameter name is None."), Label);
				return false;
			}
			if (TextureParameterNames.Contains(Name))
			{
				OutError = FString::Printf(TEXT("Texture parameter '%s' is assigned to more than one enabled output."), *Name.ToString());
				return false;
			}
			TextureParameterNames.Add(Name);
			return true;
		};
		return ValidateTextureName(Settings.bBakeBaseColorSdf, Settings.BaseColorSdfTextureParameterName, TEXT("BaseColor/SDF"))
			&& ValidateTextureName(Settings.bBakeNormalDepth, Settings.NormalDepthTextureParameterName, TEXT("Normal/Depth"))
			&& ValidateTextureName(Settings.bBakeMix, Settings.MixTextureParameterName, TEXT("Mix"))
			&& !Settings.FramesParameterName.IsNone()
			&& !Settings.DefaultMeshSizeParameterName.IsNone()
			&& !Settings.PivotOffsetParameterName.IsNone()
			&& !Settings.UpperHemisphereStaticSwitchParameterName.IsNone();
	}

	void AppendCreatedAsset(UObject* Asset, TArray<UObject*>& OutAssets)
	{
		if (Asset)
		{
			OutAssets.AddUnique(Asset);
		}
	}
}

FFoliageBakerImpostorBakeResult FFoliageBakerImpostorBaker::Bake(
	UStaticMesh& SourceStaticMesh,
	UMaterialInstanceConstant& MaterialTemplate,
	const UFoliageBakerImpostorSettings& Settings)
{
	FFoliageBakerImpostorBakeResult Result;
	FString Error;
	if (Settings.SourceLODIndex < 0 || Settings.SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: Source LOD Index is outside the supported range."), *SourceStaticMesh.GetName());
		return Result;
	}
	if (!Settings.bBakeBaseColorSdf && !Settings.bBakeNormalDepth && !Settings.bBakeMix)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: no Impostor texture output is enabled."), *SourceStaticMesh.GetName());
		return Result;
	}
	if (!ValidateParameterNames(Settings, Error))
	{
		if (Error.IsEmpty())
		{
			Error = TEXT("One or more runtime material parameter names are None.");
		}
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	FImpostorBakeData BakeData;
	if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&SourceStaticMesh,
			Settings.SourceLODIndex,
			BakeData.Triangles,
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}
	if (!ComputeSourceBounds(BakeData.Triangles, BakeData.SourceVertices, BakeData.SourceBounds))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: selected LOD bounds could not be computed."), *SourceStaticMesh.GetName());
		return Result;
	}

	BuildCaptureViews(
		Settings,
		BakeData.SourceBounds,
		BakeData.SourceVertices,
		BakeData.Views,
		BakeData.TileInfos,
		BakeData.Stats,
		BakeData.SharedCaptureHalfExtent);
	if (!BakeViewAtlas(SourceStaticMesh, Settings.SourceLODIndex, Settings, BakeData, Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), Error.IsEmpty() ? TEXT("no visible Impostor pixels were captured.") : *Error);
		return Result;
	}

	FMeshDescription MeshDescription;
	if (!BuildCutoutMeshDescription(
			BakeData.SourceBounds,
			BakeData.SharedCaptureHalfExtent,
			BakeData.Views,
			BakeData.CoverageValues,
			BakeData.Stats.AtlasWidth,
			Settings.FrameGridSize,
			MeshDescription,
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
		FFoliageBakerMeshOutputDialog::OpenAfterBake(SourceStaticMesh, Settings.SourceLODIndex);
	if (!MeshOutputSelection.IsSet())
	{
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled after bake: no mesh output was selected and no generated assets were committed."),
			*SourceStaticMesh.GetName());
		return Result;
	}
	if (MeshOutputSelection->OutputMode != EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
		&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
			SourceStaticMesh,
			BuildSourceLODAssetParams(Settings, MeshOutputSelection.GetValue()),
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	FFoliageBakerAssetTransaction Transaction;
	if (Settings.bBakeBaseColorSdf)
	{
		Result.BaseColorSdfTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			Settings.BaseColorSdfTextureSuffix,
			BakeData.BaseColorPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_World,
			true,
			FColor(0, 0, 0, 0),
			Error);
		if (!Result.BaseColorSdfTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	if (Settings.bBakeNormalDepth)
	{
		Result.NormalDepthTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			Settings.NormalDepthTextureSuffix,
			BakeData.NormalDepthPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_WorldNormalMap,
			false,
			EncodeObjectSpaceNormal(FVector::UpVector, UnitFloatToByte(0.5f)),
			Error);
		if (!Result.NormalDepthTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	if (Settings.bBakeMix)
	{
		Result.MixTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			Settings.MixTextureSuffix,
			BakeData.MixPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			FColor(255, 128, 0, 0),
			Error);
		if (!Result.MixTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	FFoliageBakerMaterialInstanceAssetParams MaterialParams;
	MaterialParams.OutputFolderName = Settings.MaterialOutputFolderName;
	MaterialParams.AssetNamePrefix = Settings.MaterialInstanceNamePrefix;
	MaterialParams.AssetNameSuffix = Settings.MaterialInstanceNameSuffix;
	MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	MaterialParams.BaseColorOpacityTextureParameterName = Settings.BaseColorSdfTextureParameterName;
	MaterialParams.NormalDepthTextureParameterName = Settings.NormalDepthTextureParameterName;
	MaterialParams.MixTextureParameterName = Settings.MixTextureParameterName;
	Result.MaterialInstance = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
		SourceStaticMesh,
		Transaction,
		MaterialParams,
		&MaterialTemplate,
		Result.BaseColorSdfTexture,
		Result.NormalDepthTexture,
		Result.MixTexture,
		Error);
	if (!Result.MaterialInstance)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	const float FrameGridSize = static_cast<float>(FMath::Clamp(Settings.FrameGridSize, 3, 8));
	Result.MaterialInstance->SetScalarParameterValueEditorOnly(
		Settings.FramesParameterName,
		FrameGridSize);
	Result.MaterialInstance->SetScalarParameterValueEditorOnly(
		Settings.DefaultMeshSizeParameterName,
		static_cast<float>(BakeData.SharedCaptureHalfExtent * 2.0));
	Result.MaterialInstance->SetVectorParameterValueEditorOnly(
		Settings.PivotOffsetParameterName,
		FLinearColor(
			static_cast<float>(BakeData.SourceBounds.Origin.X),
			static_cast<float>(BakeData.SourceBounds.Origin.Y),
			static_cast<float>(BakeData.SourceBounds.Origin.Z),
			1.0f));
	const auto SetStaticSwitch = [&](const FName ParameterName, const bool bValue)
	{
		Result.MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
			FMaterialParameterInfo(ParameterName),
			bValue);
	};
	SetStaticSwitch(
		Settings.UpperHemisphereStaticSwitchParameterName,
		Settings.Coverage == EFoliageBakerImpostorCoverage::UpperHemisphere);
	SetStaticSwitch(TEXT("UseAmbientOcclusionTexture"), Settings.bBakeMix);
	SetStaticSwitch(TEXT("AmbientOcclusion_Channel_R"), true);
	SetStaticSwitch(TEXT("AmbientOcclusion_Channel_G"), false);
	SetStaticSwitch(TEXT("AmbientOcclusion_Channel_B"), false);
	SetStaticSwitch(TEXT("AmbientOcclusion_Channel_A"), false);
	SetStaticSwitch(TEXT("UseRoughnessTexture"), Settings.bBakeMix);
	SetStaticSwitch(TEXT("Roughness_Channel_R"), false);
	SetStaticSwitch(TEXT("Roughness_Channel_G"), true);
	SetStaticSwitch(TEXT("Roughness_Channel_B"), false);
	SetStaticSwitch(TEXT("Roughness_Channel_A"), false);
	SetStaticSwitch(TEXT("UseMetallicTexture"), Settings.bBakeMix);
	SetStaticSwitch(TEXT("Metallic_Channel_R"), false);
	SetStaticSwitch(TEXT("Metallic_Channel_G"), false);
	SetStaticSwitch(TEXT("Metallic_Channel_B"), true);
	SetStaticSwitch(TEXT("Metallic_Channel_A"), false);
	Result.MaterialInstance->PostEditChange();
	Result.MaterialInstance->MarkPackageDirty();

	if (MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
	{
		FFoliageBakerStaticMeshAssetParams MeshParams;
		MeshParams.AssetNameSuffix = TEXT("_ImpostorProxy");
		MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MeshParams.DesiredUVChannelCount = 1;
		MeshParams.MaterialSlotName = TEXT("ImpostorProxy");
		MeshParams.bRecomputeNormals = true;
		MeshParams.bRecomputeTangents = true;
		MeshParams.BaseLODModel = 0;
		Result.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
			SourceStaticMesh,
			Transaction,
			MeshParams,
			MeshDescription,
			Result.MaterialInstance,
			Error);
		if (!Result.ProxyMesh)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
		Result.ProxyMesh->SetNegativeBoundsExtension(FVector::ZeroVector);
		Result.ProxyMesh->SetPositiveBoundsExtension(FVector::ZeroVector);
		Result.ProxyMesh->CalculateExtendedBounds();
		const FBox CurrentBounds = Result.ProxyMesh->GetBounds().GetBox();
		const FVector DesiredMin = BakeData.SourceBounds.Origin - BakeData.SourceBounds.BoxExtent;
		const FVector DesiredMax = BakeData.SourceBounds.Origin + BakeData.SourceBounds.BoxExtent;
		const FVector NegativeExtension(
			FMath::Max(CurrentBounds.Min.X - DesiredMin.X, 0.0),
			FMath::Max(CurrentBounds.Min.Y - DesiredMin.Y, 0.0),
			FMath::Max(CurrentBounds.Min.Z - DesiredMin.Z, 0.0));
		const FVector PositiveExtension(
			FMath::Max(DesiredMax.X - CurrentBounds.Max.X, 0.0),
			FMath::Max(DesiredMax.Y - CurrentBounds.Max.Y, 0.0),
			FMath::Max(DesiredMax.Z - CurrentBounds.Max.Z, 0.0));
		Result.ProxyMesh->SetNegativeBoundsExtension(NegativeExtension);
		Result.ProxyMesh->SetPositiveBoundsExtension(PositiveExtension);
		Result.ProxyMesh->CalculateExtendedBounds();
		Result.ProxyMesh->PostEditChange();
		Result.ProxyMesh->SetExtendedBounds(BakeData.SourceBounds);
		Result.ProxyMesh->MarkPackageDirty();
	}
	else
	{
		int32 InstalledLODIndex = INDEX_NONE;
		if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				SourceStaticMesh,
				Transaction,
				BuildSourceLODAssetParams(Settings, MeshOutputSelection.GetValue()),
				MeshDescription,
				Result.MaterialInstance,
				InstalledLODIndex,
				Error))
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
		Result.ProxyMesh = &SourceStaticMesh;
		Result.SourceMeshLODIndex = InstalledLODIndex;
	}

	Transaction.Commit();
	Result.bSucceeded = true;
	if (MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
	{
		AppendCreatedAsset(Result.ProxyMesh, Result.CreatedAssets);
	}
	AppendCreatedAsset(Result.BaseColorSdfTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.NormalDepthTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.MixTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.MaterialInstance, Result.CreatedAssets);
	const int32 AtlasPixelCount = BakeData.Stats.AtlasWidth * BakeData.Stats.AtlasHeight;
	const double PaintedPixelPercent = AtlasPixelCount > 0
		? static_cast<double>(BakeData.Stats.PaintedPixels) * 100.0 / static_cast<double>(AtlasPixelCount)
		: 0.0;
	const double TexelAreaDensityGain = FMath::Square(
		static_cast<double>(BakeData.SourceBounds.SphereRadius) / BakeData.SharedCaptureHalfExtent);
	Result.Report = FString::Printf(
		TEXT("%s\n  Impostor bake succeeded\n  source LOD: %d\n  coverage: %s\n  sampling grid: %dx%d octahedral directions (%d views)\n  atlas: %dx%d, tile=%d\n  projection: shared tight square with up to %d px guard\n  channels: ColorOpacity RGB + SDF A, NormalMask object/local RGB + UE ImpostorBaker Depth A (near 0, far 1, empty 0.5)%s\n  bounds center: (%.3f, %.3f, %.3f), source sphere radius: %.3f cm, shared capture half extent: %.3f cm\n  projected texel area-density gain versus SphereRadius: %.3fx\n  proxy: UE ImpostorBaker-compatible XY cutout, center + 8 traced outline vertices, +Z facing, source asset Pivot preserved\n  painted pixels: %d/%d (%.2f%%), rasterized triangle references: %d, opacity rejects: %d\n  WPO/displacement: disabled by source material baking path\n  collision: off, lightmap UV: off, distance fields: on\n  material instance: %s"),
		*SourceStaticMesh.GetName(),
		Settings.SourceLODIndex,
		Settings.Coverage == EFoliageBakerImpostorCoverage::FullSphere ? TEXT("full sphere") : TEXT("upper hemisphere"),
		FMath::Clamp(Settings.FrameGridSize, 3, 8),
		FMath::Clamp(Settings.FrameGridSize, 3, 8),
		BakeData.Stats.ViewCount,
		BakeData.Stats.AtlasWidth,
		BakeData.Stats.AtlasHeight,
		BakeData.Stats.TileResolution,
		ImpostorProjectionGuardPixels,
		Settings.bBakeMix ? TEXT(", Mix RGBA enabled") : TEXT(""),
		BakeData.SourceBounds.Origin.X,
		BakeData.SourceBounds.Origin.Y,
		BakeData.SourceBounds.Origin.Z,
		BakeData.SourceBounds.SphereRadius,
		BakeData.SharedCaptureHalfExtent,
		TexelAreaDensityGain,
		BakeData.Stats.PaintedPixels,
		AtlasPixelCount,
		PaintedPixelPercent,
		BakeData.Stats.RasterizedTriangleReferences,
		BakeData.Stats.OpacityRejectedPixels,
		*Result.MaterialInstance->GetPathName());
	UE_LOG(LogFoliageBakerImpostor, Display, TEXT("\n%s"), *Result.Report);
	return Result;
}
