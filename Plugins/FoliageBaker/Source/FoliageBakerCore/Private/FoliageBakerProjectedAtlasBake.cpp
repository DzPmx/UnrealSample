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

		FString GetDiagnosticName(const FRequest& Request)
		{
			return Request.DiagnosticName.IsEmpty()
				? FString(TEXT("Projected atlas"))
				: Request.DiagnosticName;
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
			const FRequest& Request;
			const UStaticMesh& SourceStaticMesh;
			const TArray<PlaneCover::FSourceTriangle>& Triangles;
			const PlaneCover::FPlaneProxySettings& Settings;
			const TArray<FStaticMaterial>& SourceMaterials;
			const FString& DiagnosticName;
			FResult& OutResult;
			FStats& Stats;
			FString& OutError;
			TBitArray<>& AtlasCoverage;
			TBitArray<>& NormalCoverage;
		};

		bool BakePlaneSide(
			FAtlasBakeContext& Context,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide)
		{
			const FRequest& Request = Context.Request;
			const UStaticMesh& SourceStaticMesh = Context.SourceStaticMesh;
			const TArray<PlaneCover::FSourceTriangle>& Triangles = Context.Triangles;
			const PlaneCover::FPlaneProxySettings& Settings = Context.Settings;
			const TArray<FStaticMaterial>& SourceMaterials = Context.SourceMaterials;
			const FString& DiagnosticName = Context.DiagnosticName;
			FResult& OutResult = Context.OutResult;
			FStats& Stats = Context.Stats;
			FString& OutError = Context.OutError;
			TBitArray<>& AtlasCoverage = Context.AtlasCoverage;
			TBitArray<>& NormalCoverage = Context.NormalCoverage;
			if (TileSize.X <= 0
				|| TileSize.Y <= 0
				|| FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				return true;
			}

			const FPlaneFragments Fragments = CollectPlaneFragments(
				Triangles,
				PlaneInfo,
				Request.bIncludeCrackReductionForTrunkCards,
				Stats);
			if (Fragments.TriangleIndices.IsEmpty()
				&& Fragments.CrackReductionProjections.IsEmpty())
			{
				return true;
			}

			const TArray<int32> MaterialIndices =
				CollectMaterialIndices(
					Triangles,
					Fragments);
			TArray<TUniquePtr<FMaterialBakeStorage>> MaterialStorage;
			MaterialStorage.Reserve(MaterialIndices.Num());

			FFoliageBakerDepthCorrectTileRequest TileRequest;
			TileRequest.TextureSize = TileSize;
			TileRequest.CaptureRayDirection = CaptureRayDirection;
			TileRequest.SourceBounds = Request.SourceLODBounds;
			TileRequest.bBakeBaseColor =
				Request.OutputSelection.bBaseColorOpacity;
			TileRequest.bBakeObjectSpaceNormal =
				Request.OutputSelection.bNormalMask;
			TileRequest.bBakePackedMix = Request.OutputSelection.bMix;
			TileRequest.bBakeRoughnessSpecular =
				Request.OutputSelection.bMaterialScalarAverages;
			TileRequest.Materials.Reserve(MaterialIndices.Num());

			for (const int32 MaterialIndex : MaterialIndices)
			{
				if (!SourceMaterials.IsValidIndex(MaterialIndex)
					&& Request.InvalidMaterialPolicy == EInvalidMaterialPolicy::Fail)
				{
					OutError = FString::Printf(
						TEXT("%s references invalid material index %d on plane %d (%s)."),
						*DiagnosticName,
						MaterialIndex,
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"));
					return false;
				}

				TUniquePtr<FMaterialBakeStorage> Storage =
					MakeUnique<FMaterialBakeStorage>();
				Storage->MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (!Storage->MaterialInterface)
				{
					Storage->MaterialInterface =
						UMaterial::GetDefaultMaterial(MD_Surface);
				}

				ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
				ProjectedBakeParams.CaptureRayDirection = CaptureRayDirection;
				ProjectedBakeParams.AtlasVConvention = Settings.AtlasVConvention;
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
					OutError = FString::Printf(
						TEXT("%s material input failed for plane %d (%s), material %d: %s"),
						*DiagnosticName,
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						*ProjectedInputError);
					return false;
				}

				Stats.RasterizedTriangleReferences += MatchingTriangleCount;
				if (Storage->MaterialInterface->GetBlendMode() == BLEND_Masked)
				{
					Stats.MaskedMaterialBakeReferences += MatchingTriangleCount;
				}

				Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
				Storage->MeshSettings.Mesh = &SourceStaticMesh;
				Storage->MeshSettings.MaterialIndices.Add(0);
				Storage->MeshSettings.TextureCoordinateBox =
					FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				Storage->MeshSettings.TextureCoordinateIndex = 0;
				Storage->MeshSettings.LightMapIndex = 0;
				Storage->MeshSettings.PrimitiveData =
					FPrimitiveData(Request.SourceLODBounds);
				Storage->MeshSettings.CustomTextureCoordinates =
					MoveTemp(Storage->CustomTileUVs);

				FMaterialBakeStorage* StoragePtr = Storage.Get();
				MaterialStorage.Add(MoveTemp(Storage));
				FFoliageBakerDepthCorrectTileMaterialInput& MaterialInput =
					TileRequest.Materials.AddDefaulted_GetRef();
				MaterialInput.MaterialInterface = StoragePtr->MaterialInterface;
				MaterialInput.MeshSettings = &StoragePtr->MeshSettings;
				MaterialInput.RasterSourceTriangleIndices =
					&StoragePtr->RasterSourceTriangleIndices;
			}

			if (TileRequest.Materials.IsEmpty())
			{
				return true;
			}

			FFoliageBakerDepthCorrectTileResult TileResult;
			FString TileError;
			if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
					TileRequest,
					TileResult,
					&TileError))
			{
				OutError = FString::Printf(
					TEXT("%s tile bake failed for plane %d (%s): %s"),
					*DiagnosticName,
					PlaneInfo.SourcePlaneIndex,
					bBackSide ? TEXT("back") : TEXT("front"),
					*TileError);
				return false;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			if (!ValidateTileResult(
					TileResult,
					Request.OutputSelection,
					TilePixelCount,
					DiagnosticName,
					PlaneInfo,
					bBackSide,
					OutError))
			{
				return false;
			}

			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = TilePixelMin.Y + LocalY;
				if (AtlasY < 0 || AtlasY >= Stats.Height)
				{
					continue;
				}
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 AtlasX = TilePixelMin.X + LocalX;
					if (AtlasX < 0 || AtlasX >= Stats.Width)
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
						OutError = FString::Printf(
							TEXT("%s decoded invalid triangle %d at pixel (%d,%d) for plane %d (%s)."),
							*DiagnosticName,
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
					const int32 AtlasPixelIndex = AtlasY * Stats.Width + AtlasX;

					if (Request.OutputSelection.bMaterialScalarAverages)
					{
						Stats.MaterialAverages.AddSample(
							SourceTriangle.bIsTrunk,
							TileResult.Roughness[TilePixelIndex].R,
							TileResult.Specular[TilePixelIndex].R);
					}
					if (Request.OutputSelection.bBaseColorOpacity)
					{
						FColor Color = TileResult.BaseColor[TilePixelIndex];
						Color.A = ClassificationValue;
						OutResult.BaseColorOpacityPixels[AtlasPixelIndex] = Color;
					}
					AtlasCoverage[AtlasPixelIndex] = true;

					if (Request.bCaptureSourceTriangleIdAndDepth)
					{
						OutResult.SourceTriangleIdAndDepthPixels[AtlasPixelIndex] =
							TileResult.SourceTriangleIdAndDepth[TilePixelIndex];
					}
					if (Request.OutputSelection.bNormalMask)
					{
						FColor Normal = TileResult.ObjectSpaceNormal[TilePixelIndex];
						if (Request.bConvertNormalsToCaptureFrame)
						{
							Normal = ConvertEncodedObjectSpaceNormalToCaptureFrame(
								Normal,
								CaptureRayDirection);
						}
						Normal.A = Request.NormalAlphaMode
							== ENormalAlphaMode::TrunkLeafClassification
							? ClassificationValue
							: TileResult.SourceTriangleIdAndDepth[TilePixelIndex].A;
						OutResult.NormalPixels[AtlasPixelIndex] = Normal;
						NormalCoverage[AtlasPixelIndex] = true;
					}
					if (Request.OutputSelection.bMix)
					{
						OutResult.MixPixels[AtlasPixelIndex] =
							TileResult.PackedMix[TilePixelIndex];
					}
					++Stats.PaintedPixels;
				}
			}
			return true;
		}
	}

	bool Bake(const FRequest& Request, FResult& OutResult, FString& OutError)
	{
		OutResult = FResult();
		OutError.Reset();

		const FString DiagnosticName = GetDiagnosticName(Request);
		if (!Request.SourceStaticMesh
			|| !Request.Triangles
			|| !Request.PlaneInfos
			|| !Request.ProxyStats
			|| !Request.Settings)
		{
			OutError = FString::Printf(
				TEXT("%s request is missing required source or atlas data."),
				*DiagnosticName);
			return false;
		}

		const UStaticMesh& SourceStaticMesh = *Request.SourceStaticMesh;
		const TArray<PlaneCover::FSourceTriangle>& Triangles = *Request.Triangles;
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos = *Request.PlaneInfos;
		const PlaneCover::FPlaneProxyMeshStats& ProxyStats = *Request.ProxyStats;
		const PlaneCover::FPlaneProxySettings& Settings = *Request.Settings;
		FStats& Stats = OutResult.Stats;
		Stats.Width = ProxyStats.AtlasWidth;
		Stats.Height = ProxyStats.AtlasHeight;
		Stats.TileResolution = ProxyStats.AtlasTileResolution;
		Stats.MaterialAlphaPolicyDetails = Request.MaterialAlphaPolicyDetails;

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
		if (Request.OutputSelection.bNormalMask)
		{
			OutResult.NormalPixels.Init(
				ProjectedMaterialBake::EncodeObjectSpaceNormalToColor(
					FVector::UpVector,
					255),
				AtlasPixelCount);
		}
		if (Request.OutputSelection.bMix)
		{
			OutResult.MixPixels.Init(FColor(255, 128, 0, 0), AtlasPixelCount);
		}
		if (Request.bCaptureSourceTriangleIdAndDepth)
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
			Request,
			SourceStaticMesh,
			Triangles,
			Settings,
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
		if (Request.OutputSelection.bNormalMask)
		{
			Atlas::FillTransparentRGBInsideTiles(
				OutResult.NormalPixels,
				Stats.Width,
				Stats.Height,
				PlaneInfos,
				&NormalCoverage,
				false);
			const uint8 UncoveredAlpha = Request.NormalAlphaMode
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
		if (Request.OutputSelection.bMix)
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
