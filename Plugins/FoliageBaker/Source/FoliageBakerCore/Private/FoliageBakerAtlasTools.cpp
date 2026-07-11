#include "FoliageBakerAtlasTools.h"

namespace UE::FoliageBaker::Atlas
{
	namespace
	{
		FColor NormalizeEncodedObjectSpaceNormal(const FColor& Pixel)
		{
			const FVector Normal(
				static_cast<double>(Pixel.R) / 255.0 * 2.0 - 1.0,
				static_cast<double>(Pixel.G) / 255.0 * 2.0 - 1.0,
				static_cast<double>(Pixel.B) / 255.0 * 2.0 - 1.0);
			const FVector SafeNormal = Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			return FColor(
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.X * 0.5 + 0.5) * 255.0), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.Y * 0.5 + 0.5) * 255.0), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.Z * 0.5 + 0.5) * 255.0), 0, 255)),
				Pixel.A);
		}

		bool AccumulateAlphaBoundsForTile(
			const TArray<FColor>& Pixels,
			const int32 Width,
			const int32 Height,
			const FIntPoint& PixelMin,
			const FIntPoint& TileSize,
			const int32 GuardPixels,
			const uint8 AlphaThreshold,
			double& InOutMinUFraction,
			double& InOutMaxUFraction,
			double& InOutMinVFraction,
			double& InOutMaxVFraction)
		{
			if (Width <= 0 || Height <= 0 || TileSize.X <= 0 || TileSize.Y <= 0 || Pixels.Num() < Width * Height)
			{
				return false;
			}

			int32 MinLocalX = TNumericLimits<int32>::Max();
			int32 MaxLocalX = -TNumericLimits<int32>::Max();
			int32 MinLocalY = TNumericLimits<int32>::Max();
			int32 MaxLocalY = -TNumericLimits<int32>::Max();
			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 Y = PixelMin.Y + LocalY;
				if (Y < 0 || Y >= Height)
				{
					continue;
				}
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 X = PixelMin.X + LocalX;
					if (X < 0 || X >= Width || Pixels[Y * Width + X].A < AlphaThreshold)
					{
						continue;
					}
					MinLocalX = FMath::Min(MinLocalX, LocalX);
					MaxLocalX = FMath::Max(MaxLocalX, LocalX);
					MinLocalY = FMath::Min(MinLocalY, LocalY);
					MaxLocalY = FMath::Max(MaxLocalY, LocalY);
				}
			}

			if (MaxLocalX < MinLocalX || MaxLocalY < MinLocalY)
			{
				return false;
			}

			const int32 ExpandedMinX = FMath::Clamp(MinLocalX - GuardPixels, 0, TileSize.X - 1);
			const int32 ExpandedMaxX = FMath::Clamp(MaxLocalX + GuardPixels, 0, TileSize.X - 1);
			const int32 ExpandedMinY = FMath::Clamp(MinLocalY - GuardPixels, 0, TileSize.Y - 1);
			const int32 ExpandedMaxY = FMath::Clamp(MaxLocalY + GuardPixels, 0, TileSize.Y - 1);
			InOutMinUFraction = FMath::Min(InOutMinUFraction, static_cast<double>(ExpandedMinX) / TileSize.X);
			InOutMaxUFraction = FMath::Max(InOutMaxUFraction, static_cast<double>(ExpandedMaxX + 1) / TileSize.X);
			InOutMinVFraction = FMath::Min(InOutMinVFraction, static_cast<double>(ExpandedMinY) / TileSize.Y);
			InOutMaxVFraction = FMath::Max(InOutMaxVFraction, static_cast<double>(ExpandedMaxY + 1) / TileSize.Y);
			return true;
		}
	}

	void NormalizeEncodedObjectSpaceNormals(TArray<FColor>& Pixels)
	{
		for (FColor& Pixel : Pixels)
		{
			Pixel = NormalizeEncodedObjectSpaceNormal(Pixel);
		}
	}

	void FillTransparentRGBInsideTiles(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>* CoverageMask,
		const bool bFillAlpha)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
		{
			return;
		}

		const bool bUseCoverageMask = CoverageMask && CoverageMask->Num() == Pixels.Num();
		auto FillTile = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			const int32 MinX = FMath::Clamp(PixelMin.X, 0, Width - 1);
			const int32 MinY = FMath::Clamp(PixelMin.Y, 0, Height - 1);
			const int32 MaxX = FMath::Clamp(PixelMin.X + TileSize.X - 1, 0, Width - 1);
			const int32 MaxY = FMath::Clamp(PixelMin.Y + TileSize.Y - 1, 0, Height - 1);
			const int32 RegionWidth = MaxX - MinX + 1;
			const int32 RegionHeight = MaxY - MinY + 1;
			if (RegionWidth <= 0 || RegionHeight <= 0)
			{
				return;
			}

			TArray<int32> NearestSource;
			NearestSource.Init(INDEX_NONE, RegionWidth * RegionHeight);
			auto ToLocalIndex = [MinX, MinY, RegionWidth](const int32 X, const int32 Y)
			{
				return (Y - MinY) * RegionWidth + (X - MinX);
			};

			bool bHasAnySource = false;
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 AtlasIndex = Y * Width + X;
					if (bUseCoverageMask ? (*CoverageMask)[AtlasIndex] : Pixels[AtlasIndex].A > 0)
					{
						const int32 LocalIndex = ToLocalIndex(X, Y);
						NearestSource[LocalIndex] = LocalIndex;
						bHasAnySource = true;
					}
				}
			}
			if (!bHasAnySource)
			{
				return;
			}

			auto TryAdoptNearestSource = [&](const int32 TargetX, const int32 TargetY, const int32 CandidateX, const int32 CandidateY)
			{
				if (CandidateX < MinX || CandidateX > MaxX || CandidateY < MinY || CandidateY > MaxY)
				{
					return;
				}
				const int32 TargetIndex = ToLocalIndex(TargetX, TargetY);
				const int32 CandidateSourceIndex = NearestSource[ToLocalIndex(CandidateX, CandidateY)];
				if (CandidateSourceIndex == INDEX_NONE)
				{
					return;
				}
				const int32 TargetLocalX = TargetX - MinX;
				const int32 TargetLocalY = TargetY - MinY;
				const int32 CandidateDeltaX = TargetLocalX - CandidateSourceIndex % RegionWidth;
				const int32 CandidateDeltaY = TargetLocalY - CandidateSourceIndex / RegionWidth;
				const int32 CandidateDistanceSquared = CandidateDeltaX * CandidateDeltaX + CandidateDeltaY * CandidateDeltaY;
				int32 CurrentDistanceSquared = MAX_int32;
				const int32 CurrentSourceIndex = NearestSource[TargetIndex];
				if (CurrentSourceIndex != INDEX_NONE)
				{
					const int32 CurrentDeltaX = TargetLocalX - CurrentSourceIndex % RegionWidth;
					const int32 CurrentDeltaY = TargetLocalY - CurrentSourceIndex / RegionWidth;
					CurrentDistanceSquared = CurrentDeltaX * CurrentDeltaX + CurrentDeltaY * CurrentDeltaY;
				}
				if (CandidateDistanceSquared < CurrentDistanceSquared)
				{
					NearestSource[TargetIndex] = CandidateSourceIndex;
				}
			};

			auto RelaxPass = [&](const bool bTopToBottom, const bool bLeftToRight)
			{
				const int32 YStart = bTopToBottom ? MinY : MaxY;
				const int32 YEnd = bTopToBottom ? MaxY + 1 : MinY - 1;
				const int32 YStep = bTopToBottom ? 1 : -1;
				const int32 XStart = bLeftToRight ? MinX : MaxX;
				const int32 XEnd = bLeftToRight ? MaxX + 1 : MinX - 1;
				const int32 XStep = bLeftToRight ? 1 : -1;
				for (int32 Y = YStart; Y != YEnd; Y += YStep)
				{
					for (int32 X = XStart; X != XEnd; X += XStep)
					{
						TryAdoptNearestSource(X, Y, X - XStep, Y);
						const int32 PreviousY = Y - YStep;
						TryAdoptNearestSource(X, Y, X - 1, PreviousY);
						TryAdoptNearestSource(X, Y, X, PreviousY);
						TryAdoptNearestSource(X, Y, X + 1, PreviousY);
					}
				}
			};
			RelaxPass(true, true);
			RelaxPass(true, false);
			RelaxPass(false, true);
			RelaxPass(false, false);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 AtlasIndex = Y * Width + X;
					if (bUseCoverageMask ? (*CoverageMask)[AtlasIndex] : Pixels[AtlasIndex].A > 0)
					{
						continue;
					}
					const int32 SourceLocalIndex = NearestSource[ToLocalIndex(X, Y)];
					if (SourceLocalIndex == INDEX_NONE)
					{
						continue;
					}
					FColor FilledColor = Pixels[
						(MinY + SourceLocalIndex / RegionWidth) * Width
						+ MinX + SourceLocalIndex % RegionWidth];
					if (!bFillAlpha)
					{
						FilledColor.A = 0;
					}
					Pixels[AtlasIndex] = FilledColor;
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			FillTile(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				FillTile(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize);
			}
		}
	}

	int32 BuildAlphaAwareTileCrops(
		const TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const int32 GuardPixels,
		const uint8 AlphaThreshold,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop>& OutTileCrops)
	{
		OutTileCrops.Reset();
		OutTileCrops.SetNum(PlaneInfos.Num());

		int32 CroppedPlaneCount = 0;
		constexpr double CropEpsilon = 1.0e-5;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			double MinUFraction = 1.0;
			double MaxUFraction = 0.0;
			double MinVFraction = 1.0;
			double MaxVFraction = 0.0;
			bool bHasCoverage = AccumulateAlphaBoundsForTile(
				Pixels, Width, Height, PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize,
				GuardPixels, AlphaThreshold, MinUFraction, MaxUFraction, MinVFraction, MaxVFraction);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				bHasCoverage |= AccumulateAlphaBoundsForTile(
					Pixels, Width, Height, PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize,
					GuardPixels, AlphaThreshold, MinUFraction, MaxUFraction, MinVFraction, MaxVFraction);
			}
			if (!bHasCoverage)
			{
				continue;
			}

			MinUFraction = FMath::Clamp(MinUFraction, 0.0, 1.0);
			MaxUFraction = FMath::Clamp(MaxUFraction, 0.0, 1.0);
			MinVFraction = FMath::Clamp(MinVFraction, 0.0, 1.0);
			MaxVFraction = FMath::Clamp(MaxVFraction, 0.0, 1.0);
			const bool bCropsTile = MinUFraction > CropEpsilon
				|| MaxUFraction < 1.0 - CropEpsilon
				|| MinVFraction > CropEpsilon
				|| MaxVFraction < 1.0 - CropEpsilon;
			if (!bCropsTile || MaxUFraction <= MinUFraction || MaxVFraction <= MinVFraction)
			{
				continue;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop& Crop = OutTileCrops[PlaneIndex];
			Crop.bEnabled = true;
			Crop.MinUFraction = MinUFraction;
			Crop.MaxUFraction = MaxUFraction;
			Crop.MinVFraction = MinVFraction;
			Crop.MaxVFraction = MaxVFraction;
			++CroppedPlaneCount;
		}
		return CroppedPlaneCount;
	}
}
