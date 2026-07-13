#include "FoliageBakerCardBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerPlaneCover.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerCardsCore, Log, All);

namespace
{
	bool ComputeSourceTriangleBounds(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		FBoxSphereBounds& OutBounds)
	{
		FBox Bounds(ForceInit);
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Bounds += Vertex;
			}
		}
		if (!Bounds.IsValid)
		{
			OutBounds = FBoxSphereBounds(ForceInitToZero);
			return false;
		}

		OutBounds = FBoxSphereBounds(Bounds);
		return true;
	}

	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const FFoliageBakerCardBakeRequest& Request)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		Settings.TextureAtlasResolution = FMath::Clamp(Request.TextureResolution, 256, 4096);
		Settings.SourceMaterialBakeResolution = FMath::Clamp(Request.SourceMaterialBakeResolution, 256, 4096);
		Settings.DoubleSidedBakeMode = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes
			: UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
		Settings.TrunkCardAtlasScale = 1.0;
		FBoxSphereBounds SourceBounds(ForceInitToZero);
		ComputeSourceTriangleBounds(SourceTriangles, SourceBounds);
		Settings.ErrorTolerance = FMath::Max(0.01, static_cast<double>(SourceBounds.SphereRadius) * 1.0e-6);
		Settings.bEnableAlphaAwareTileCrop = true;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
		return Settings;
	}

	struct FTrunkLeafClassification
	{
		int32 MatchedMaterialCount = 0;
		int32 TrunkTriangleCount = 0;
	};

	FTrunkLeafClassification ClassifyTrianglesForTrunkLeafMask(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<FString>& RawKeywords)
	{
		FTrunkLeafClassification Classification;
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(StaticMesh, RawKeywords);
		Classification.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;

		for (UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : SourceTriangles)
		{
			const bool bIsTrunk = MaterialMatches.IsMatch(Triangle.MaterialIndex);
			Triangle.bTrunkCardOnly = bIsTrunk;
			if (bIsTrunk)
			{
				++Classification.TrunkTriangleCount;
			}
		}

		return Classification;
	}

	using EBillboardOpacityMaskChannel = UE::FoliageBaker::MaterialResolver::EOpacityMaskChannel;
	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;
	using FMaterialBakeData = UE::FoliageBaker::MaterialResolver::FMaterialBakeData;
	using FMaterialScalarBakeData = UE::FoliageBaker::MaterialResolver::FMaterialScalarBakeData;
	using UE::FoliageBaker::MaterialResolver::SampleOpacityMaskValue;

	struct FAtlasBakeStats : UE::FoliageBaker::MaterialResolver::FMaterialResolveStats
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 TileResolution = 0;
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 SourceTexturedTriangles = 0;
		int32 FallbackTriangles = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 SourceMixTextureReferences = 0;
		int32 TextureAlphaOpacityReferences = 0;
		int32 ForcedOpaqueAlphaReferences = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
		int32 OpacitySdfRangePixels = 0;
	};


	bool ComputeBarycentric2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B, const FVector2D& C, double& OutA, double& OutB, double& OutC)
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

	FColor EncodeObjectSpaceNormalToColor(const FVector& InNormal, const uint8 Alpha = 255)
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

	FVector DecodeObjectSpaceNormalColor(const FColor& Color)
	{
		return FVector(
			static_cast<double>(Color.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.B) / 255.0 * 2.0 - 1.0).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	struct FNormalBakeBasisSample
	{
		FVector Normal = FVector::UpVector;
		FVector Tangent = FVector::ForwardVector;
		float BinormalSign = 1.0f;
		float OutputNormalSign = 1.0f;
		bool bValid = false;
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

	FNormalBakeBasisSample MakeNormalBakeBasisSample(
		const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle,
		const double W0,
		const double W1,
		const double W2,
		const bool bFlipOutputNormalForTwoSidedBackFace)
	{
		FNormalBakeBasisSample Result;

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
		Result.OutputNormalSign = bFlipOutputNormalForTwoSidedBackFace ? -1.0f : 1.0f;
		Result.bValid = true;
		return Result;
	}

	FColor EncodeBakedTangentSpaceNormalToObjectSpaceColor(
		const FColor& RawBakedTangentSpaceNormal,
		const FNormalBakeBasisSample& Basis,
		const uint8 AlphaOverride)
	{
		if (!Basis.bValid)
		{
			return EncodeObjectSpaceNormalToColor(FVector::UpVector, AlphaOverride);
		}

		const FVector TangentSpaceNormal = DecodeObjectSpaceNormalColor(RawBakedTangentSpaceNormal);
		const FVector Normal = Basis.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		FVector Tangent = Basis.Tangent - Normal * FVector::DotProduct(Basis.Tangent, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = DeriveTangentForNormal(Normal);
		}
		const FVector Binormal = FVector::CrossProduct(Normal, Tangent).GetSafeNormal() * Basis.BinormalSign;
		const FVector ObjectSpaceNormal = (Tangent * TangentSpaceNormal.X
			+ Binormal * TangentSpaceNormal.Y
			+ Normal * TangentSpaceNormal.Z).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Normal)
			* static_cast<double>(Basis.OutputNormalSign);
		return EncodeObjectSpaceNormalToColor(ObjectSpaceNormal, AlphaOverride);
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
		case EBillboardOpacityMaskChannel::Green: return FMath::Clamp(Sample.G, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Blue: return FMath::Clamp(Sample.B, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Alpha: return FMath::Clamp(Sample.A, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Red:
		default: return FMath::Clamp(Sample.R, 0.0f, 1.0f);
		}
	}

	struct FCardVisibleFragment
	{
		FVector2f Barycentric01 = FVector2f::ZeroVector;
		float CaptureDepth = TNumericLimits<float>::Max();
		int32 TriangleIndex = INDEX_NONE;
		uint8 ClassificationValue = 255;

		bool IsValid() const
		{
			return TriangleIndex != INDEX_NONE;
		}
	};

	void BakeCardAtlasOrthographic(
		const UStaticMesh& SourceStaticMesh,
		const int32 SourceLODIndex,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats& ProxyStats,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const FAtlasOutputSelection& OutputSelection,
		const bool bPackOpacitySdf,
		const int32 OpacitySdfRangePixels,
		TArray<FColor>& OutPixels,
		TArray<FColor>& OutNormalPixels,
		TArray<FColor>& OutMixPixels,
		FAtlasBakeStats& OutStats)
	{
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;

		const int32 AtlasPixelCount = FMath::Max(0, OutStats.Width * OutStats.Height);
		OutPixels.Init(FColor(0, 0, 0, 0), AtlasPixelCount);
		if (OutputSelection.bNormalMask)
		{
			OutNormalPixels.Init(EncodeObjectSpaceNormalToColor(FVector::UpVector, 255), AtlasPixelCount);
		}
		else
		{
			OutNormalPixels.Reset();
		}
		if (OutputSelection.bMix)
		{
			OutMixPixels.Init(FColor(255, 128, 0, 0), AtlasPixelCount);
		}
		else
		{
			OutMixPixels.Reset();
		}

		int64 PackedPaddedTilePixels = 0;
		auto AccumulateTileStats = [&](const FIntPoint& TileSize, const int32 Padding, const bool bBackFace)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			PackedPaddedTilePixels += static_cast<int64>(TileSize.X + Padding * 2)
				* static_cast<int64>(TileSize.Y + Padding * 2);
			if (bBackFace)
			{
				++OutStats.BackTileCount;
			}
			else
			{
				++OutStats.FrontTileCount;
			}
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const int32 Padding = FMath::Max(0, PlaneInfo.AtlasTilePaddingPixels);
			AccumulateTileStats(PlaneInfo.AtlasTileSize, Padding, false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileStats(PlaneInfo.BackAtlasTileSize, Padding, true);
			}
		}
		OutStats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, AtlasPixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, AtlasPixelCount);
		FAtlasOutputSelection SourcePropertySelection = OutputSelection;

		SourcePropertySelection.bBaseColorOpacity = true;
		const TArray<FMaterialBakeData> MaterialBakeData = UE::FoliageBaker::MaterialResolver::ResolveMaterialBakeData(
			SourceStaticMesh,
			SourceLODIndex,
			SourceLODBounds,
			Triangles,
			SourcePropertySelection,
			Settings.SourceMaterialBakeResolution,
			true,
			OutStats);
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0
				|| FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				return;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			TArray<FCardVisibleFragment> VisibleFragments;
			VisibleFragments.SetNum(TilePixelCount);
			const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
			const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);

			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
				if (Triangle.Area <= 0.0
					|| FVector::CrossProduct(
						Triangle.Vertices[1] - Triangle.Vertices[0],
						Triangle.Vertices[2] - Triangle.Vertices[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}

				const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialBakeData.Num() - 1);
				if (!MaterialBakeData.IsValidIndex(MaterialIndex))
				{
					continue;
				}
				const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
				++OutStats.RasterizedTriangleReferences;
				if (BakeData.bHasReadableBaseColorTexture)
				{
					++OutStats.SourceTexturedTriangles;
				}
				else
				{
					++OutStats.FallbackTriangles;
				}
				if (BakeData.bUseTextureAlphaAsOpacity && BakeData.bHasReadableOpacityMaskTexture)
				{
					++OutStats.TextureAlphaOpacityReferences;
				}
				else
				{
					++OutStats.ForcedOpaqueAlphaReferences;
				}
				if (OutputSelection.bMix
					&& (BakeData.AmbientOcclusion.bHasReadableTexture
						|| BakeData.Roughness.bHasReadableTexture
						|| BakeData.Metallic.bHasReadableTexture
						|| BakeData.Emission.bHasReadableTexture))
				{
					++OutStats.SourceMixTextureReferences;
				}
				const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (MaterialInterface && MaterialInterface->GetBlendMode() == BLEND_Masked)
				{
					++OutStats.MaskedMaterialBakeReferences;
				}

				FVector2D ProjectedPoints[3];
				for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
				{
					const FVector ProjectedVertex = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
						Triangle.Vertices[VertexIndex],
						PlaneInfo.Normal,
						PlaneInfo.Rho);
					const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
					const double PlaneVFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
					ProjectedPoints[VertexIndex] = FVector2D(
						UFraction * TileSize.X,
						(1.0 - PlaneVFraction) * TileSize.Y);
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);

				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						double W0 = 0.0;
						double W1 = 0.0;
						double W2 = 0.0;
						if (!ComputeBarycentric2D(
							FVector2D(X + 0.5, Y + 0.5),
							ProjectedPoints[0],
							ProjectedPoints[1],
							ProjectedPoints[2],
							W0,
							W1,
							W2))
						{
							continue;
						}

						const FVector2f SourceUV = Triangle.bHasUVs
							? Triangle.UVs[0] * static_cast<float>(W0)
								+ Triangle.UVs[1] * static_cast<float>(W1)
								+ Triangle.UVs[2] * static_cast<float>(W2)
							: FVector2f::ZeroVector;
						const uint8 ClassificationValue = Triangle.bTrunkCardOnly ? 128 : 255;
						if (Triangle.bHasUVs
							&& BakeData.bUseTextureAlphaAsOpacity
							&& BakeData.bHasReadableOpacityMaskTexture
							&& BakeData.OpacityMaskTexture.IsValid())
						{
							const float Opacity = SampleOpacityMaskValue(
								BakeData.OpacityMaskTexture,
								SourceUV,
								BakeData.OpacityMaskChannel);
							if (Opacity < BakeData.OpacityMaskClipValue)
							{
								continue;
							}
						}

						const FVector SourcePoint = Triangle.Vertices[0] * W0
							+ Triangle.Vertices[1] * W1
							+ Triangle.Vertices[2] * W2;
						const float CaptureDepth = static_cast<float>(FVector::DotProduct(SourcePoint, CaptureRayDirection));
						const int32 PixelIndex = Y * TileSize.X + X;
						if (!VisibleFragments.IsValidIndex(PixelIndex)
							|| CaptureDepth > VisibleFragments[PixelIndex].CaptureDepth + 1.0e-6f)
						{
							continue;
						}

						FCardVisibleFragment& Fragment = VisibleFragments[PixelIndex];
						Fragment.Barycentric01 = FVector2f(static_cast<float>(W0), static_cast<float>(W1));
						Fragment.CaptureDepth = CaptureDepth;
						Fragment.TriangleIndex = TriangleIndex;
						Fragment.ClassificationValue = ClassificationValue;
					}
				}
			}

			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = TilePixelMin.Y + LocalY;
				if (AtlasY < 0 || AtlasY >= OutStats.Height)
				{
					continue;
				}
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 AtlasX = TilePixelMin.X + LocalX;
					if (AtlasX < 0 || AtlasX >= OutStats.Width)
					{
						continue;
					}
					const int32 TilePixelIndex = LocalY * TileSize.X + LocalX;
					if (!VisibleFragments.IsValidIndex(TilePixelIndex)
						|| !VisibleFragments[TilePixelIndex].IsValid())
					{
						continue;
					}
					const FCardVisibleFragment& Fragment = VisibleFragments[TilePixelIndex];
					if (!Triangles.IsValidIndex(Fragment.TriangleIndex))
					{
						continue;
					}
					const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[Fragment.TriangleIndex];
					const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialBakeData.Num() - 1);
					if (!MaterialBakeData.IsValidIndex(MaterialIndex))
					{
						continue;
					}
					const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
					const double W0 = Fragment.Barycentric01.X;
					const double W1 = Fragment.Barycentric01.Y;
					const double W2 = 1.0 - W0 - W1;
					const FVector2f SourceUV = Triangle.bHasUVs
						? Triangle.UVs[0] * static_cast<float>(W0)
							+ Triangle.UVs[1] * static_cast<float>(W1)
							+ Triangle.UVs[2] * static_cast<float>(W2)
						: FVector2f::ZeroVector;
					const bool bFlipTwoSidedBackFaceOutputNormal = BakeData.bTwoSided
						&& BakeData.bSourceTangentSpaceNormal
						&& FVector::DotProduct(Triangle.Normal, CaptureRayDirection) < 0.0;
					FNormalBakeBasisSample Basis = MakeNormalBakeBasisSample(
						Triangle,
						W0,
						W1,
						W2,
						bFlipTwoSidedBackFaceOutputNormal);
					const int32 AtlasPixelIndex = AtlasY * OutStats.Width + AtlasX;

					if (OutputSelection.bBaseColorOpacity)
					{
						const FLinearColor BaseColor = BakeData.bHasReadableBaseColorTexture
							? BakeData.BaseColorTexture.Sample(SourceUV)
							: BakeData.BaseColor;
						FColor Color = BaseColor.ToFColorSRGB();
						Color.A = Fragment.ClassificationValue;
						OutPixels[AtlasPixelIndex] = Color;
					}
					if (AtlasCoverage.IsValidIndex(AtlasPixelIndex))
					{
						AtlasCoverage[AtlasPixelIndex] = true;
					}

					if (OutputSelection.bNormalMask)
					{
						OutNormalPixels[AtlasPixelIndex] = BakeData.bHasReadableNormalTexture
							? EncodeBakedTangentSpaceNormalToObjectSpaceColor(
								BakeData.NormalTexture.SampleRawColor(SourceUV),
								Basis,
								Fragment.ClassificationValue)
							: EncodeObjectSpaceNormalToColor(
								Basis.Normal * static_cast<double>(Basis.OutputNormalSign),
								Fragment.ClassificationValue);
						if (NormalCoverage.IsValidIndex(AtlasPixelIndex))
						{
							NormalCoverage[AtlasPixelIndex] = true;
						}
					}

					if (OutputSelection.bMix)
					{
						OutMixPixels[AtlasPixelIndex] = FColor(
							UnitFloatToByte(SampleMaterialScalar(BakeData.AmbientOcclusion, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Roughness, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Metallic, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Emission, SourceUV)));
					}
					++OutStats.PaintedPixels;
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			BakePlaneAndSide(
				PlaneInfo,
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				-PlaneInfo.Normal);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal);
			}
		}

		UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);
		if (OutputSelection.bBaseColorOpacity && bPackOpacitySdf)
		{
			OutStats.OpacitySdfRangePixels = FMath::Clamp(OpacitySdfRangePixels, 1, 64);
			UE::FoliageBaker::Atlas::WriteUnionSdfToAlpha(
				OutPixels,
				OutStats.Width,
				OutStats.Height,
				PlaneInfos,
				AtlasCoverage,
				OutStats.OpacitySdfRangePixels);
		}
		if (OutputSelection.bNormalMask)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutNormalPixels, OutStats.Width, OutStats.Height, PlaneInfos, &NormalCoverage, false);
			for (int32 PixelIndex = 0; PixelIndex < OutNormalPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage.IsValidIndex(PixelIndex) || !NormalCoverage[PixelIndex])
				{
					OutNormalPixels[PixelIndex].A = 0;
				}
			}
			UE::FoliageBaker::Atlas::NormalizeEncodedObjectSpaceNormals(OutNormalPixels);
		}
		if (OutputSelection.bMix)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutMixPixels, OutStats.Width, OutStats.Height, PlaneInfos, &AtlasCoverage, true);
		}
	}

	bool TrimUnusedAtlasSpace(
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FMeshDescription& MeshDescription,
		TArray<FColor>& AtlasPixels,
		TArray<FColor>& NormalAtlasPixels,
		TArray<FColor>& MixAtlasPixels,
		FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		const int32 OldWidth = AtlasStats.Width;
		const int32 OldHeight = AtlasStats.Height;
		if (OldWidth <= 0 || OldHeight <= 0 || PlaneInfos.IsEmpty())
		{
			return true;
		}

		int32 UsedMinX = OldWidth;
		int32 UsedMinY = OldHeight;
		int32 UsedMaxX = 0;
		int32 UsedMaxY = 0;
		auto AccumulateTileBounds = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize, const int32 Padding)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			const int32 SafePadding = FMath::Max(0, Padding);
			UsedMinX = FMath::Min(UsedMinX, FMath::Max(0, PixelMin.X - SafePadding));
			UsedMinY = FMath::Min(UsedMinY, FMath::Max(0, PixelMin.Y - SafePadding));
			UsedMaxX = FMath::Max(UsedMaxX, FMath::Min(OldWidth, PixelMin.X + TileSize.X + SafePadding));
			UsedMaxY = FMath::Max(UsedMaxY, FMath::Min(OldHeight, PixelMin.Y + TileSize.Y + SafePadding));
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateTileBounds(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileBounds(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			}
		}
		if (UsedMaxX <= UsedMinX || UsedMaxY <= UsedMinY)
		{
			OutError = TEXT("Could not determine the used atlas bounds for unused-space trimming.");
			return false;
		}



		constexpr int32 TextureBlockSize = 4;
		const int32 CropMinX = FMath::Clamp((UsedMinX / TextureBlockSize) * TextureBlockSize, 0, OldWidth - 1);
		const int32 CropMinY = FMath::Clamp((UsedMinY / TextureBlockSize) * TextureBlockSize, 0, OldHeight - 1);
		const int32 CropMaxX = FMath::Clamp(((UsedMaxX + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize, CropMinX + 1, OldWidth);
		const int32 CropMaxY = FMath::Clamp(((UsedMaxY + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize, CropMinY + 1, OldHeight);
		const int32 NewWidth = CropMaxX - CropMinX;
		const int32 NewHeight = CropMaxY - CropMinY;
		if (CropMinX == 0 && CropMinY == 0 && NewWidth == OldWidth && NewHeight == OldHeight)
		{
			return true;
		}

		auto BuildCroppedPixels = [&](const TArray<FColor>& SourcePixels, TArray<FColor>& OutCroppedPixels) -> bool
		{
			OutCroppedPixels.Reset();
			if (SourcePixels.IsEmpty())
			{
				return true;
			}
			if (SourcePixels.Num() != OldWidth * OldHeight)
			{
				return false;
			}
			OutCroppedPixels.SetNumUninitialized(NewWidth * NewHeight);
			for (int32 Y = 0; Y < NewHeight; ++Y)
			{
				const FColor* SourceRow = SourcePixels.GetData() + (CropMinY + Y) * OldWidth + CropMinX;
				FColor* DestinationRow = OutCroppedPixels.GetData() + Y * NewWidth;
				FMemory::Memcpy(DestinationRow, SourceRow, static_cast<SIZE_T>(NewWidth) * sizeof(FColor));
			}
			return true;
		};

		TArray<FColor> CroppedAtlasPixels;
		TArray<FColor> CroppedNormalAtlasPixels;
		TArray<FColor> CroppedMixAtlasPixels;
		if (!BuildCroppedPixels(AtlasPixels, CroppedAtlasPixels)
			|| !BuildCroppedPixels(NormalAtlasPixels, CroppedNormalAtlasPixels)
			|| !BuildCroppedPixels(MixAtlasPixels, CroppedMixAtlasPixels))
		{
			OutError = TEXT("Atlas pixel count did not match the atlas dimensions during unused-space trimming.");
			return false;
		}

		FStaticMeshAttributes MeshAttributes(MeshDescription);
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();
		if (VertexInstanceUVs.GetNumChannels() < 2)
		{
			OutError = TEXT("Generated card mesh does not contain UV0 and UV1 for atlas trimming.");
			return false;
		}
		auto RemapAtlasUV = [&](const FVector2f& OldUV)
		{
			return FVector2f(
				(static_cast<float>(OldUV.X) * static_cast<float>(OldWidth) - static_cast<float>(CropMinX)) / static_cast<float>(NewWidth),
				(static_cast<float>(OldUV.Y) * static_cast<float>(OldHeight) - static_cast<float>(CropMinY)) / static_cast<float>(NewHeight));
		};
		for (const FVertexInstanceID VertexInstanceID : MeshDescription.VertexInstances().GetElementIDs())
		{
			VertexInstanceUVs.Set(VertexInstanceID, 0, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 0)));
			VertexInstanceUVs.Set(VertexInstanceID, 1, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 1)));
		}

		for (UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			PlaneInfo.AtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PlaneInfo.BackAtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			}
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PlaneInfo.AtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.AtlasUVs[CornerIndex]);
				PlaneInfo.BackAtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.BackAtlasUVs[CornerIndex]);
			}
		}

		AtlasPixels = MoveTemp(CroppedAtlasPixels);
		NormalAtlasPixels = MoveTemp(CroppedNormalAtlasPixels);
		MixAtlasPixels = MoveTemp(CroppedMixAtlasPixels);
		AtlasStats.Width = NewWidth;
		AtlasStats.Height = NewHeight;

		int64 PackedPaddedTilePixels = 0;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const int32 Padding = FMath::Max(0, PlaneInfo.AtlasTilePaddingPixels);
			PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.AtlasTileSize.X + Padding * 2)
				* static_cast<int64>(PlaneInfo.AtlasTileSize.Y + Padding * 2);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.BackAtlasTileSize.X + Padding * 2)
					* static_cast<int64>(PlaneInfo.BackAtlasTileSize.Y + Padding * 2);
			}
		}
		const int64 NewAtlasPixelCount = static_cast<int64>(NewWidth) * static_cast<int64>(NewHeight);
		AtlasStats.PackedTileUtilizationPercent = NewAtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(NewAtlasPixelCount)
			: 0.0;
		return true;
	}

	UTexture2D* CreateBillboardTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FString& OutputFolderName,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TextureCompressionSettings CompressionSettings,
		const TextureGroup LODGroup,
		const bool bSRGB,
		const float AlphaCoverageThreshold,
		const FString& EmptyPixelsError,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = OutputFolderName;
		Params.AssetNamePrefix = AssetNamePrefix;
		Params.AssetNameSuffix = AssetNameSuffix;
		Params.Width = AtlasStats.Width;
		Params.Height = AtlasStats.Height;
		Params.CompressionSettings = CompressionSettings;
		Params.LODGroup = LODGroup;
		Params.bSRGB = bSRGB;
		Params.AlphaCoverageThreshold = AlphaCoverageThreshold;
		Params.EmptyPixelsError = EmptyPixelsError;
		return FFoliageBakerAssetBuilder::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Params,
			Pixels,
			OutError);
	}

	UTexture2D* CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.BaseColorOpacityTextureSuffix,
			Pixels,
			AtlasStats,
			TC_BC7,
			TEXTUREGROUP_World,
			true,
			0.0f,
			TEXT("No atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.NormalDepthTextureSuffix,
			Pixels,
			AtlasStats,
			TC_BC7,
			TEXTUREGROUP_WorldNormalMap,
			false,
			0.0f,
			TEXT("No normal atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.MixTextureSuffix,
			Pixels,
			AtlasStats,
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			0.0f,
			TEXT("No mix atlas pixels were generated."),
			OutError);
	}

	const TCHAR* GetMeshOutputModeText(const EFoliageBakerMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
		case EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD:
			return TEXT("replaced source mesh LOD");
		case EFoliageBakerMeshOutputMode::SeparateMeshAsset:
		default:
			return TEXT("separate mesh asset");
		}
	}

	struct FProxyPlaneCoverBuildData
	{
		int32 SourceLODIndex = INDEX_NONE;
		FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		FTrunkLeafClassification TrunkLeafClassification;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
	};

	struct FProxyMeshBuildData
	{
		FMeshDescription MeshDescription;
		UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats Stats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> PlaneInfos;
	};

	struct FProxyTextureBuildData
	{
		FAtlasOutputSelection OutputSelection;
		TArray<FColor> AtlasPixels;
		TArray<FColor> NormalAtlasPixels;
		TArray<FColor> MixAtlasPixels;
		FAtlasBakeStats AtlasStats;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	struct FProxyAssetBuildResult
	{
		bool bSucceeded = false;
		bool bCancelled = false;
		FString Report;
		UStaticMesh* ProxyMesh = nullptr;
		EFoliageBakerMeshOutputMode MeshOutputMode = EFoliageBakerMeshOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh.GetName(), *Error);
		UE_LOG(LogFoliageBakerCardsCore, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FProxyAssetBuildResult MakeProxyBuildCancelled(const UStaticMesh& StaticMesh)
	{
		FProxyAssetBuildResult Result;
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled after bake: no mesh output was selected and no generated assets were committed."),
			*StaticMesh.GetName());
		return Result;
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const FFoliageBakerCardBakeRequest& Settings)
	{
		FAtlasOutputSelection Selection;
		Selection.bBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Selection.bNormalMask = Settings.bBakeNormalDepth;
		Selection.bMix = Settings.bBakeMix;
		return Selection;
	}

	FVector ResolveSingleCaptureNormal(const EFoliageBakerCaptureAxis Axis)
	{
		switch (Axis)
		{
		case EFoliageBakerCaptureAxis::NegativeX: return FVector(-1.0, 0.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveY: return FVector(0.0, 1.0, 0.0);
		case EFoliageBakerCaptureAxis::NegativeY: return FVector(0.0, -1.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveX:
		default: return FVector(1.0, 0.0, 0.0);
		}
	}

	EFoliageBakerMeshAssetOutputMode ToAssetOutputMode(const EFoliageBakerMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshOutputMode::AddToSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD;
		case EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD;
		case EFoliageBakerMeshOutputMode::SeparateMeshAsset:
		default:
			return EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		}
	}

	EFoliageBakerMeshOutputMode ToCardOutputMode(const EFoliageBakerMeshAssetOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD:
			return EFoliageBakerMeshOutputMode::AddToSourceMeshLOD;
		case EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD:
			return EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD;
		case EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset:
		default:
			return EFoliageBakerMeshOutputMode::SeparateMeshAsset;
		}
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(const FFoliageBakerCardBakeRequest& Request)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = ToAssetOutputMode(Request.MeshOutputMode);
		Params.RequestedReplaceLODIndex = Request.ReplaceSourceLODIndex;
		Params.SourceLODIndex = Request.SourceLODIndex;
		Params.DesiredUVChannelCount = 2;
		Params.RebuildLODMetadataKey = Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			? FName(TEXT("FoliageBaker.SingleBillboardLOD"))
			: FName(TEXT("FoliageBaker.CrossCardsLOD"));
		return Params;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		OutData.SourceLODIndex = EditorSettings.SourceLODIndex;
		if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			OutData.SourceLODIndex,
			OutData.Triangles,
			OutError))
		{
			return false;
		}

		if (OutData.Triangles.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Source LOD %d contains no bakeable triangles."), OutData.SourceLODIndex);
			return false;
		}
		if (!ComputeSourceTriangleBounds(OutData.Triangles, OutData.SourceLODBounds))
		{
			OutError = FString::Printf(TEXT("Source LOD %d has no valid bounds."), OutData.SourceLODIndex);
			return false;
		}




		OutData.TrunkLeafClassification = ClassifyTrianglesForTrunkLeafMask(
			StaticMesh,
			OutData.Triangles,
			EditorSettings.TrunkMaterialKeywords);

		OutData.Settings = BuildSettingsForMesh(OutData.Triangles, EditorSettings);
		OutData.ProxyResult.SourceTriangleCount = OutData.Triangles.Num();
		OutData.ProxyResult.CoveredTriangleCount = OutData.Triangles.Num();
		TArray<int32> AllTriangleIndices;
		AllTriangleIndices.Reserve(OutData.Triangles.Num());
		double SourceArea = 0.0;
		for (int32 TriangleIndex = 0; TriangleIndex < OutData.Triangles.Num(); ++TriangleIndex)
		{
			AllTriangleIndices.Add(TriangleIndex);
			SourceArea += OutData.Triangles[TriangleIndex].Area;
		}
		OutData.ProxyResult.SourceArea = SourceArea;
		OutData.ProxyResult.CoveredArea = SourceArea;


		const int32 PlaneCount = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			? 1
			: FMath::Clamp(EditorSettings.CrossCardPlaneCount, 2, 5);
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneCount; ++PlaneIndex)
		{
			const FVector Normal = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis)
				: FVector(
					FMath::Cos(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
					FMath::Sin(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
					0.0);
			FVector AxisU = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane = OutData.ProxyResult.Planes.AddDefaulted_GetRef();
			Plane.Normal = Normal;
			Plane.Rho = 0.0;
			Plane.Score = SourceArea;
			Plane.CoveredArea = SourceArea;
			Plane.TriangleIndices = AllTriangleIndices;



			Plane.bIsTrunkCard = true;
			Plane.bUseFixedPlaneFrame = true;
			Plane.FixedAxisU = AxisU;
			Plane.FixedAxisV = FVector::UpVector;
		}
		return true;
	}

	bool BuildProxyMeshData(
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& OutData,
		FString& OutError)
	{
		return UE::FoliageBaker::PlaneCover::BuildPlaneProxyMeshDescription(
			CoverData.Triangles,
			CoverData.ProxyResult,
			CoverData.Settings,
			OutData.MeshDescription,
			OutData.Stats,
			OutError,
			&OutData.PlaneInfos);
	}

	bool BuildProxyTextureData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable BaseColor/SDF, Normal/TrunkLeafMask, or Mix.");
			return false;
		}
		UMaterialInstanceConstant* TemplateMaterialInstance = EditorSettings.MaterialTemplate;
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("A Material Instance Constant template is required.");
			return false;
		}

		auto BakeFeatureAtlas = [&](const FAtlasOutputSelection& OutputSelection,
			const bool bPackOpacitySdf,
			TArray<FColor>& AtlasPixels,
			TArray<FColor>& NormalPixels,
			TArray<FColor>& MixPixels,
			FAtlasBakeStats& AtlasStats)
		{
			BakeCardAtlasOrthographic(
				StaticMesh,
				CoverData.SourceLODIndex,
				CoverData.SourceLODBounds,
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				OutputSelection,
				bPackOpacitySdf,
				EditorSettings.OpacitySdfRangePixels,
				AtlasPixels,
				NormalPixels,
				MixPixels,
				AtlasStats);
		};

		int32 AlphaAwareCroppedPlaneCount = 0;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			constexpr uint8 AlphaCropThreshold = 1;
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			FAtlasBakeStats CropStats;
			BakeFeatureAtlas(
				CropOutputSelection,
				false,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropStats);

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				CoverData.Settings.AlphaAwareTileCropGuardPixels,
				AlphaCropThreshold,
				TileCrops);

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::FoliageBaker::PlaneCover::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					TileCrops,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError))
				{
					return false;
				}
			}
		}

		BakeFeatureAtlas(
			OutData.OutputSelection,
			true,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.AtlasStats);
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? CoverData.Settings.AlphaAwareTileCropGuardPixels
			: 0;
		if (EditorSettings.bTrimUnusedAtlasSpace)
		{
			if (!TrimUnusedAtlasSpace(
				MeshData.PlaneInfos,
				MeshData.MeshDescription,
				OutData.AtlasPixels,
				OutData.NormalAtlasPixels,
				OutData.MixAtlasPixels,
				OutData.AtlasStats,
				OutError))
			{
				return false;
			}
			MeshData.Stats.AtlasWidth = OutData.AtlasStats.Width;
			MeshData.Stats.AtlasHeight = OutData.AtlasStats.Height;
		}

		if (OutData.OutputSelection.bBaseColorOpacity)
		{
			OutData.AtlasTexture = CreateAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutData.AtlasPixels,
				OutData.AtlasStats,
				OutError);
			if (!OutData.AtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bNormalMask)
		{
			OutData.NormalAtlasTexture = CreateNormalAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutData.NormalAtlasPixels,
				OutData.AtlasStats,
				OutError);
			if (!OutData.NormalAtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bMix)
		{
			OutData.MixAtlasTexture = CreateMixAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutData.MixAtlasPixels,
				OutData.AtlasStats,
				OutError);
			if (!OutData.MixAtlasTexture)
			{
				return false;
			}
		}

		FFoliageBakerMaterialInstanceAssetParams MaterialParams;
		MaterialParams.OutputFolderName = EditorSettings.MaterialOutputFolderName;
		MaterialParams.AssetNamePrefix = EditorSettings.MaterialInstanceNamePrefix;
		MaterialParams.AssetNameSuffix = EditorSettings.MaterialInstanceNameSuffix;
		MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MaterialParams.BaseColorOpacityTextureParameterName = EditorSettings.BaseColorOpacityTextureParameterName;
		MaterialParams.NormalDepthTextureParameterName = EditorSettings.NormalDepthTextureParameterName;
		MaterialParams.MixTextureParameterName = EditorSettings.MixTextureParameterName;
		OutData.Material = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
			StaticMesh,
			AssetTransaction,
			MaterialParams,
			TemplateMaterialInstance,
			OutData.AtlasTexture,
			OutData.NormalAtlasTexture,
			OutData.MixAtlasTexture,
			OutError);
		return OutData.Material != nullptr;
	}

	bool CreateProxyMeshAssetBundle(
		UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = EditorSettings.MeshOutputMode;

		if (EditorSettings.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? TEXT("_Billboard")
				: TEXT("_CrossCards");
			MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
			MeshParams.DesiredUVChannelCount = 2;
			OutResult.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
				StaticMesh,
				AssetTransaction,
				MeshParams,
				MeshData.MeshDescription,
				TextureData.Material,
				OutError);
			if (!OutResult.ProxyMesh)
			{
				return false;
			}
		}
		else
		{
			int32 InstalledLODIndex = INDEX_NONE;
			const FFoliageBakerSourceLODAssetParams LODParams = BuildSourceLODAssetParams(EditorSettings);
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				LODParams,
				MeshData.MeshDescription,
				TextureData.Material,
				InstalledLODIndex,
				OutError))
			{
				return false;
			}

			OutResult.ProxyMesh = &StaticMesh;
			OutResult.SourceMeshLODIndex = InstalledLODIndex;
		}

		OutResult.AtlasTexture = TextureData.AtlasTexture;
		OutResult.NormalAtlasTexture = TextureData.NormalAtlasTexture;
		OutResult.MixAtlasTexture = TextureData.MixAtlasTexture;
		OutResult.Material = TextureData.Material;
		return true;
	}

	FString BuildProxySuccessReport(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& Request,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);

		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm\n  feature: %s, capture=%s, selected-LOD projected bounds, per-angle alpha crop\n  trunk/leaf classification: shared material/parent keyword rule, matched materials=%d, trunk triangles=%d"),
			*StaticMesh.GetName(),
			CoverData.SourceLODIndex,
			CoverData.SourceLODBounds.SphereRadius,
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard ? TEXT("Single Billboard") : TEXT("Cross Cards"),
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard ? TEXT("one selected axis, one baked side") : TEXT("equally spaced over 180 degrees, front and back baked"),
			CoverData.TrunkLeafClassification.MatchedMaterialCount,
			CoverData.TrunkLeafClassification.TrunkTriangleCount);
		const FString BaseAtlasPath = TextureData.AtlasTexture ? TextureData.AtlasTexture->GetPathName() : TEXT("disabled");
		const FString NormalAtlasPath = TextureData.NormalAtlasTexture ? TextureData.NormalAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MixAtlasPath = TextureData.MixAtlasTexture ? TextureData.MixAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/SDF=%s, Normal/TrunkLeafMask=%s, Mix=%s"),
			*Request.BaseColorOpacityTextureParameterName.ToString(),
			*Request.NormalDepthTextureParameterName.ToString(),
			*Request.MixTextureParameterName.ToString());

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, readable material textures=%d, mix-texture materials=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, alpha refs=%d, masked refs=%d, mix refs texture=%d, forced opaque=%d, shooting=%s, resolve=%s\n  base/color SDF atlas: %s, RGB=BaseColor, A=whole-vegetation Union SDF (outside 0, contour 0.5, inside 1), SDF range=%d px\n  normal/trunk-leaf atlas: %s, RGB=object/local-space normal, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; Single Billboard uses one baked side\n  material instance: %s (copied from the supplied MIC template; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals"),
			*TechniqueSummary,
			*AlphaPolicyDetails,
			*MeshOutputDetails,
			MeshData.Stats.PlaneCount,
			MeshData.Stats.QuadCount,
			MeshData.Stats.TriangleCount,
			TextureData.AtlasStats.Width,
			TextureData.AtlasStats.Height,
			TextureData.AtlasStats.TileResolution,
			TextureData.AtlasStats.PackedTileUtilizationPercent,
			TextureData.AtlasStats.FrontTileCount,
			TextureData.AtlasStats.BackTileCount,
			TextureData.AtlasStats.PaintedPixels,
			TextureData.AtlasStats.AlphaAwareCroppedPlanes,
			TextureData.AtlasStats.AlphaAwareTileCropGuardPixels,
			TextureData.AtlasStats.ReadableMaterialTextures,
			TextureData.AtlasStats.SourceMixTextureMaterials,
			TextureData.AtlasStats.TextureAlphaOpacityMaterials,
			TextureData.AtlasStats.SourceTexturedTriangles,
			TextureData.AtlasStats.FallbackTriangles,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.TextureAlphaOpacityReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			TextureData.AtlasStats.SourceMixTextureReferences,
			TextureData.AtlasStats.ForcedOpaqueAlphaReferences,
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? TEXT("dedicated fixed-axis orthographic capture, all selected-LOD triangles, WPO disabled")
				: TEXT("dedicated fixed-angle orthographic capture, front and back per plane, all selected-LOD triangles, WPO disabled"),
			TEXT("opacity rejection before exact per-pixel nearest-depth selection; winning source UV samples BaseColor, Normal, and Mix"),
			*BaseAtlasPath,
			TextureData.AtlasStats.OpacitySdfRangePixels,
			*NormalAtlasPath,
			*MixAtlasPath,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails);
	}

	FProxyAssetBuildResult BuildCardProxyAsset(
		UStaticMesh& StaticMesh,
		FFoliageBakerCardBakeRequest EditorSettings)
	{
		FString Error;
		FProxyPlaneCoverBuildData CoverData;
		if (!BuildProxyPlaneCoverData(StaticMesh, EditorSettings, CoverData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyMeshBuildData MeshData;
		if (!BuildProxyMeshData(CoverData, MeshData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyTextureBuildData TextureData;
		FFoliageBakerAssetTransaction AssetTransaction;
		if (!BuildProxyTextureData(StaticMesh, EditorSettings, AssetTransaction, CoverData, MeshData, TextureData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
			FFoliageBakerMeshOutputDialog::OpenAfterBake(StaticMesh, EditorSettings.SourceLODIndex);
		if (!MeshOutputSelection.IsSet())
		{
			return MakeProxyBuildCancelled(StaticMesh);
		}
		EditorSettings.MeshOutputMode = ToCardOutputMode(MeshOutputSelection->OutputMode);
		EditorSettings.ReplaceSourceLODIndex = MeshOutputSelection->ReplaceLODIndex;
		if (EditorSettings.MeshOutputMode != EFoliageBakerMeshOutputMode::SeparateMeshAsset
			&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
				StaticMesh,
				BuildSourceLODAssetParams(EditorSettings),
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(StaticMesh, EditorSettings, AssetTransaction, MeshData, TextureData, Result, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
		AssetTransaction.Commit();

		Result.bSucceeded = true;
		Result.Report = BuildProxySuccessReport(StaticMesh, EditorSettings, CoverData, MeshData, TextureData, Result);
		UE_LOG(LogFoliageBakerCardsCore, Display, TEXT("\n%s"), *Result.Report);
		return Result;
	}

	void AppendCardCreatedAssets(const FProxyAssetBuildResult& BuildResult, TArray<UObject*>& OutCreatedAssets)
	{
		if (BuildResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset && BuildResult.ProxyMesh)
		{
			OutCreatedAssets.Add(BuildResult.ProxyMesh);
		}
		if (BuildResult.AtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.AtlasTexture);
		}
		if (BuildResult.NormalAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.NormalAtlasTexture);
		}
		if (BuildResult.MixAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.MixAtlasTexture);
		}
		if (BuildResult.Material)
		{
			OutCreatedAssets.Add(BuildResult.Material);
		}
	}
}


FFoliageBakerCardBakeResult FFoliageBakerCardBaker::Bake(const FFoliageBakerCardBakeRequest& Request)
{
	FFoliageBakerCardBakeResult OutResult;
	if (!Request.SourceStaticMesh)
	{
		OutResult.Report = TEXT("Foliage Baker failed: source Static Mesh is null.");
		return OutResult;
	}
	if (Request.SourceLODIndex < 0 || Request.SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: source LOD index %d is outside the supported range 0-%d."),
			*Request.SourceStaticMesh->GetName(),
			Request.SourceLODIndex,
			MAX_STATIC_MESH_LODS - 1);
		return OutResult;
	}
	if (!Request.MaterialTemplate)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: a Material Instance Constant template is required."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	if (!Request.bBakeBaseColorOpacity && !Request.bBakeNormalDepth && !Request.bBakeMix)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: no texture output is enabled."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	TSet<FName> UsedTextureParameterNames;
	FString TextureParameterError;
	auto ValidateTextureParameterName = [&](const bool bEnabled, const FName ParameterName, const TCHAR* OutputLabel) -> bool
	{
		if (!bEnabled)
		{
			return true;
		}
		if (ParameterName.IsNone())
		{
			TextureParameterError = FString::Printf(
				TEXT("%s output is enabled, but its Material texture parameter name is None."),
				OutputLabel);
			return false;
		}
		if (UsedTextureParameterNames.Contains(ParameterName))
		{
			TextureParameterError = FString::Printf(
				TEXT("Material texture parameter '%s' is assigned to more than one enabled output."),
				*ParameterName.ToString());
			return false;
		}
		UsedTextureParameterNames.Add(ParameterName);
		return true;
	};
	if (!ValidateTextureParameterName(Request.bBakeBaseColorOpacity, Request.BaseColorOpacityTextureParameterName, TEXT("BaseColor/SDF"))
		|| !ValidateTextureParameterName(Request.bBakeNormalDepth, Request.NormalDepthTextureParameterName, TEXT("Normal/TrunkLeafMask"))
		|| !ValidateTextureParameterName(Request.bBakeMix, Request.MixTextureParameterName, TEXT("Mix")))
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*Request.SourceStaticMesh->GetName(),
			*TextureParameterError);
		return OutResult;
	}
	if (Request.TextureResolution < 256 || Request.TextureResolution > 4096)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: texture resolution must be between 256 and 4096."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}

	FFoliageBakerCardBakeRequest SanitizedRequest = Request;
	SanitizedRequest.SourceLODIndex = Request.SourceLODIndex;
	SanitizedRequest.CrossCardPlaneCount = FMath::Clamp(Request.CrossCardPlaneCount, 2, 5);
	SanitizedRequest.SourceMaterialBakeResolution = FMath::Clamp(Request.SourceMaterialBakeResolution, 256, 4096);
	SanitizedRequest.AlphaCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
	SanitizedRequest.OpacitySdfRangePixels = FMath::Clamp(Request.OpacitySdfRangePixels, 1, 64);
	const FString FeatureSuffix = Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
		? TEXT("_Billboard")
		: TEXT("_CrossCards");
	SanitizedRequest.BaseColorOpacityTextureSuffix = FeatureSuffix + Request.BaseColorOpacityTextureSuffix;
	SanitizedRequest.NormalDepthTextureSuffix = FeatureSuffix + Request.NormalDepthTextureSuffix;
	SanitizedRequest.MixTextureSuffix = FeatureSuffix + Request.MixTextureSuffix;
	SanitizedRequest.MaterialInstanceNameSuffix = FeatureSuffix + Request.MaterialInstanceNameSuffix;

	const FProxyAssetBuildResult InternalResult = BuildCardProxyAsset(*Request.SourceStaticMesh, SanitizedRequest);
	OutResult.bSucceeded = InternalResult.bSucceeded;
	OutResult.bCancelled = InternalResult.bCancelled;
	OutResult.ProxyMesh = InternalResult.ProxyMesh;
	OutResult.SourceMeshLODIndex = InternalResult.SourceMeshLODIndex;
	OutResult.ColorOpacityTexture = InternalResult.AtlasTexture;
	OutResult.NormalDepthTexture = InternalResult.NormalAtlasTexture;
	OutResult.MixTexture = InternalResult.MixAtlasTexture;
	OutResult.MaterialInstance = InternalResult.Material;
	OutResult.Report = InternalResult.Report;
	AppendCardCreatedAssets(InternalResult, OutResult.CreatedAssets);
	return OutResult;
}
