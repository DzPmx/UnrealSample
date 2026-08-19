#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

class SWidget;
class UObject;
class UStaticMesh;

DECLARE_DELEGATE_RetVal(bool, FFoliageBakerFeaturePredicateDelegate);
DECLARE_DELEGATE(FFoliageBakerFeatureActionDelegate);

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerFeatureControllerArgs
{
	TStrongObjectPtr<UObject> SettingsObject;
	TFunction<TArray<TObjectPtr<UStaticMesh>>&()> GetSourceStaticMeshes;
	FText BakeButtonText;
	FText BakeButtonTooltip;
	FText RequirementsHint;
	FText AddMeshesTransactionText;
	FText ClearMeshesTransactionText;
	bool bShowDetailsOptions = true;
	bool bShowPropertyMatrixButton = true;
	FFoliageBakerFeaturePredicateDelegate CanBake;
	FFoliageBakerFeatureActionDelegate Bake;
};

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerFeatureController final
{
public:
	static TSharedRef<FFoliageBakerFeatureController> Create(
		const FFoliageBakerFeatureControllerArgs& Args);

	~FFoliageBakerFeatureController();

	TSharedRef<SWidget> GetWidget() const;

private:
	struct FImpl;

	explicit FFoliageBakerFeatureController(TUniquePtr<FImpl>&& InImpl);

	TUniquePtr<FImpl> Impl;
};

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerFeatureBakeItemResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	TArray<TStrongObjectPtr<UObject>> CreatedAssets;
	FString Report;
};

DECLARE_DELEGATE_RetVal_OneParam(
	FFoliageBakerFeatureBakeItemResult,
	FFoliageBakerBakeStaticMeshDelegate,
	UStaticMesh&);

struct FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerFeatureBatchResult
{
	int32 SuccessCount = 0;
	int32 TotalCount = 0;
	TArray<TStrongObjectPtr<UObject>> CreatedAssets;
	FString Report;
};

class FOLIAGEBAKEREDITORCOMMON_API FFoliageBakerFeatureTool final
{
public:
	template <typename StoredSettingsType, typename ConcreteSettingsType = StoredSettingsType>
	static StoredSettingsType& EnsureTransientSettings(
		TStrongObjectPtr<StoredSettingsType>& Settings,
		const FName ObjectName = NAME_None)
	{
		if (!Settings.IsValid())
		{
			Settings.Reset(NewObject<ConcreteSettingsType>(
				GetTransientPackage(),
				ObjectName,
				RF_Transactional));
		}
		return *Settings;
	}

	template <typename BakeResultType>
	static FFoliageBakerFeatureBakeItemResult MakeBakeItemResult(
		const BakeResultType& Result)
	{
		FFoliageBakerFeatureBakeItemResult ItemResult;
		ItemResult.bSucceeded = Result.bSucceeded;
		ItemResult.bCancelled = Result.bCancelled;
		ItemResult.CreatedAssets = Result.CreatedAssets;
		ItemResult.Report = Result.Report;
		return ItemResult;
	}

	static bool HasAnyValidStaticMesh(const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes);
	static bool HasExistingAsset(const FSoftObjectPath& AssetPath);
	static bool CanBakeFeature(
		bool bHasMaterialTemplate,
		bool bHasEnabledOutput,
		const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes);

	static FFoliageBakerFeatureBatchResult RunBakeBatch(
		const TArray<TObjectPtr<UStaticMesh>>& SourceStaticMeshes,
		const FText& SlowTaskText,
		bool bAllowCancel,
		const FString& ReportSeparator,
		const FFoliageBakerBakeStaticMeshDelegate& BakeStaticMesh);

	static void SyncCreatedAssetsToContentBrowser(
		const TArray<TStrongObjectPtr<UObject>>& CreatedAssets);
	static void ShowMessage(const FText& Message);
	static void ShowBatchSummary(
		const FFoliageBakerFeatureBatchResult& BatchResult,
		const FText& SummaryFormat);
};
