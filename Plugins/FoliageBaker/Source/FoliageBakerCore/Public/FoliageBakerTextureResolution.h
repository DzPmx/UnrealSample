#pragma once

#include "CoreMinimal.h"

#include "FoliageBakerTextureResolution.generated.h"

UENUM()
enum class EFoliageBakerTextureResolutionMode : uint8
{
	AutoWorldTexelSize UMETA(DisplayName = "Auto - World Texel Size"),
	ManualAtlasResolution UMETA(DisplayName = "Manual Atlas Resolution")
};

namespace UE::FoliageBaker::TextureResolution
{
	constexpr int32 MinimumSupportedAtlasResolution = 64;
	constexpr int32 MaximumSupportedAtlasResolution = 4096;

	inline int32 FloorToSupportedPowerOfTwo(const int32 Resolution)
	{
		const int32 ClampedResolution = FMath::Clamp(
			Resolution,
			MinimumSupportedAtlasResolution,
			MaximumSupportedAtlasResolution);
		return 1 << FMath::FloorLog2(static_cast<uint32>(ClampedResolution));
	}

	inline int32 CeilToSupportedPowerOfTwo(const int32 Resolution)
	{
		const int32 ClampedResolution = FMath::Clamp(
			Resolution,
			MinimumSupportedAtlasResolution,
			MaximumSupportedAtlasResolution);
		return static_cast<int32>(
			FMath::RoundUpToPowerOfTwo(static_cast<uint32>(ClampedResolution)));
	}

	inline int32 ResolveMinimumAtlasResolution(
		const int32 MinimumResolution,
		const int32 MaximumResolution)
	{
		return FMath::Min(
			CeilToSupportedPowerOfTwo(MinimumResolution),
			FloorToSupportedPowerOfTwo(MaximumResolution));
	}
}
