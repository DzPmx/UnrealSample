#include "FoliageBakerMaterialResolver.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"

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
			const UMaterialInterface* MaterialInterface,
			const TArray<FString>& Keywords)
		{
			if (!MaterialInterface || Keywords.IsEmpty())
			{
				return false;
			}

			if (DoesAnyKeywordMatchName(Keywords, MaterialInterface->GetName()))
			{
				return true;
			}

			const UMaterialInterface* Parent = nullptr;
			if (const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(MaterialInterface))
			{
				Parent = MaterialInstance->Parent;
			}

			while (Parent)
			{
				if (DoesAnyKeywordMatchName(Keywords, Parent->GetName()))
				{
					return true;
				}

				const UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(Parent);
				Parent = ParentInstance ? ParentInstance->Parent : nullptr;
			}

			return false;
		}
	}

	bool FMaterialOutputSelection::HasAnyOutput() const
	{
		return bBaseColorOpacity || bNormalMask || bMix;
	}

	bool FMaterialKeywordMatchResult::IsMatch(const int32 MaterialIndex) const
	{
		return bEnabled
			&& MatchingMaterialFlags.IsValidIndex(MaterialIndex)
			&& MatchingMaterialFlags[MaterialIndex] != 0;
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
			const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
				? SourceMaterials[MaterialIndex].MaterialInterface
				: nullptr;
			if (DoesMaterialOrParentNameMatchKeywords(MaterialInterface, Keywords))
			{
				Result.MatchingMaterialFlags[MaterialIndex] = 1;
				++Result.MatchedMaterialCount;
			}
		}

		return Result;
	}
}
