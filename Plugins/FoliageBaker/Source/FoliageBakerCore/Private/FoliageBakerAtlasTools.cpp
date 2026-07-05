#include "FoliageBakerAtlasTools.h"

namespace UE::FoliageBaker::Atlas
{
	uint8 EncodeTrunkLeafMask(const bool bIsTrunk)
	{
		return bIsTrunk ? 128 : 255;
	}

	uint8 EncodeUnitFloat(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	FVector DecodeXYZNormal(const FColor& EncodedNormal)
	{
		return FVector(
			static_cast<double>(EncodedNormal.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(EncodedNormal.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(EncodedNormal.B) / 255.0 * 2.0 - 1.0)
			.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	FColor EncodeOctahedralNormal(
		const FVector& InNormal,
		const uint8 TrunkLeafMask,
		const uint8 Depth)
	{
		const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const double L1Norm = FMath::Abs(Normal.X)
			+ FMath::Abs(Normal.Y)
			+ FMath::Abs(Normal.Z);
		const FVector Projected = Normal / FMath::Max(L1Norm, UE_DOUBLE_SMALL_NUMBER);
		FVector2D Octahedral(Projected.X, Projected.Y);
		if (Projected.Z < 0.0)
		{
			const double OldX = Octahedral.X;
			Octahedral.X = (1.0 - FMath::Abs(Octahedral.Y))
				* (OldX >= 0.0 ? 1.0 : -1.0);
			Octahedral.Y = (1.0 - FMath::Abs(OldX))
				* (Octahedral.Y >= 0.0 ? 1.0 : -1.0);
		}
		return FColor(
			EncodeUnitFloat(static_cast<float>(Octahedral.X * 0.5 + 0.5)),
			EncodeUnitFloat(static_cast<float>(Octahedral.Y * 0.5 + 0.5)),
			TrunkLeafMask,
			Depth);
	}

	FVector DecodeOctahedralNormal(const FColor& Pixel)
	{
		FVector Normal(
			static_cast<double>(Pixel.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Pixel.G) / 255.0 * 2.0 - 1.0,
			0.0);
		Normal.Z = 1.0 - FMath::Abs(Normal.X) - FMath::Abs(Normal.Y);
		if (Normal.Z < 0.0)
		{
			const double OldX = Normal.X;
			Normal.X = (1.0 - FMath::Abs(Normal.Y))
				* (OldX >= 0.0 ? 1.0 : -1.0);
			Normal.Y = (1.0 - FMath::Abs(OldX))
				* (Normal.Y >= 0.0 ? 1.0 : -1.0);
		}
		return Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	namespace
	{
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

		bool BuildNearestSourceMap(
			const TBitArray<>& SourceMask,
			const int32 Width,
			const int32 Height,
			TArray<int32>& OutNearestSource)
		{
			const int32 PixelCount = Width * Height;
			OutNearestSource.Init(INDEX_NONE, PixelCount);
			bool bHasAnySource = false;
			for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				if (SourceMask.IsValidIndex(PixelIndex) && SourceMask[PixelIndex])
				{
					OutNearestSource[PixelIndex] = PixelIndex;
					bHasAnySource = true;
				}
			}
			if (!bHasAnySource)
			{
				return false;
			}

			auto TryAdoptNearestSource = [
				Height,
				&OutNearestSource,
				Width](
				const int32 TargetX,
				const int32 TargetY,
				const int32 CandidateX,
				const int32 CandidateY)
			{
				if (CandidateX < 0 || CandidateX >= Width || CandidateY < 0 || CandidateY >= Height)
				{
					return;
				}
				const int32 TargetIndex = TargetY * Width + TargetX;
				const int32 CandidateSourceIndex = OutNearestSource[CandidateY * Width + CandidateX];
				if (CandidateSourceIndex == INDEX_NONE)
				{
					return;
				}
				const int32 CandidateDeltaX = TargetX - CandidateSourceIndex % Width;
				const int32 CandidateDeltaY = TargetY - CandidateSourceIndex / Width;
				const int32 CandidateDistanceSquared = CandidateDeltaX * CandidateDeltaX + CandidateDeltaY * CandidateDeltaY;
				int32 CurrentDistanceSquared = MAX_int32;
				const int32 CurrentSourceIndex = OutNearestSource[TargetIndex];
				if (CurrentSourceIndex != INDEX_NONE)
				{
					const int32 CurrentDeltaX = TargetX - CurrentSourceIndex % Width;
					const int32 CurrentDeltaY = TargetY - CurrentSourceIndex / Width;
					CurrentDistanceSquared = CurrentDeltaX * CurrentDeltaX + CurrentDeltaY * CurrentDeltaY;
				}
				if (CandidateDistanceSquared < CurrentDistanceSquared)
				{
					OutNearestSource[TargetIndex] = CandidateSourceIndex;
				}
			};

			auto RelaxPass = [
				Height,
				&TryAdoptNearestSource,
				Width](const bool bTopToBottom, const bool bLeftToRight)
			{
				const int32 YStart = bTopToBottom ? 0 : Height - 1;
				const int32 YEnd = bTopToBottom ? Height : -1;
				const int32 YStep = bTopToBottom ? 1 : -1;
				const int32 XStart = bLeftToRight ? 0 : Width - 1;
				const int32 XEnd = bLeftToRight ? Width : -1;
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
			return true;
		}
	}

	void FillTransparentRGBInsideTiles(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>& CoverageMask,
		const bool bFillAlpha)
	{
		if (Width <= 0
			|| Height <= 0
			|| Pixels.Num() != Width * Height
			|| CoverageMask.Num() != Pixels.Num())
		{
			return;
		}

		auto FillTile = [
			&CoverageMask,
			&Pixels,
			Height,
			Width,
			bFillAlpha](const FIntPoint& PixelMin, const FIntPoint& TileSize)
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

			TBitArray<> SourceMask;
			SourceMask.Init(false, RegionWidth * RegionHeight);
			auto ToLocalIndex = [MinX, MinY, RegionWidth](const int32 X, const int32 Y)
			{
				return (Y - MinY) * RegionWidth + (X - MinX);
			};

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 AtlasIndex = Y * Width + X;
					if (CoverageMask[AtlasIndex])
					{
						SourceMask[ToLocalIndex(X, Y)] = true;
					}
				}
			}

			TArray<int32> NearestSource;
			if (!BuildNearestSourceMap(SourceMask, RegionWidth, RegionHeight, NearestSource))
			{
				return;
			}

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 AtlasIndex = Y * Width + X;
					if (CoverageMask[AtlasIndex])
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

	void FillTransparentRGBInsideTiles(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos)
	{
		TBitArray<> CoverageMask;
		CoverageMask.Init(false, Pixels.Num());
		for (int32 PixelIndex = 0; PixelIndex < Pixels.Num(); ++PixelIndex)
		{
			CoverageMask[PixelIndex] = Pixels[PixelIndex].A > 0;
		}
		FillTransparentRGBInsideTiles(
			Pixels,
			Width,
			Height,
			PlaneInfos,
			CoverageMask,
			false);
	}

	bool BuildTileOwnerMap(
		const int32 Width,
		const int32 Height,
		const TArray<FIntRect>& TileRects,
		TArray<uint16>& OutTileOwners)
	{
		OutTileOwners.Reset();
		if (Width <= 0
			|| Height <= 0
			|| TileRects.IsEmpty()
			|| TileRects.Num() > MAX_uint16)
		{
			return false;
		}

		TBitArray<> TileMask;
		TileMask.Init(false, Width * Height);
		OutTileOwners.Init(MAX_uint16, Width * Height);
		int32 CoveredPixelCount = 0;
		for (int32 TileIndex = 0; TileIndex < TileRects.Num(); ++TileIndex)
		{
			const FIntRect& TileRect = TileRects[TileIndex];
			const int32 MinX = FMath::Clamp(TileRect.Min.X, 0, Width);
			const int32 MinY = FMath::Clamp(TileRect.Min.Y, 0, Height);
			const int32 MaxX = FMath::Clamp(TileRect.Max.X, 0, Width);
			const int32 MaxY = FMath::Clamp(TileRect.Max.Y, 0, Height);
			for (int32 Y = MinY; Y < MaxY; ++Y)
			{
				for (int32 X = MinX; X < MaxX; ++X)
				{
					const int32 PixelIndex = Y * Width + X;
					if (TileMask[PixelIndex])
					{
						OutTileOwners.Reset();
						return false;
					}
					TileMask[PixelIndex] = true;
					OutTileOwners[PixelIndex] = static_cast<uint16>(TileIndex);
					++CoveredPixelCount;
				}
			}
		}
		if (CoveredPixelCount == Width * Height)
		{
			return true;
		}

		TArray<int32> NearestSource;
		if (!BuildNearestSourceMap(TileMask, Width, Height, NearestSource))
		{
			OutTileOwners.Reset();
			return false;
		}

		for (int32 PixelIndex = 0; PixelIndex < OutTileOwners.Num(); ++PixelIndex)
		{
			const int32 SourceIndex = NearestSource[PixelIndex];
			if (!OutTileOwners.IsValidIndex(SourceIndex)
				|| OutTileOwners[SourceIndex] == MAX_uint16)
			{
				OutTileOwners.Reset();
				return false;
			}
			OutTileOwners[PixelIndex] = OutTileOwners[SourceIndex];
		}
		return true;
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
