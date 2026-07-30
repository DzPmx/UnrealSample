#include "FoliageBakerMaterialResolver.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::FoliageBaker::MaterialResolver
{
	namespace
	{
		TArray<FString> BuildNormalizedKeywords(const TArray<FString>& RawKeywords)
		{
			TArray<FString> Keywords;
			for (FString Keyword : RawKeywords)
			{
				Keyword.TrimStartAndEndInline();
				Keyword.ToLowerInline();
				if (!Keyword.IsEmpty())
				{
					Keywords.AddUnique(Keyword);
				}
			}
			return Keywords;
		}

		bool DoesAnyKeywordMatchName(const TArray<FString>& Keywords, const FString& Name)
		{
			const FString LowerName = Name.ToLower();
			for (const FString& Keyword : Keywords)
			{
				if (LowerName.Contains(Keyword))
				{
					return true;
				}
			}
			return false;
		}

		bool DoesMaterialOrParentNameMatchKeywords(
			const UMaterialInterface& MaterialInterface,
			const TArray<FString>& Keywords)
		{
			if (Keywords.IsEmpty())
			{
				return false;
			}

			if (DoesAnyKeywordMatchName(Keywords, MaterialInterface.GetName()))
			{
				return true;
			}

			TStrongObjectPtr<const UMaterialInterface> Parent;
			const TStrongObjectPtr<const UMaterialInstance> MaterialInstance(
				Cast<UMaterialInstance>(&MaterialInterface));
			if (MaterialInstance)
			{
				Parent.Reset(MaterialInstance->Parent.Get());
			}

			while (Parent)
			{
				if (DoesAnyKeywordMatchName(Keywords, Parent->GetName()))
				{
					return true;
				}

				const TStrongObjectPtr<const UMaterialInstance> ParentInstance(
					Cast<UMaterialInstance>(Parent.Get()));
				Parent.Reset(
					ParentInstance
						? ParentInstance->Parent.Get()
						: nullptr);
			}

			return false;
		}

		float ResolveAverage(const uint64 Sum, const int64 SampleCount)
		{
			check(SampleCount > 0);
			return static_cast<float>(Sum) / (static_cast<float>(SampleCount) * 255.0f);
		}
	}

	bool FMaterialOutputSelection::HasAnyOutput() const
	{
		return bBaseColorOpacity || bNormalMask || bMix;
	}

	void FTrunkLeafMaterialAverages::AddSample(
		const bool bIsTrunk,
		const uint8 Roughness,
		const uint8 Specular)
	{
		if (bIsTrunk)
		{
			++TrunkSampleCount;
			TrunkRoughnessSum += Roughness;
			TrunkSpecularSum += Specular;
			return;
		}

		++LeafSampleCount;
		LeafRoughnessSum += Roughness;
		LeafSpecularSum += Specular;
	}

	bool FTrunkLeafMaterialAverages::HasLeafSamples() const
	{
		return LeafSampleCount > 0;
	}

	bool FTrunkLeafMaterialAverages::HasTrunkSamples() const
	{
		return TrunkSampleCount > 0;
	}

	float FTrunkLeafMaterialAverages::GetLeafRoughness() const
	{
		return ResolveAverage(LeafRoughnessSum, LeafSampleCount);
	}

	float FTrunkLeafMaterialAverages::GetLeafSpecular() const
	{
		return ResolveAverage(LeafSpecularSum, LeafSampleCount);
	}

	float FTrunkLeafMaterialAverages::GetTrunkRoughness() const
	{
		return ResolveAverage(TrunkRoughnessSum, TrunkSampleCount);
	}

	float FTrunkLeafMaterialAverages::GetTrunkSpecular() const
	{
		return ResolveAverage(TrunkSpecularSum, TrunkSampleCount);
	}

	int64 FTrunkLeafMaterialAverages::GetLeafSampleCount() const
	{
		return LeafSampleCount;
	}

	int64 FTrunkLeafMaterialAverages::GetTrunkSampleCount() const
	{
		return TrunkSampleCount;
	}

	bool FMaterialKeywordMatchResult::IsMatch(const int32 MaterialIndex) const
	{
		return bEnabled
			&& MatchingMaterialFlags.IsValidIndex(MaterialIndex)
			&& MatchingMaterialFlags[MaterialIndex] != 0;
	}

	bool ResolveTrunkLeafMaterialScalarParameters(
		const FTrunkLeafMaterialAverages& Averages,
		const FTrunkLeafMaterialParameterNames& ParameterNames,
		TArray<FMaterialScalarParameterValue>& OutParameters,
		FString& OutError)
	{
		OutParameters.Reset();
		OutError.Reset();
		TSet<FName> UsedNames;
		auto AddParameter = [&OutParameters, &OutError, &UsedNames](
			const FName ParameterName,
			const TCHAR* Label,
			const float Value) -> bool
		{
			if (ParameterName.IsNone())
			{
				OutError = FString::Printf(
					TEXT("%s Material scalar parameter name is None."),
					Label);
				return false;
			}
			if (UsedNames.Contains(ParameterName))
			{
				OutError = FString::Printf(
					TEXT("Material scalar parameter '%s' is assigned more than once."),
					*ParameterName.ToString());
				return false;
			}
			UsedNames.Add(ParameterName);
			FMaterialScalarParameterValue& Parameter =
				OutParameters.AddDefaulted_GetRef();
			Parameter.ParameterName = ParameterName;
			Parameter.Value = Value;
			return true;
		};

		if (Averages.HasLeafSamples()
			&& (!AddParameter(
					ParameterNames.LeafRoughness,
					TEXT("Leaf Roughness"),
					Averages.GetLeafRoughness())
				|| !AddParameter(
					ParameterNames.LeafSpecular,
					TEXT("Leaf Specular"),
					Averages.GetLeafSpecular())))
		{
			OutParameters.Reset();
			return false;
		}
		if (Averages.HasTrunkSamples()
			&& (!AddParameter(
					ParameterNames.TrunkRoughness,
					TEXT("Trunk Roughness"),
					Averages.GetTrunkRoughness())
				|| !AddParameter(
					ParameterNames.TrunkSpecular,
					TEXT("Trunk Specular"),
					Averages.GetTrunkSpecular())))
		{
			OutParameters.Reset();
			return false;
		}
		return true;
	}

	FString BuildTrunkLeafMaterialAveragesReport(
		const bool bEnabled,
		const FTrunkLeafMaterialAverages& Averages,
		const FTrunkLeafMaterialParameterNames& ParameterNames)
	{
		if (!bEnabled)
		{
			return TEXT("disabled because Mix output is enabled");
		}

		const FString LeafDetails = Averages.HasLeafSamples()
			? FString::Printf(
				TEXT("LeafRoughness=%s:%.4f, LeafSpecular=%s:%.4f; visible samples=%lld"),
				*ParameterNames.LeafRoughness.ToString(),
				Averages.GetLeafRoughness(),
				*ParameterNames.LeafSpecular.ToString(),
				Averages.GetLeafSpecular(),
				Averages.GetLeafSampleCount())
			: TEXT("leaf: no valid pixels, parameters not set");
		const FString TrunkDetails = Averages.HasTrunkSamples()
			? FString::Printf(
				TEXT("TrunkRoughness=%s:%.4f, TrunkSpecular=%s:%.4f; visible samples=%lld"),
				*ParameterNames.TrunkRoughness.ToString(),
				Averages.GetTrunkRoughness(),
				*ParameterNames.TrunkSpecular.ToString(),
				Averages.GetTrunkSpecular(),
				Averages.GetTrunkSampleCount())
			: TEXT("trunk: no valid pixels, parameters not set");
		return FString::Printf(TEXT("%s; %s"), *LeafDetails, *TrunkDetails);
	}

	FMaterialKeywordMatchResult ResolveMaterialKeywordMatches(
		const UStaticMesh& StaticMesh,
		const TArray<FString>& RawKeywords)
	{
		FMaterialKeywordMatchResult Result;
		const TArray<FString> Keywords = BuildNormalizedKeywords(RawKeywords);
		Result.bEnabled = !Keywords.IsEmpty();

		const TArray<FStaticMaterial>& SourceMaterials = StaticMesh.GetStaticMaterials();
		Result.MatchingMaterialFlags.SetNumZeroed(FMath::Max(1, SourceMaterials.Num()));
		if (!Result.bEnabled)
		{
			return Result;
		}

		for (int32 MaterialIndex = 0; MaterialIndex < Result.MatchingMaterialFlags.Num(); ++MaterialIndex)
		{
			const TStrongObjectPtr<UMaterialInterface> MaterialInterface(
				SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface.Get()
					: nullptr);
			if (MaterialInterface
				&& DoesMaterialOrParentNameMatchKeywords(
					*MaterialInterface,
					Keywords))
			{
				Result.MatchingMaterialFlags[MaterialIndex] = 1;
				++Result.MatchedMaterialCount;
			}
		}

		return Result;
	}
}
