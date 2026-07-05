#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

namespace UE::FoliageBaker::MaterialResolver
{
	struct FOLIAGEBAKERCORE_API FMaterialOutputSelection
	{
		bool bColorAtlas = true;
		bool bNormalAtlas = true;
		bool bMix = false;
		bool bMaterialScalarAverages = false;

		bool HasAnyOutput() const;
	};

	struct FOLIAGEBAKERCORE_API FTrunkLeafMaterialAverages
	{
		void AddSample(bool bIsTrunk, uint8 Roughness, uint8 Specular);

		bool HasLeafSamples() const;
		bool HasTrunkSamples() const;
		float GetLeafRoughness() const;
		float GetLeafSpecular() const;
		float GetTrunkRoughness() const;
		float GetTrunkSpecular() const;
		int64 GetLeafSampleCount() const;
		int64 GetTrunkSampleCount() const;

	private:
		int64 LeafSampleCount = 0;
		int64 TrunkSampleCount = 0;
		uint64 LeafRoughnessSum = 0;
		uint64 LeafSpecularSum = 0;
		uint64 TrunkRoughnessSum = 0;
		uint64 TrunkSpecularSum = 0;
	};

	struct FOLIAGEBAKERCORE_API FTrunkLeafMaterialParameterNames
	{
		FName LeafRoughness = NAME_None;
		FName LeafSpecular = NAME_None;
		FName TrunkRoughness = NAME_None;
		FName TrunkSpecular = NAME_None;
	};

	struct FOLIAGEBAKERCORE_API FMaterialScalarParameterValue
	{
		FName ParameterName = NAME_None;
		float Value = 0.0f;
	};

	struct FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<uint8> MatchingMaterialFlags;

		bool IsMatch(int32 MaterialIndex) const;
	};

	FOLIAGEBAKERCORE_API bool ResolveTrunkLeafMaterialScalarParameters(
		const FTrunkLeafMaterialAverages& Averages,
		const FTrunkLeafMaterialParameterNames& ParameterNames,
		TArray<FMaterialScalarParameterValue>& OutParameters,
		FString& OutError);

	FOLIAGEBAKERCORE_API FString BuildTrunkLeafMaterialAveragesReport(
		bool bEnabled,
		const FTrunkLeafMaterialAverages& Averages,
		const FTrunkLeafMaterialParameterNames& ParameterNames);

	FOLIAGEBAKERCORE_API FMaterialKeywordMatchResult ResolveMaterialKeywordMatches(
		const UStaticMesh& StaticMesh,
		const TArray<FString>& RawKeywords);
}
