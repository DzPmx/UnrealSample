#include "FoliageBakerCardBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialBakingStructures.h"
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

	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;

	struct FAtlasBakeStats
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 TileResolution = 0;
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 RasterizedTriangleReferences = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
		int32 OpacitySdfRangePixels = 0;
		FString MaterialAlphaPolicyDetails;
	};

	struct FCardVisibleFragment
	{
		FColor BaseColor = FColor::Black;
		uint8 ClassificationValue = 255;
		bool bVisible = false;

		bool IsValid() const
		{
			return bVisible;
		}
	};

	bool BakeCardAtlasOrthographic(
		const UStaticMesh& SourceStaticMesh,
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
		FAtlasBakeStats& OutStats,
		FString& OutError)
	{
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;

		const int32 AtlasPixelCount = FMath::Max(0, OutStats.Width * OutStats.Height);
		OutPixels.Init(FColor(0, 0, 0, 0), AtlasPixelCount);
		if (OutputSelection.bNormalMask)
		{
			OutNormalPixels.Init(
				UE::FoliageBaker::ProjectedMaterialBake::EncodeObjectSpaceNormalToColor(FVector::UpVector, 255),
				AtlasPixelCount);
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
		OutStats.MaterialAlphaPolicyDetails =
			TEXT("\n    card BaseColor/source-triangle-id/normal/Mix=per-tile source masked shader with shared GPU depth; all materials compete in one depth buffer; no CPU material-property fallback");
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide) -> bool
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0
				|| FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				return true;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			TArray<FCardVisibleFragment> VisibleFragments;
			VisibleFragments.SetNum(TilePixelCount);
			TArray<FColor> ProjectedNormalTile;
			TArray<FColor> ProjectedMixTile;
			if (OutputSelection.bNormalMask)
			{
				ProjectedNormalTile.Init(FColor::Black, TilePixelCount);
			}
			{
				// Cards use all selected-LOD triangles for every capture plane. The
				// same projected mesh feeds the shared depth winner and every requested attribute pass.
				TArray<int32> PrimaryTriangleIndices;
				TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection> CrackReductionProjectionsToBake;
				TBitArray<> QueuedTriangles;
				QueuedTriangles.Init(false, Triangles.Num());
				for (const int32 TriangleIndex : PlaneInfo.TriangleIndices)
				{
					if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
					{
						QueuedTriangles[TriangleIndex] = true;
						PrimaryTriangleIndices.Add(TriangleIndex);
					}
				}
				for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : PlaneInfo.CrackReductionProjections)
				{
					if (Triangles.IsValidIndex(Projection.TriangleIndex) && !QueuedTriangles[Projection.TriangleIndex])
					{
						QueuedTriangles[Projection.TriangleIndex] = true;
						CrackReductionProjectionsToBake.Add(Projection);
					}
				}

				TSet<int32> ReferencedMaterialSet;
				for (const int32 TriangleIndex : PrimaryTriangleIndices)
				{
					if (Triangles.IsValidIndex(TriangleIndex))
					{
						ReferencedMaterialSet.Add(Triangles[TriangleIndex].MaterialIndex);
					}
				}
				for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjectionsToBake)
				{
					if (Triangles.IsValidIndex(Projection.TriangleIndex))
					{
						ReferencedMaterialSet.Add(Triangles[Projection.TriangleIndex].MaterialIndex);
					}
				}
				TArray<int32> ReferencedMaterialIndices = ReferencedMaterialSet.Array();
				ReferencedMaterialIndices.Sort();

				if (!ReferencedMaterialIndices.IsEmpty())
				{
					struct FDepthCorrectMaterialStorage
					{
						UMaterialInterface* MaterialInterface = nullptr;
						FMeshDescription MeshDescription;
						TArray<FVector2D> CustomTileUVs;
						TArray<int32> RasterSourceTriangleIndices;
						FMeshData MeshSettings;
					};

					TArray<TUniquePtr<FDepthCorrectMaterialStorage>> MaterialStorage;
					MaterialStorage.Reserve(ReferencedMaterialIndices.Num());
					FFoliageBakerDepthCorrectTileRequest DepthCorrectRequest;
					DepthCorrectRequest.TextureSize = TileSize;
					DepthCorrectRequest.CaptureRayDirection = CaptureRayDirection;
					DepthCorrectRequest.SourceBounds = SourceLODBounds;
					DepthCorrectRequest.bBakeBaseColor = OutputSelection.bBaseColorOpacity;
					DepthCorrectRequest.bBakeObjectSpaceNormal = OutputSelection.bNormalMask;
					DepthCorrectRequest.bBakePackedMix = OutputSelection.bMix;
					DepthCorrectRequest.Materials.Reserve(ReferencedMaterialIndices.Num());

					for (const int32 MaterialIndex : ReferencedMaterialIndices)
					{
						if (!SourceMaterials.IsValidIndex(MaterialIndex))
						{
							OutError = FString::Printf(
								TEXT("Card depth-correct material bake references invalid material index %d on plane %d (%s)."),
								MaterialIndex,
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"));
							return false;
						}
						TUniquePtr<FDepthCorrectMaterialStorage> Storage =
							MakeUnique<FDepthCorrectMaterialStorage>();
						Storage->MaterialInterface = SourceMaterials[MaterialIndex].MaterialInterface;
						if (!Storage->MaterialInterface)
						{
							Storage->MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
						}

						UE::FoliageBaker::ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
						ProjectedBakeParams.TileSize = TileSize;
						ProjectedBakeParams.CaptureRayDirection = CaptureRayDirection;
						ProjectedBakeParams.AtlasVConvention = Settings.AtlasVConvention;
						ProjectedBakeParams.MaterialIndexFilter = MaterialIndex;
						ProjectedBakeParams.bBackSide = bBackSide;
						ProjectedBakeParams.bBuildNormalBasisMap = false;

						TArray<UE::FoliageBaker::ProjectedMaterialBake::FNormalBasisSample> UnusedNormalBasisMap;
						int32 MatchingTriangleCount = 0;
						FString ProjectedInputError;
						if (!UE::FoliageBaker::ProjectedMaterialBake::BuildPlaneSideBakeInputs(
								Triangles,
								PrimaryTriangleIndices,
								CrackReductionProjectionsToBake,
								PlaneInfo,
								ProjectedBakeParams,
								Storage->MeshDescription,
								Storage->CustomTileUVs,
								UnusedNormalBasisMap,
								MatchingTriangleCount,
								&ProjectedInputError,
								&Storage->RasterSourceTriangleIndices))
						{
							OutError = FString::Printf(
								TEXT("Card depth-correct material input failed for plane %d (%s), material %d: %s"),
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"),
								MaterialIndex,
								*ProjectedInputError);
							return false;
						}
						OutStats.RasterizedTriangleReferences += MatchingTriangleCount;
						if (Storage->MaterialInterface->GetBlendMode() == BLEND_Masked)
						{
							OutStats.MaskedMaterialBakeReferences += MatchingTriangleCount;
						}

						Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
						Storage->MeshSettings.Mesh = &SourceStaticMesh;
						Storage->MeshSettings.MaterialIndices.Add(0);
						Storage->MeshSettings.TextureCoordinateBox =
							FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
						Storage->MeshSettings.TextureCoordinateIndex = 0;
						Storage->MeshSettings.LightMapIndex = 0;
						Storage->MeshSettings.PrimitiveData = FPrimitiveData(SourceLODBounds);
						Storage->MeshSettings.CustomTextureCoordinates = MoveTemp(Storage->CustomTileUVs);
						FDepthCorrectMaterialStorage* StoragePtr = Storage.Get();
						MaterialStorage.Add(MoveTemp(Storage));

						FFoliageBakerDepthCorrectTileMaterialInput& MaterialInput =
							DepthCorrectRequest.Materials.AddDefaulted_GetRef();
						MaterialInput.MaterialInterface = StoragePtr->MaterialInterface;
						MaterialInput.MeshSettings = &StoragePtr->MeshSettings;
						MaterialInput.RasterSourceTriangleIndices =
							&StoragePtr->RasterSourceTriangleIndices;
					}

					FFoliageBakerDepthCorrectTileResult DepthCorrectResult;
					FString DepthCorrectError;
					if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
							DepthCorrectRequest,
							DepthCorrectResult,
							&DepthCorrectError))
					{
						OutError = FString::Printf(
							TEXT("Card depth-correct tile bake failed for plane %d (%s): %s"),
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"),
							*DepthCorrectError);
						return false;
					}
					if (DepthCorrectResult.SourceTriangleIdAndDepth.Num() != TilePixelCount
						|| (OutputSelection.bBaseColorOpacity
							&& DepthCorrectResult.BaseColor.Num() != TilePixelCount)
						|| (OutputSelection.bNormalMask
							&& DepthCorrectResult.ObjectSpaceNormal.Num() != TilePixelCount)
						|| (OutputSelection.bMix
							&& DepthCorrectResult.PackedMix.Num() != TilePixelCount))
					{
						OutError = FString::Printf(
							TEXT("Card depth-correct tile returned invalid sizes for plane %d (%s): base=%d, id=%d, normal=%d, mix=%d, expected=%d."),
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"),
							DepthCorrectResult.BaseColor.Num(),
							DepthCorrectResult.SourceTriangleIdAndDepth.Num(),
							DepthCorrectResult.ObjectSpaceNormal.Num(),
							DepthCorrectResult.PackedMix.Num(),
							TilePixelCount);
						return false;
					}
					ProjectedMixTile = MoveTemp(DepthCorrectResult.PackedMix);

					for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
					{
						for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
						{
							const int32 TilePixelIndex = LocalY * TileSize.X + LocalX;
							const int32 SourceTriangleIndex =
								FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
									DepthCorrectResult.SourceTriangleIdAndDepth[TilePixelIndex]);
							if (SourceTriangleIndex == INDEX_NONE)
							{
								continue;
							}
							if (!Triangles.IsValidIndex(SourceTriangleIndex))
							{
								OutError = FString::Printf(
									TEXT("Card depth-correct tile decoded invalid triangle %d at pixel (%d,%d) for plane %d (%s)."),
									SourceTriangleIndex,
									LocalX,
									LocalY,
									PlaneInfo.SourcePlaneIndex,
									bBackSide ? TEXT("back") : TEXT("front"));
								return false;
							}

							const UE::FoliageBaker::PlaneCover::FSourceTriangle& SourceTriangle =
								Triangles[SourceTriangleIndex];
							FCardVisibleFragment& GpuFragment = VisibleFragments[TilePixelIndex];
							if (OutputSelection.bBaseColorOpacity)
							{
								GpuFragment.BaseColor = DepthCorrectResult.BaseColor[TilePixelIndex];
							}
							GpuFragment.ClassificationValue = SourceTriangle.bTrunkCardOnly ? 128 : 255;
							GpuFragment.bVisible = true;
							if (OutputSelection.bNormalMask)
							{
								ProjectedNormalTile[TilePixelIndex] =
									DepthCorrectResult.ObjectSpaceNormal[TilePixelIndex];
							}
						}
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
					if (!VisibleFragments[TilePixelIndex].IsValid())
					{
						continue;
					}
					const FCardVisibleFragment& Fragment = VisibleFragments[TilePixelIndex];
					const int32 AtlasPixelIndex = AtlasY * OutStats.Width + AtlasX;

					if (OutputSelection.bBaseColorOpacity)
					{
						FColor Color = Fragment.BaseColor;
						Color.A = Fragment.ClassificationValue;
						OutPixels[AtlasPixelIndex] = Color;
					}
					if (AtlasCoverage.IsValidIndex(AtlasPixelIndex))
					{
						AtlasCoverage[AtlasPixelIndex] = true;
					}

					if (OutputSelection.bNormalMask)
					{
						if (ProjectedNormalTile.IsValidIndex(TilePixelIndex))
						{
							FColor ProjectedNormal = ProjectedNormalTile[TilePixelIndex];
							ProjectedNormal.A = Fragment.ClassificationValue;
							OutNormalPixels[AtlasPixelIndex] = ProjectedNormal;
							if (NormalCoverage.IsValidIndex(AtlasPixelIndex))
							{
								NormalCoverage[AtlasPixelIndex] = true;
							}
						}
					}

					if (OutputSelection.bMix)
					{
						OutMixPixels[AtlasPixelIndex] = ProjectedMixTile[TilePixelIndex];
					}
					++OutStats.PaintedPixels;
				}
			}
			return true;
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			if (!BakePlaneAndSide(
				PlaneInfo,
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				-PlaneInfo.Normal,
				false))
			{
				return false;
			}
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				if (!BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal,
					true))
				{
					return false;
				}
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
		return true;
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
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const FColor MipBackgroundColor,
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
		Params.MipBackgroundColor = MipBackgroundColor;
		Params.bNormalizeMipNormals = LODGroup == TEXTUREGROUP_WorldNormalMap;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			Params.MipTileRects.Add(FIntRect(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasPixelMin + PlaneInfo.AtlasTileSize));
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				Params.MipTileRects.Add(FIntRect(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasPixelMin + PlaneInfo.BackAtlasTileSize));
			}
		}
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
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
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
			PlaneInfos,
			FColor(0, 0, 0, 0),
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
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
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
			PlaneInfos,
			FColor(128, 128, 255, 0),
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
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
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
			PlaneInfos,
			FColor(255, 128, 0, 0),
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
			FAtlasBakeStats& AtlasStats) -> bool
		{
			return BakeCardAtlasOrthographic(
				StaticMesh,
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
				AtlasStats,
				OutError);
		};

		int32 AlphaAwareCroppedPlaneCount = 0;
		const int32 EffectiveAlphaCropGuardPixels =
			CoverData.Settings.AlphaAwareTileCropGuardPixels;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			constexpr uint8 AlphaCropThreshold = 1;
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;
			CropOutputSelection.bNormalMask = false;
			CropOutputSelection.bMix = false;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			FAtlasBakeStats CropStats;
			if (!BakeFeatureAtlas(
				CropOutputSelection,
				false,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropStats))
			{
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				EffectiveAlphaCropGuardPixels,
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

		if (!BakeFeatureAtlas(
			OutData.OutputSelection,
			true,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.AtlasStats))
		{
			return false;
		}
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? EffectiveAlphaCropGuardPixels
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
				MeshData.PlaneInfos,
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
				MeshData.PlaneInfos,
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
				MeshData.PlaneInfos,
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
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, rasterized refs=%d, masked refs=%d, shooting=%s, resolve=%s\n  base/color SDF atlas: %s, RGB=BaseColor, A=whole-vegetation Union SDF (outside 0, contour 0.5, inside 1), SDF range=%d px\n  normal/trunk-leaf atlas: %s, RGB=object/local-space normal, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; Single Billboard uses one baked side\n  material instance: %s (copied from the supplied MIC template; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals"),
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
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? TEXT("dedicated fixed-axis orthographic capture, all selected-LOD triangles, WPO disabled")
				: TEXT("dedicated fixed-angle orthographic capture, front and back per plane, all selected-LOD triangles, WPO disabled"),
			TEXT("one shared per-tile masked RDG depth winner supplies BaseColor, object normal, source triangle ID, and packed Mix; no CPU material-property fallback"),
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
