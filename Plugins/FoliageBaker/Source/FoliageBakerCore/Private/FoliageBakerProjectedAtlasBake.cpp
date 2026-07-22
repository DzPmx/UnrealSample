#include "FoliageBakerProjectedAtlasBake.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "Engine/StaticMesh.h"
#include "MaterialBakingStructures.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"

namespace UE::FoliageBaker::ProjectedAtlasBake
{
	namespace
	{
		struct FPlaneFragments
		{
			TArray<int32> TriangleIndices;
			TArray<PlaneCover::FCrackReductionProjection> CrackReductionProjections;
		};

		struct FMaterialBakeStorage
		{
			UMaterialInterface* MaterialInterface = nullptr;
			FMeshDescription MeshDescription;
			TArray<FVector2D> CustomTileUVs;
			TArray<int32> RasterSourceTriangleIndices;
			FMeshData MeshSettings;
		};

		FString GetDiagnosticName(const FPolicy& Policy)
		{
			return Policy.DiagnosticName.IsEmpty()
				? FString(TEXT("Projected atlas"))
				: Policy.DiagnosticName;
		}

		FColor ConvertEncodedObjectSpaceNormalToCaptureFrame(
			const FColor& EncodedNormal,
			const FVector& CaptureRayDirection)
		{
			const FVector ObjectSpaceNormal = FVector(
				static_cast<double>(EncodedNormal.R) / 255.0 * 2.0 - 1.0,
				static_cast<double>(EncodedNormal.G) / 255.0 * 2.0 - 1.0,
				static_cast<double>(EncodedNormal.B) / 255.0 * 2.0 - 1.0)
				.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			const FVector Facing = (-CaptureRayDirection).GetSafeNormal();
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
			if (Facing.IsNearlyZero() || Right.IsNearlyZero())
			{
				return EncodedNormal;
			}

			const FVector CaptureFrameNormal(
				FVector::DotProduct(ObjectSpaceNormal, Facing),
				FVector::DotProduct(ObjectSpaceNormal, Right),
				FVector::DotProduct(ObjectSpaceNormal, FVector::UpVector));
			return ProjectedMaterialBake::EncodeObjectSpaceNormalToColor(
				CaptureFrameNormal,
				EncodedNormal.A);
		}

		FPlaneFragments CollectPlaneFragments(
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const bool bIncludeCrackReductionForTrunkCards,
			FStats& InOutStats)
		{
			FPlaneFragments Result;
			Result.TriangleIndices.Reserve(PlaneInfo.TriangleIndices.Num());
			Result.CrackReductionProjections.Reserve(
				PlaneInfo.CrackReductionProjections.Num());

			TBitArray<> QueuedTriangles;
			QueuedTriangles.Init(false, Triangles.Num());
			for (const int32 TriangleIndex : PlaneInfo.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
				{
					QueuedTriangles[TriangleIndex] = true;
					Result.TriangleIndices.Add(TriangleIndex);
				}
			}

			if (bIncludeCrackReductionForTrunkCards || !PlaneInfo.bIsTrunkCard)
			{
				for (const PlaneCover::FCrackReductionProjection& Projection :
					PlaneInfo.CrackReductionProjections)
				{
					const int32 TriangleIndex = Projection.TriangleIndex;
					if (Triangles.IsValidIndex(TriangleIndex)
						&& !QueuedTriangles[TriangleIndex])
					{
						QueuedTriangles[TriangleIndex] = true;
						Result.CrackReductionProjections.Add(Projection);
						++InOutStats.CrackReductionTriangleReferences;
					}
				}
			}
			return Result;
		}

		TArray<int32> CollectMaterialIndices(
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const FPlaneFragments& Fragments)
		{
			TSet<int32> MaterialIndices;
			for (const int32 TriangleIndex : Fragments.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex))
				{
					MaterialIndices.Add(Triangles[TriangleIndex].MaterialIndex);
				}
			}
			for (const PlaneCover::FCrackReductionProjection& Projection :
				Fragments.CrackReductionProjections)
			{
				if (Triangles.IsValidIndex(Projection.TriangleIndex))
				{
					MaterialIndices.Add(
						Triangles[Projection.TriangleIndex].MaterialIndex);
				}
			}

			TArray<int32> Result = MaterialIndices.Array();
			Result.Sort();
			return Result;
		}

		bool ValidateTileResult(
			const FFoliageBakerDepthCorrectTileResult& Result,
			const MaterialResolver::FMaterialOutputSelection& OutputSelection,
			const int32 ExpectedPixelCount,
			const FString& DiagnosticName,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const bool bBackSide,
			FString& OutError)
		{
			if (Result.SourceTriangleIdAndDepth.Num() == ExpectedPixelCount
				&& (!OutputSelection.bBaseColorOpacity
					|| Result.BaseColor.Num() == ExpectedPixelCount)
				&& (!OutputSelection.bNormalMask
					|| Result.ObjectSpaceNormal.Num() == ExpectedPixelCount)
				&& (!OutputSelection.bMix
					|| Result.PackedMix.Num() == ExpectedPixelCount)
				&& (!OutputSelection.bMaterialScalarAverages
					|| (Result.Roughness.Num() == ExpectedPixelCount
						&& Result.Specular.Num() == ExpectedPixelCount)))
			{
				return true;
			}

			OutError = FString::Printf(
				TEXT("%s depth-correct tile returned invalid sizes for plane %d (%s): ")
				TEXT("base=%d, id=%d, normal=%d, mix=%d, roughness=%d, ")
				TEXT("specular=%d, expected=%d."),
				*DiagnosticName,
				PlaneInfo.SourcePlaneIndex,
				bBackSide ? TEXT("back") : TEXT("front"),
				Result.BaseColor.Num(),
				Result.SourceTriangleIdAndDepth.Num(),
				Result.ObjectSpaceNormal.Num(),
				Result.PackedMix.Num(),
				Result.Roughness.Num(),
				Result.Specular.Num(),
				ExpectedPixelCount);
			return false;
		}

		struct FAtlasBakeContext
		{
			const FInputs& Inputs;
			const FPolicy& Policy;
			const TArray<FStaticMaterial>& SourceMaterials;
			const FString& DiagnosticName;
			FResult& OutResult;
			FStats& Stats;
			FString& OutError;
			TBitArray<>& AtlasCoverage;
			TBitArray<>& NormalCoverage;
		};

		struct FPlaneSideTileInputs
		{
			TArray<TUniquePtr<FMaterialBakeStorage>> MaterialStorage;
			FFoliageBakerDepthCorrectTileRequest TileRequest;
		};

		bool BuildPlaneSideTileInputs(
			FAtlasBakeContext& Context,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide,
			const FPlaneFragments& Fragments,
			FPlaneSideTileInputs& OutInputs)
		{
			const FInputs& Inputs = Context.Inputs;
			const FPolicy& Policy = Context.Policy;
			const TArray<PlaneCover::FSourceTriangle>& Triangles = Inputs.Triangles;
			const TArray<int32> MaterialIndices =
				CollectMaterialIndices(Triangles, Fragments);

			OutInputs.MaterialStorage.Reserve(MaterialIndices.Num());
			FFoliageBakerDepthCorrectTileRequest& TileRequest = OutInputs.TileRequest;
			TileRequest.TextureSize = TileSize;
			TileRequest.CaptureRayDirection = CaptureRayDirection;
			TileRequest.SourceBounds = Inputs.SourceLODBounds;
			TileRequest.bBakeBaseColor = Policy.OutputSelection.bBaseColorOpacity;
			TileRequest.bBakeObjectSpaceNormal = Policy.OutputSelection.bNormalMask;
			TileRequest.bBakePackedMix = Policy.OutputSelection.bMix;
			TileRequest.bBakeRoughnessSpecular =
				Policy.OutputSelection.bMaterialScalarAverages;
			TileRequest.Materials.Reserve(MaterialIndices.Num());

			for (const int32 MaterialIndex : MaterialIndices)
			{
				if (!Context.SourceMaterials.IsValidIndex(MaterialIndex)
					&& Policy.InvalidMaterialPolicy == EInvalidMaterialPolicy::Fail)
				{
					Context.OutError = FString::Printf(
						TEXT("%s references invalid material index %d on plane %d (%s)."),
						*Context.DiagnosticName,
						MaterialIndex,
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"));
					return false;
				}

				TUniquePtr<FMaterialBakeStorage> Storage =
					MakeUnique<FMaterialBakeStorage>();
				Storage->MaterialInterface = Context.SourceMaterials.IsValidIndex(MaterialIndex)
					? Context.SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (!Storage->MaterialInterface)
				{
					Storage->MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
				}

				ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
				ProjectedBakeParams.CaptureRayDirection = CaptureRayDirection;
				ProjectedBakeParams.AtlasVConvention = Inputs.Settings.AtlasVConvention;
				ProjectedBakeParams.MaterialIndexFilter = MaterialIndex;
				ProjectedBakeParams.bBackSide = bBackSide;

				int32 MatchingTriangleCount = 0;
				FString ProjectedInputError;
				const bool bBuiltInput =
					ProjectedMaterialBake::BuildPlaneSideBakeInputs(
						Triangles,
						Fragments.TriangleIndices,
						Fragments.CrackReductionProjections,
						PlaneInfo,
						ProjectedBakeParams,
						Storage->MeshDescription,
						Storage->CustomTileUVs,
						MatchingTriangleCount,
						&ProjectedInputError,
						&Storage->RasterSourceTriangleIndices);
				if (MatchingTriangleCount == 0)
				{
					continue;
				}
				if (!bBuiltInput)
				{
					Context.OutError = FString::Printf(
						TEXT("%s material input failed for plane %d (%s), material %d: %s"),
						*Context.DiagnosticName,
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						*ProjectedInputError);
					return false;
				}

				Context.Stats.RasterizedTriangleReferences += MatchingTriangleCount;
				if (Storage->MaterialInterface->GetBlendMode() == BLEND_Masked)
				{
					Context.Stats.MaskedMaterialBakeReferences += MatchingTriangleCount;
				}

				Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
				Storage->MeshSettings.Mesh = &Inputs.SourceStaticMesh;
				Storage->MeshSettings.MaterialIndices.Add(0);
				Storage->MeshSettings.TextureCoordinateBox =
					FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				Storage->MeshSettings.TextureCoordinateIndex = 0;
				Storage->MeshSettings.LightMapIndex = 0;
				Storage->MeshSettings.PrimitiveData =
					FPrimitiveData(Inputs.SourceLODBounds);
				Storage->MeshSettings.CustomTextureCoordinates =
					MoveTemp(Storage->CustomTileUVs);

				FMaterialBakeStorage* StoragePtr = Storage.Get();
				OutInputs.MaterialStorage.Add(MoveTemp(Storage));
				FFoliageBakerDepthCorrectTileMaterialInput& MaterialInput =
					TileRequest.Materials.AddDefaulted_GetRef();
				MaterialInput.MaterialInterface = StoragePtr->MaterialInterface;
				MaterialInput.MeshSettings = &StoragePtr->MeshSettings;
				MaterialInput.RasterSourceTriangleIndices =
					&StoragePtr->RasterSourceTriangleIndices;
			}
			return true;
		}

		bool BakePlaneSideTile(
			FAtlasBakeContext& Context,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const bool bBackSide,
			const FPlaneSideTileInputs& Inputs,
			FFoliageBakerDepthCorrectTileResult& OutResult)
		{
			FString TileError;
			if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
					Inputs.TileRequest,
					OutResult,
					&TileError))
			{
				Context.OutError = FString::Printf(
					TEXT("%s tile bake failed for plane %d (%s): %s"),
					*Context.DiagnosticName,
					PlaneInfo.SourcePlaneIndex,
					bBackSide ? TEXT("back") : TEXT("front"),
					*TileError);
				return false;
			}

			const int32 ExpectedPixelCount =
				Inputs.TileRequest.TextureSize.X * Inputs.TileRequest.TextureSize.Y;
			return ValidateTileResult(
				OutResult,
				Context.Policy.OutputSelection,
				ExpectedPixelCount,
				Context.DiagnosticName,
				PlaneInfo,
				bBackSide,
				Context.OutError);
		}

		bool ResolvePlaneSideTile(
			FAtlasBakeContext& Context,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide,
			const FFoliageBakerDepthCorrectTileResult& TileResult)
		{
			const FPolicy& Policy = Context.Policy;
			const TArray<PlaneCover::FSourceTriangle>& Triangles =
				Context.Inputs.Triangles;
			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = TilePixelMin.Y + LocalY;
				if (AtlasY < 0 || AtlasY >= Context.Stats.Height)
				{
					continue;
				}
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 AtlasX = TilePixelMin.X + LocalX;
					if (AtlasX < 0 || AtlasX >= Context.Stats.Width)
					{
						continue;
					}

					const int32 TilePixelIndex = LocalY * TileSize.X + LocalX;
					const int32 SourceTriangleIndex =
						FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
							TileResult.SourceTriangleIdAndDepth[TilePixelIndex]);
					if (SourceTriangleIndex == INDEX_NONE)
					{
						continue;
					}
					if (!Triangles.IsValidIndex(SourceTriangleIndex))
					{
						Context.OutError = FString::Printf(
							TEXT("%s decoded invalid triangle %d at pixel (%d,%d) for plane %d (%s)."),
							*Context.DiagnosticName,
							SourceTriangleIndex,
							LocalX,
							LocalY,
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"));
						return false;
					}

					const PlaneCover::FSourceTriangle& SourceTriangle =
						Triangles[SourceTriangleIndex];
					const uint8 ClassificationValue =
						Atlas::EncodeTrunkLeafAlpha(SourceTriangle.bIsTrunk);
					const int32 AtlasPixelIndex =
						AtlasY * Context.Stats.Width + AtlasX;

					if (Policy.OutputSelection.bMaterialScalarAverages)
					{
						Context.Stats.MaterialAverages.AddSample(
							SourceTriangle.bIsTrunk,
							TileResult.Roughness[TilePixelIndex].R,
							TileResult.Specular[TilePixelIndex].R);
					}
					if (Policy.OutputSelection.bBaseColorOpacity)
					{
						FColor Color = TileResult.BaseColor[TilePixelIndex];
						Color.A = ClassificationValue;
						Context.OutResult.BaseColorOpacityPixels[AtlasPixelIndex] = Color;
					}
					Context.AtlasCoverage[AtlasPixelIndex] = true;

					if (Policy.bCaptureSourceTriangleIdAndDepth)
					{
						Context.OutResult.SourceTriangleIdAndDepthPixels[AtlasPixelIndex] =
							TileResult.SourceTriangleIdAndDepth[TilePixelIndex];
					}
					if (Policy.OutputSelection.bNormalMask)
					{
						FColor Normal = TileResult.ObjectSpaceNormal[TilePixelIndex];
						if (Policy.bConvertNormalsToCaptureFrame)
						{
							Normal = ConvertEncodedObjectSpaceNormalToCaptureFrame(
								Normal,
								CaptureRayDirection);
						}
						Normal.A = Policy.NormalAlphaMode
							== ENormalAlphaMode::TrunkLeafClassification
							? ClassificationValue
							: TileResult.SourceTriangleIdAndDepth[TilePixelIndex].A;
						Context.OutResult.NormalPixels[AtlasPixelIndex] = Normal;
						Context.NormalCoverage[AtlasPixelIndex] = true;
					}
					if (Policy.OutputSelection.bMix)
					{
						Context.OutResult.MixPixels[AtlasPixelIndex] =
							TileResult.PackedMix[TilePixelIndex];
					}
					++Context.Stats.PaintedPixels;
				}
			}
			return true;
		}

		bool BakePlaneSide(
			FAtlasBakeContext& Context,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide)
		{
			if (TileSize.X <= 0
				|| TileSize.Y <= 0
				|| FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				return true;
			}

			const FPlaneFragments Fragments = CollectPlaneFragments(
				Context.Inputs.Triangles,
				PlaneInfo,
				Context.Policy.bIncludeCrackReductionForTrunkCards,
				Context.Stats);
			if (Fragments.TriangleIndices.IsEmpty()
				&& Fragments.CrackReductionProjections.IsEmpty())
			{
				return true;
			}

			FPlaneSideTileInputs TileInputs;
			if (!BuildPlaneSideTileInputs(
					Context,
					PlaneInfo,
					TileSize,
					CaptureRayDirection,
					bBackSide,
					Fragments,
					TileInputs))
			{
				return false;
			}
			if (TileInputs.TileRequest.Materials.IsEmpty())
			{
				return true;
			}

			FFoliageBakerDepthCorrectTileResult TileResult;
			return BakePlaneSideTile(
					Context,
					PlaneInfo,
					bBackSide,
					TileInputs,
					TileResult)
				&& ResolvePlaneSideTile(
					Context,
					PlaneInfo,
					TilePixelMin,
					TileSize,
					CaptureRayDirection,
					bBackSide,
					TileResult);
		}
	}

	bool Bake(
		const FInputs& Inputs,
		const FPolicy& Policy,
		FResult& OutResult,
		FString& OutError)
	{
		OutResult = FResult();
		OutError.Reset();

		const FString DiagnosticName = GetDiagnosticName(Policy);
		const UStaticMesh& SourceStaticMesh = Inputs.SourceStaticMesh;
		const TArray<PlaneCover::FSourceTriangle>& Triangles = Inputs.Triangles;
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos = Inputs.PlaneInfos;
		const PlaneCover::FPlaneProxyMeshStats& ProxyStats = Inputs.ProxyStats;
		FStats& Stats = OutResult.Stats;
		Stats.Width = ProxyStats.AtlasWidth;
		Stats.Height = ProxyStats.AtlasHeight;
		Stats.TileResolution = ProxyStats.AtlasTileResolution;
		Stats.MaterialAlphaPolicyDetails = Policy.MaterialAlphaPolicyDetails;

		const int64 AtlasPixelCount64 =
			static_cast<int64>(Stats.Width) * static_cast<int64>(Stats.Height);
		if (Stats.Width <= 0
			|| Stats.Height <= 0
			|| AtlasPixelCount64 > MAX_int32)
		{
			OutError = FString::Printf(
				TEXT("%s has invalid atlas dimensions %dx%d."),
				*DiagnosticName,
				Stats.Width,
				Stats.Height);
			return false;
		}
		const int32 AtlasPixelCount = static_cast<int32>(AtlasPixelCount64);

		OutResult.BaseColorOpacityPixels.Init(FColor(0, 0, 0, 0), AtlasPixelCount);
		if (Policy.OutputSelection.bNormalMask)
		{
			OutResult.NormalPixels.Init(
				ProjectedMaterialBake::EncodeObjectSpaceNormalToColor(
					FVector::UpVector,
					255),
				AtlasPixelCount);
		}
		if (Policy.OutputSelection.bMix)
		{
			OutResult.MixPixels.Init(FColor(255, 128, 0, 0), AtlasPixelCount);
		}
		if (Policy.bCaptureSourceTriangleIdAndDepth)
		{
			OutResult.SourceTriangleIdAndDepthPixels.Init(
				FColor::Black,
				AtlasPixelCount);
		}

		int64 PackedPaddedTilePixels = 0;
		auto AccumulateTileStats = [&Stats, &PackedPaddedTilePixels](
			const FIntPoint& TileSize,
			const int32 Padding,
			const bool bBackFace)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			const int32 SafePadding = FMath::Max(0, Padding);
			PackedPaddedTilePixels +=
				static_cast<int64>(TileSize.X + SafePadding * 2)
				* static_cast<int64>(TileSize.Y + SafePadding * 2);
			if (bBackFace)
			{
				++Stats.BackTileCount;
			}
			else
			{
				++Stats.FrontTileCount;
			}
		};
		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateTileStats(
				PlaneInfo.AtlasTileSize,
				PlaneInfo.AtlasTilePaddingPixels,
				false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileStats(
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.AtlasTilePaddingPixels,
					true);
			}
		}
		Stats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels)
				/ static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, AtlasPixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, AtlasPixelCount);
		const TArray<FStaticMaterial>& SourceMaterials =
			SourceStaticMesh.GetStaticMaterials();

		FAtlasBakeContext Context{
			Inputs,
			Policy,
			SourceMaterials,
			DiagnosticName,
			OutResult,
			Stats,
			OutError,
			AtlasCoverage,
			NormalCoverage
		};

		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			if (!BakePlaneSide(
					Context,
					PlaneInfo,
					PlaneInfo.AtlasPixelMin,
					PlaneInfo.AtlasTileSize,
					-PlaneInfo.Normal,
					false))
			{
				return false;
			}
			if (PlaneInfo.bHasBackFaceAtlas
				&& !BakePlaneSide(
					Context,
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal,
					true))
			{
				return false;
			}
		}

		Atlas::FillTransparentRGBInsideTiles(
			OutResult.BaseColorOpacityPixels,
			Stats.Width,
			Stats.Height,
			PlaneInfos);
		if (Policy.OutputSelection.bNormalMask)
		{
			Atlas::FillTransparentRGBInsideTiles(
				OutResult.NormalPixels,
				Stats.Width,
				Stats.Height,
				PlaneInfos,
				&NormalCoverage,
				false);
			const uint8 UncoveredAlpha = Policy.NormalAlphaMode
				== ENormalAlphaMode::TrunkLeafClassification
				? 0
				: 255;
			for (int32 PixelIndex = 0; PixelIndex < OutResult.NormalPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage[PixelIndex])
				{
					OutResult.NormalPixels[PixelIndex].A = UncoveredAlpha;
				}
			}
			Atlas::NormalizeEncodedObjectSpaceNormals(OutResult.NormalPixels);
		}
		if (Policy.OutputSelection.bMix)
		{
			Atlas::FillTransparentRGBInsideTiles(
				OutResult.MixPixels,
				Stats.Width,
				Stats.Height,
				PlaneInfos,
				&AtlasCoverage,
				true);
		}
		return true;
	}

	UTexture2D* CreateTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FTextureAssetRequest& Request,
		const TArray<FColor>& Pixels,
		const FStats& Stats,
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = Request.OutputFolderName;
		Params.OutputPackagePathOverride = Request.OutputPackagePathOverride;
		Params.AssetNamePrefix = Request.AssetNamePrefix;
		Params.AssetNameSuffix = Request.AssetNameSuffix;
		Params.Width = Stats.Width;
		Params.Height = Stats.Height;
		Params.CompressionSettings = Request.CompressionSettings;
		Params.LODGroup = Request.LODGroup;
		Params.bSRGB = Request.bSRGB;
		Params.SemanticMaskMipCoverageThreshold =
			Request.SemanticMaskMipCoverageThreshold;
		Params.MipBackgroundColor = Request.MipBackgroundColor;
		Params.bNormalizeMipNormals =
			Request.LODGroup == TEXTUREGROUP_WorldNormalMap;
		Params.EmptyPixelsError = Request.EmptyPixelsError;

		Params.MipTileRects.Reserve(PlaneInfos.Num() * 2);
		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			Params.MipTileRects.Add(FIntRect(
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasPixelMin + PlaneInfo.AtlasTileSize));
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				Params.MipTileRects.Add(FIntRect(
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasPixelMin + PlaneInfo.BackAtlasTileSize));
			}
		}

		return FFoliageBakerAssetBuilder::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Params,
			Pixels,
			OutError);
	}
}
