#include "FoliageBakerCardAtlas.h"

#include "FoliageBakerAtlasTools.h"
#include "StaticMeshAttributes.h"

namespace UE::FoliageBaker::Cards::Atlas
{
	int32 MergeDoublePlaneTileCrops(
		TArray<PlaneCover::FPlaneProxyTileCrop>& TileCrops)
	{
		if (TileCrops.Num() != 2 || !TileCrops[0].bEnabled || !TileCrops[1].bEnabled)
		{
			for (PlaneCover::FPlaneProxyTileCrop& Crop : TileCrops)
			{
				Crop = PlaneCover::FPlaneProxyTileCrop();
			}
			return 0;
		}

		PlaneCover::FPlaneProxyTileCrop SharedCrop;
		SharedCrop.bEnabled = true;
		SharedCrop.MinUFraction = FMath::Min(TileCrops[0].MinUFraction, TileCrops[1].MinUFraction);
		SharedCrop.MaxUFraction = FMath::Max(TileCrops[0].MaxUFraction, TileCrops[1].MaxUFraction);
		SharedCrop.MinVFraction = FMath::Min(TileCrops[0].MinVFraction, TileCrops[1].MinVFraction);
		SharedCrop.MaxVFraction = FMath::Max(TileCrops[0].MaxVFraction, TileCrops[1].MaxVFraction);

		constexpr double CropEpsilon = 1.0e-5;
		const bool bCropsTile = SharedCrop.MinUFraction > CropEpsilon
			|| SharedCrop.MaxUFraction < 1.0 - CropEpsilon
			|| SharedCrop.MinVFraction > CropEpsilon
			|| SharedCrop.MaxVFraction < 1.0 - CropEpsilon;
		if (!bCropsTile
			|| SharedCrop.MaxUFraction <= SharedCrop.MinUFraction
			|| SharedCrop.MaxVFraction <= SharedCrop.MinVFraction)
		{
			for (PlaneCover::FPlaneProxyTileCrop& Crop : TileCrops)
			{
				Crop = PlaneCover::FPlaneProxyTileCrop();
			}
			return 0;
		}

		TileCrops[0] = SharedCrop;
		TileCrops[1] = SharedCrop;
		return 2;
	}

	int32 MergeGroupedTileCrops(
		TArray<PlaneCover::FPlaneProxyTileCrop>& TileCrops,
		const TArray<int32>& PlaneGroupIndices)
	{
		if (TileCrops.Num() != PlaneGroupIndices.Num())
		{
			return 0;
		}

		TMap<int32, PlaneCover::FPlaneProxyTileCrop> SharedCrops;
		TSet<int32> GroupsWithoutCrop;
		for (int32 PlaneIndex = 0; PlaneIndex < TileCrops.Num(); ++PlaneIndex)
		{
			const int32 GroupIndex = PlaneGroupIndices[PlaneIndex];
			const PlaneCover::FPlaneProxyTileCrop& Crop = TileCrops[PlaneIndex];
			if (!Crop.bEnabled)
			{
				GroupsWithoutCrop.Add(GroupIndex);
				continue;
			}
			PlaneCover::FPlaneProxyTileCrop& SharedCrop = SharedCrops.FindOrAdd(GroupIndex);
			if (!SharedCrop.bEnabled)
			{
				SharedCrop = Crop;
			}
			else
			{
				SharedCrop.MinUFraction = FMath::Min(SharedCrop.MinUFraction, Crop.MinUFraction);
				SharedCrop.MaxUFraction = FMath::Max(SharedCrop.MaxUFraction, Crop.MaxUFraction);
				SharedCrop.MinVFraction = FMath::Min(SharedCrop.MinVFraction, Crop.MinVFraction);
				SharedCrop.MaxVFraction = FMath::Max(SharedCrop.MaxVFraction, Crop.MaxVFraction);
			}
		}

		constexpr double CropEpsilon = 1.0e-5;
		int32 CroppedPlaneCount = 0;
		for (int32 PlaneIndex = 0; PlaneIndex < TileCrops.Num(); ++PlaneIndex)
		{
			const int32 GroupIndex = PlaneGroupIndices[PlaneIndex];
			if (GroupsWithoutCrop.Contains(GroupIndex))
			{
				TileCrops[PlaneIndex] = PlaneCover::FPlaneProxyTileCrop();
				continue;
			}
			const PlaneCover::FPlaneProxyTileCrop* SharedCrop = SharedCrops.Find(GroupIndex);
			const bool bValidCrop = SharedCrop
				&& SharedCrop->bEnabled
				&& SharedCrop->MaxUFraction > SharedCrop->MinUFraction
				&& SharedCrop->MaxVFraction > SharedCrop->MinVFraction
				&& (SharedCrop->MinUFraction > CropEpsilon
					|| SharedCrop->MaxUFraction < 1.0 - CropEpsilon
					|| SharedCrop->MinVFraction > CropEpsilon
					|| SharedCrop->MaxVFraction < 1.0 - CropEpsilon);
			if (!bValidCrop)
			{
				TileCrops[PlaneIndex] = PlaneCover::FPlaneProxyTileCrop();
				continue;
			}
			TileCrops[PlaneIndex] = *SharedCrop;
			++CroppedPlaneCount;
		}
		return CroppedPlaneCount;
	}

	bool CropToUsedSpace(
		FFoliageBakerProxyGeometry& InOutGeometry,
		TArray<FColor>& BaseColorOpacityPixels,
		TArray<FColor>& NormalPixels,
		TArray<FColor>& MixPixels,
		TArray<FColor>& SourceTriangleIdAndDepthPixels,
		ProjectedAtlasBake::FStats& InOutStats,
		const EOuterCropMode CropMode,
		FString& OutError)
	{
		const int32 OldWidth = InOutStats.Width;
		const int32 OldHeight = InOutStats.Height;
		auto SynchronizeGeometryStats = [&]()
		{
			InOutGeometry.Stats.AtlasWidth = InOutStats.Width;
			InOutGeometry.Stats.AtlasHeight = InOutStats.Height;
		};
		if (OldWidth <= 0 || OldHeight <= 0 || InOutGeometry.PlaneInfos.IsEmpty())
		{
			SynchronizeGeometryStats();
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
		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : InOutGeometry.PlaneInfos)
		{
			AccumulateTileBounds(
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileBounds(
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.AtlasTilePaddingPixels);
			}
		}
		if (UsedMaxX <= UsedMinX || UsedMaxY <= UsedMinY)
		{
			OutError = TEXT("Could not determine the used atlas bounds for outer-space cropping.");
			return false;
		}

		constexpr int32 TextureBlockSize = 4;
		int32 CropMinX = 0;
		int32 CropMinY = 0;
		int32 NewWidth = OldWidth;
		int32 NewHeight = OldHeight;
		if (CropMode == EOuterCropMode::PowerOfTwoUsedBounds)
		{
			const int32 UsedWidth = UsedMaxX - UsedMinX;
			const int32 UsedHeight = UsedMaxY - UsedMinY;
			NewWidth = FMath::Min(
				OldWidth,
				static_cast<int32>(FMath::RoundUpToPowerOfTwo(
					static_cast<uint32>(FMath::Max(TextureBlockSize, UsedWidth)))));
			NewHeight = FMath::Min(
				OldHeight,
				static_cast<int32>(FMath::RoundUpToPowerOfTwo(
					static_cast<uint32>(FMath::Max(TextureBlockSize, UsedHeight)))));
			CropMinX = FMath::Clamp(UsedMinX, 0, OldWidth - NewWidth);
			CropMinY = FMath::Clamp(UsedMinY, 0, OldHeight - NewHeight);
		}
		else
		{
			CropMinX = FMath::Clamp(
				(UsedMinX / TextureBlockSize) * TextureBlockSize,
				0,
				OldWidth - 1);
			CropMinY = FMath::Clamp(
				(UsedMinY / TextureBlockSize) * TextureBlockSize,
				0,
				OldHeight - 1);
			const int32 CropMaxX = FMath::Clamp(
				((UsedMaxX + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize,
				CropMinX + 1,
				OldWidth);
			const int32 CropMaxY = FMath::Clamp(
				((UsedMaxY + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize,
				CropMinY + 1,
				OldHeight);
			NewWidth = CropMaxX - CropMinX;
			NewHeight = CropMaxY - CropMinY;
		}
		if (CropMinX == 0 && CropMinY == 0 && NewWidth == OldWidth && NewHeight == OldHeight)
		{
			SynchronizeGeometryStats();
			return true;
		}

		auto BuildCroppedPixels = [&](const TArray<FColor>& SourcePixels, TArray<FColor>& OutCroppedPixels)
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
				FMemory::Memcpy(
					DestinationRow,
					SourceRow,
					static_cast<SIZE_T>(NewWidth) * sizeof(FColor));
			}
			return true;
		};

		TArray<FColor> CroppedBaseColorOpacityPixels;
		TArray<FColor> CroppedNormalPixels;
		TArray<FColor> CroppedMixPixels;
		TArray<FColor> CroppedSourceTriangleIdAndDepthPixels;
		if (!BuildCroppedPixels(BaseColorOpacityPixels, CroppedBaseColorOpacityPixels)
			|| !BuildCroppedPixels(NormalPixels, CroppedNormalPixels)
			|| !BuildCroppedPixels(MixPixels, CroppedMixPixels)
			|| !BuildCroppedPixels(
				SourceTriangleIdAndDepthPixels,
				CroppedSourceTriangleIdAndDepthPixels))
		{
			OutError = TEXT("Atlas pixel count did not match the atlas dimensions during outer-space cropping.");
			return false;
		}

		FStaticMeshAttributes MeshAttributes(InOutGeometry.MeshDescription);
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();
		if (VertexInstanceUVs.GetNumChannels() < 2)
		{
			OutError = TEXT("Generated card mesh does not contain UV0 and UV1 for atlas cropping.");
			return false;
		}
		auto RemapAtlasUV = [&](const FVector2f& OldUV)
		{
			return FVector2f(
				(static_cast<float>(OldUV.X) * static_cast<float>(OldWidth) - static_cast<float>(CropMinX))
					/ static_cast<float>(NewWidth),
				(static_cast<float>(OldUV.Y) * static_cast<float>(OldHeight) - static_cast<float>(CropMinY))
					/ static_cast<float>(NewHeight));
		};
		for (const FVertexInstanceID VertexInstanceID :
			InOutGeometry.MeshDescription.VertexInstances().GetElementIDs())
		{
			VertexInstanceUVs.Set(
				VertexInstanceID,
				0,
				RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 0)));
			VertexInstanceUVs.Set(
				VertexInstanceID,
				1,
				RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 1)));
		}

		for (PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : InOutGeometry.PlaneInfos)
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

		BaseColorOpacityPixels = MoveTemp(CroppedBaseColorOpacityPixels);
		NormalPixels = MoveTemp(CroppedNormalPixels);
		MixPixels = MoveTemp(CroppedMixPixels);
		SourceTriangleIdAndDepthPixels = MoveTemp(CroppedSourceTriangleIdAndDepthPixels);
		InOutStats.Width = NewWidth;
		InOutStats.Height = NewHeight;

		int64 PackedPaddedTilePixels = 0;
		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : InOutGeometry.PlaneInfos)
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
		InOutStats.PackedTileUtilizationPercent = NewAtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(NewAtlasPixelCount)
			: 0.0;
		SynchronizeGeometryStats();
		return true;
	}

	bool ResizeTileIsolated(
		const TArray<FColor>& SourcePixels,
		const ProjectedAtlasBake::FStats& SourceStats,
		const TArray<PlaneCover::FPlaneProxyPlaneInfo>& SourcePlaneInfos,
		const int32 RequestedMaximumDimension,
		const FColor BackgroundColor,
		TArray<FColor>& OutPixels,
		ProjectedAtlasBake::FStats& OutStats,
		TArray<PlaneCover::FPlaneProxyPlaneInfo>& OutPlaneInfos,
		FString& OutError)
	{
		int32 OutputWidth = 0;
		int32 OutputHeight = 0;
		if (!UE::FoliageBaker::Atlas::ResizeTileIsolated(
				SourcePixels,
				SourceStats.Width,
				SourceStats.Height,
				SourcePlaneInfos,
				FMath::Clamp(RequestedMaximumDimension, 64, 1024),
				BackgroundColor,
				OutPixels,
				OutputWidth,
				OutputHeight,
				OutPlaneInfos,
				OutError))
		{
			return false;
		}

		OutStats = SourceStats;
		OutStats.Width = OutputWidth;
		OutStats.Height = OutputHeight;
		OutStats.TileResolution = FMath::Max(
			1,
			FMath::RoundToInt(
				SourceStats.TileResolution
					* FMath::Min(
						static_cast<double>(OutputWidth) / SourceStats.Width,
						static_cast<double>(OutputHeight) / SourceStats.Height)));
		int64 PackedTilePixels = 0;
		for (const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : OutPlaneInfos)
		{
			PackedTilePixels += static_cast<int64>(PlaneInfo.AtlasTileSize.X)
				* PlaneInfo.AtlasTileSize.Y;
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackedTilePixels += static_cast<int64>(PlaneInfo.BackAtlasTileSize.X)
					* PlaneInfo.BackAtlasTileSize.Y;
			}
		}
		const int64 TargetPixelCount = static_cast<int64>(OutputWidth) * OutputHeight;
		OutStats.PackedTileUtilizationPercent = TargetPixelCount > 0
			? 100.0 * static_cast<double>(PackedTilePixels) / static_cast<double>(TargetPixelCount)
			: 0.0;
		return true;
	}
}
