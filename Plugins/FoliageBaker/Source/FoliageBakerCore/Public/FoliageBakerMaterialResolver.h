#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

namespace UE::FoliageBaker::MaterialResolver
{
	struct FOLIAGEBAKERCORE_API FMaterialOutputSelection
	{
		bool bBaseColorOpacity = true;
		bool bNormalMask = true;
		bool bMix = false;

		bool HasAnyOutput() const;
	};

	struct FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<uint8> MatchingMaterialFlags;

		bool IsMatch(int32 MaterialIndex) const;
	};

	FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult ResolveMaterialKeywordMatches(
		const UStaticMesh& StaticMesh,
		const TArray<FString>& RawKeywords);
}
