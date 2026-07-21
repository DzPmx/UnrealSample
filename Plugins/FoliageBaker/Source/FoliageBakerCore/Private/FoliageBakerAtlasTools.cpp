#include "FoliageBakerAtlasTools.h"

#include "ImageCore.h"

namespace UE::FoliageBaker::Atlas
{
	uint8 EncodeTrunkLeafAlpha(const bool bIsTrunk)
	{
		return bIsTrunk ? 128 : 255;
	}

	namespace
	{
		constexpr float DistanceInfinity = 1.0e20f;

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

			auto TryAdoptNearestSource = [&](const int32 TargetX, const int32 TargetY, const int32 CandidateX, const int32 CandidateY)
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

			auto RelaxPass = [&](const bool bTopToBottom, const bool bLeftToRight)
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

		void BuildSquaredDistanceLine(
			const TArray<float>& Source,
			TArray<float>& OutDistances,
			TArray<int32>& ParabolaLocations,
			TArray<float>& Intersections)
		{
			const int32 Count = Source.Num();
			OutDistances.Init(DistanceInfinity, Count);
			ParabolaLocations.SetNumUninitialized(Count);
			Intersections.SetNumUninitialized(Count + 1);

			int32 FirstFiniteIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Source[Index] < DistanceInfinity * 0.5f)
				{
					FirstFiniteIndex = Index;
					break;
				}
			}
			if (FirstFiniteIndex == INDEX_NONE)
			{
				return;
			}

			int32 ParabolaCount = 0;
			ParabolaLocations[0] = FirstFiniteIndex;
			Intersections[0] = -DistanceInfinity;
			Intersections[1] = DistanceInfinity;
			for (int32 Location = FirstFiniteIndex + 1; Location < Count; ++Location)
			{
				if (Source[Location] >= DistanceInfinity * 0.5f)
				{
					continue;
				}

				float Intersection = 0.0f;
				while (true)
				{
					const int32 PreviousLocation = ParabolaLocations[ParabolaCount];
					Intersection = static_cast<float>(
						((static_cast<double>(Source[Location]) + static_cast<double>(Location) * Location)
							- (static_cast<double>(Source[PreviousLocation]) + static_cast<double>(PreviousLocation) * PreviousLocation))
						/ (2.0 * (Location - PreviousLocation)));
					if (Intersection > Intersections[ParabolaCount] || ParabolaCount == 0)
					{
						break;
					}
					--ParabolaCount;
				}
				++ParabolaCount;
				ParabolaLocations[ParabolaCount] = Location;
				Intersections[ParabolaCount] = Intersection;
				Intersections[ParabolaCount + 1] = DistanceInfinity;
			}

			int32 ActiveParabola = 0;
			for (int32 Location = 0; Location < Count; ++Location)
			{
				while (Intersections[ActiveParabola + 1] < Location)
				{
					++ActiveParabola;
				}
				const int32 SourceLocation = ParabolaLocations[ActiveParabola];
				const float Delta = static_cast<float>(Location - SourceLocation);
				OutDistances[Location] = Delta * Delta + Source[SourceLocation];
			}
		}

		void BuildSquaredDistanceField(
			const TBitArray<>& Mask,
			const int32 Width,
			const int32 Height,
			const bool bFeatureValue,
			TArray<float>& OutDistances)
		{
			const int32 PixelCount = Width * Height;
			TArray<float> Intermediate;
			Intermediate.SetNumUninitialized(PixelCount);
			TArray<float> LineSource;
			TArray<float> LineDistances;
			TArray<int32> ParabolaLocations;
			TArray<float> Intersections;

			LineSource.SetNumUninitialized(Width);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const int32 PixelIndex = Y * Width + X;
					LineSource[X] = Mask.IsValidIndex(PixelIndex) && Mask[PixelIndex] == bFeatureValue
						? 0.0f
						: DistanceInfinity;
				}
				BuildSquaredDistanceLine(LineSource, LineDistances, ParabolaLocations, Intersections);
				for (int32 X = 0; X < Width; ++X)
				{
					Intermediate[Y * Width + X] = LineDistances[X];
				}
			}

			OutDistances.SetNumUninitialized(PixelCount);
			LineSource.SetNumUninitialized(Height);
			for (int32 X = 0; X < Width; ++X)
			{
				for (int32 Y = 0; Y < Height; ++Y)
				{
					LineSource[Y] = Intermediate[Y * Width + X];
				}
				BuildSquaredDistanceLine(LineSource, LineDistances, ParabolaLocations, Intersections);
				for (int32 Y = 0; Y < Height; ++Y)
				{
					OutDistances[Y * Width + X] = LineDistances[Y];
				}
			}
		}
	}

	void NormalizeEncodedObjectSpaceNormals(TArray<FColor>& Pixels)
	{
		for (FColor& Pixel : Pixels)
		{
			Pixel = NormalizeEncodedObjectSpaceNormal(Pixel);
		}
	}

	bool ResizeTileIsolated(
		const TArray<FColor>& SourcePixels,
		const int32 SourceWidth,
		const int32 SourceHeight,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& SourcePlaneInfos,
		const int32 RequestedMaximumDimension,
		const FColor BackgroundColor,
		TArray<FColor>& OutPixels,
		int32& OutWidth,
		int32& OutHeight,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& OutPlaneInfos,
		FString& OutError)
	{
		if (SourceWidth <= 0
			|| SourceHeight <= 0
			|| SourcePixels.Num() != SourceWidth * SourceHeight
			|| SourcePlaneInfos.IsEmpty())
		{
			OutError = TEXT("Tile-isolated atlas resize received invalid source data.");
			return false;
		}

		const int32 MaximumDimension = FMath::Max(4, RequestedMaximumDimension);
		const double Scale =
			static_cast<double>(MaximumDimension)
			/ static_cast<double>(FMath::Max(SourceWidth, SourceHeight));
		OutWidth = MaximumDimension;
		OutHeight = MaximumDimension;
		if (SourceWidth >= SourceHeight)
		{
			OutHeight = FMath::Clamp(
				Align(FMath::Max(1, FMath::RoundToInt(SourceHeight * Scale)), 4),
				4,
				MaximumDimension);
		}
		else
		{
			OutWidth = FMath::Clamp(
				Align(FMath::Max(1, FMath::RoundToInt(SourceWidth * Scale)), 4),
				4,
				MaximumDimension);
		}

		OutPixels.Init(BackgroundColor, OutWidth * OutHeight);
		OutPlaneInfos = SourcePlaneInfos;

		auto ScaleTileRect = [SourceWidth,
							  SourceHeight,
							  TargetWidth = OutWidth,
							  TargetHeight = OutHeight](
			const FIntPoint& SourcePixelMin,
			const FIntPoint& SourceTileSize)
		{
			const int32 TargetMinX = FMath::Clamp(
				FMath::RoundToInt(
					static_cast<double>(SourcePixelMin.X) * TargetWidth / SourceWidth),
				0,
				TargetWidth - 1);
			const int32 TargetMinY = FMath::Clamp(
				FMath::RoundToInt(
					static_cast<double>(SourcePixelMin.Y) * TargetHeight / SourceHeight),
				0,
				TargetHeight - 1);
			const int32 TargetMaxX = FMath::Clamp(
				FMath::RoundToInt(
					static_cast<double>(SourcePixelMin.X + SourceTileSize.X)
						* TargetWidth / SourceWidth),
				TargetMinX + 1,
				TargetWidth);
			const int32 TargetMaxY = FMath::Clamp(
				FMath::RoundToInt(
					static_cast<double>(SourcePixelMin.Y + SourceTileSize.Y)
						* TargetHeight / SourceHeight),
				TargetMinY + 1,
				TargetHeight);
			return FIntRect(
				FIntPoint(TargetMinX, TargetMinY),
				FIntPoint(TargetMaxX, TargetMaxY));
		};

		auto ResizeTile = [&](const FIntPoint& SourcePixelMin,
							  const FIntPoint& SourceTileSize,
							  FIntPoint& OutPixelMin,
							  FIntPoint& OutTileSize)
		{
			const FIntRect SourceRect(
				SourcePixelMin,
				SourcePixelMin + SourceTileSize);
			if (SourceRect.Min.X < 0
				|| SourceRect.Min.Y < 0
				|| SourceRect.Max.X > SourceWidth
				|| SourceRect.Max.Y > SourceHeight
				|| SourceRect.Width() <= 0
				|| SourceRect.Height() <= 0)
			{
				OutError = TEXT("Tile-isolated atlas resize contains an invalid source tile.");
				return false;
			}

			const FIntRect TargetRect = ScaleTileRect(SourcePixelMin, SourceTileSize);
			FImage SourceImage(
				SourceRect.Width(),
				SourceRect.Height(),
				1,
				ERawImageFormat::BGRA8,
				EGammaSpace::Linear);
			FColor* SourceImagePixels =
				reinterpret_cast<FColor*>(SourceImage.RawData.GetData());
			for (int32 LocalY = 0; LocalY < SourceRect.Height(); ++LocalY)
			{
				FMemory::Memcpy(
					SourceImagePixels + LocalY * SourceRect.Width(),
					SourcePixels.GetData()
						+ (SourceRect.Min.Y + LocalY) * SourceWidth
						+ SourceRect.Min.X,
					static_cast<SIZE_T>(SourceRect.Width()) * sizeof(FColor));
			}

			FImage TargetImage(
				TargetRect.Width(),
				TargetRect.Height(),
				1,
				ERawImageFormat::BGRA8,
				EGammaSpace::Linear);
			if (SourceRect.Size() == TargetRect.Size())
			{
				FMemory::Memcpy(
					TargetImage.RawData.GetData(),
					SourceImage.RawData.GetData(),
					SourceImage.RawData.Num());
			}
			else
			{
				const FImageCore::EResizeImageFilter Filter =
					TargetRect.Width() < SourceRect.Width()
						|| TargetRect.Height() < SourceRect.Height()
					? FImageCore::EResizeImageFilter::Box
					: FImageCore::EResizeImageFilter::Bilinear;
				FImageCore::ResizeImage(SourceImage, TargetImage, Filter);
			}

			const FColor* TargetImagePixels =
				reinterpret_cast<const FColor*>(TargetImage.RawData.GetData());
			for (int32 LocalY = 0; LocalY < TargetRect.Height(); ++LocalY)
			{
				FMemory::Memcpy(
					OutPixels.GetData()
						+ (TargetRect.Min.Y + LocalY) * OutWidth
						+ TargetRect.Min.X,
					TargetImagePixels + LocalY * TargetRect.Width(),
					static_cast<SIZE_T>(TargetRect.Width()) * sizeof(FColor));
			}

			OutPixelMin = TargetRect.Min;
			OutTileSize = TargetRect.Size();
			return true;
		};

		const double PixelScale = FMath::Min(
			static_cast<double>(OutWidth) / SourceWidth,
			static_cast<double>(OutHeight) / SourceHeight);
		for (int32 PlaneIndex = 0; PlaneIndex < SourcePlaneInfos.Num(); ++PlaneIndex)
		{
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& SourcePlaneInfo =
				SourcePlaneInfos[PlaneIndex];
			UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& TargetPlaneInfo =
				OutPlaneInfos[PlaneIndex];
			if (!ResizeTile(
					SourcePlaneInfo.AtlasPixelMin,
					SourcePlaneInfo.AtlasTileSize,
					TargetPlaneInfo.AtlasPixelMin,
					TargetPlaneInfo.AtlasTileSize))
			{
				return false;
			}
			if (SourcePlaneInfo.bHasBackFaceAtlas
				&& !ResizeTile(
					SourcePlaneInfo.BackAtlasPixelMin,
					SourcePlaneInfo.BackAtlasTileSize,
					TargetPlaneInfo.BackAtlasPixelMin,
					TargetPlaneInfo.BackAtlasTileSize))
			{
				return false;
			}

			TargetPlaneInfo.AtlasTileResolution = FMath::Max(
				1,
				FMath::RoundToInt(SourcePlaneInfo.AtlasTileResolution * PixelScale));
			TargetPlaneInfo.AtlasTilePaddingPixels = FMath::Max(
				0,
				FMath::RoundToInt(SourcePlaneInfo.AtlasTilePaddingPixels * PixelScale));
		}
		return true;
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
					if (bUseCoverageMask ? (*CoverageMask)[AtlasIndex] : Pixels[AtlasIndex].A > 0)
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

	void WriteUnionSdfToAlpha(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>& CoverageMask,
		const int32 SdfRangePixels)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height || CoverageMask.Num() != Pixels.Num())
		{
			return;
		}

		const float SafeRange = static_cast<float>(FMath::Max(1, SdfRangePixels));
		auto PackTile = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize)
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
			const int32 RegionPixelCount = RegionWidth * RegionHeight;
			if (RegionPixelCount <= 0)
			{
				return;
			}

			TBitArray<> LocalCoverage;
			LocalCoverage.Init(false, RegionPixelCount);
			for (int32 LocalY = 0; LocalY < RegionHeight; ++LocalY)
			{
				for (int32 LocalX = 0; LocalX < RegionWidth; ++LocalX)
				{
					const int32 AtlasIndex = (MinY + LocalY) * Width + MinX + LocalX;
					LocalCoverage[LocalY * RegionWidth + LocalX] = CoverageMask[AtlasIndex];
				}
			}

			TArray<float> DistanceToCoverage;
			TArray<float> DistanceToBackground;
			BuildSquaredDistanceField(LocalCoverage, RegionWidth, RegionHeight, true, DistanceToCoverage);
			BuildSquaredDistanceField(LocalCoverage, RegionWidth, RegionHeight, false, DistanceToBackground);

			for (int32 LocalY = 0; LocalY < RegionHeight; ++LocalY)
			{
				for (int32 LocalX = 0; LocalX < RegionWidth; ++LocalX)
				{
					const int32 LocalIndex = LocalY * RegionWidth + LocalX;
					const int32 AtlasIndex = (MinY + LocalY) * Width + MinX + LocalX;
					const bool bCovered = LocalCoverage[LocalIndex];
					const float SquaredDistance = bCovered
						? DistanceToBackground[LocalIndex]
						: DistanceToCoverage[LocalIndex];
					const float Distance = SquaredDistance < DistanceInfinity * 0.5f
						? FMath::Max(0.0f, FMath::Sqrt(SquaredDistance) - 0.5f)
						: SafeRange;
					const float SignedDistance = bCovered ? Distance : -Distance;
					const float UnionSdf = FMath::Clamp(0.5f + SignedDistance / (2.0f * SafeRange), 0.0f, 1.0f);
					Pixels[AtlasIndex].A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(UnionSdf * 255.0f), 0, 255));
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			PackTile(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackTile(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize);
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
