#include "FoliageBakerProxyPreflight.h"

#include "FoliageBakerSourceMesh.h"
#include "Engine/StaticMesh.h"

namespace
{
	bool BuildGeneratedAssetPlan(
		const UStaticMesh& SourceStaticMesh,
		const FFoliageBakerProxyPreflightRequest& Request,
		const FFoliageBakerProxyPreflightResult& PreflightResult,
		TArray<FFoliageBakerGeneratedAssetPath>& OutGeneratedAssets,
		FString& OutError)
	{
		OutGeneratedAssets.Reset();
		OutGeneratedAssets.Reserve(
			Request.GeneratedAssets.Num()
			+ (PreflightResult.MeshOutputSelection.OutputMode
				== EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
				? 1
				: 0));

		for (const FFoliageBakerGeneratedAssetSpec& AssetSpec :
			Request.GeneratedAssets)
		{
			const FString& OutputPackagePathOverride =
				AssetSpec.Location
					== EFoliageBakerGeneratedAssetLocation::Texture
				? PreflightResult.OutputFolders.TexturePackagePath
				: PreflightResult.OutputFolders.MaterialPackagePath;
			FFoliageBakerGeneratedAssetPath AssetPath;
			if (!FFoliageBakerAssetBuilder::BuildGeneratedAssetPath(
					SourceStaticMesh,
					AssetSpec.ConfiguredOutputFolder,
					OutputPackagePathOverride,
					AssetSpec.AssetNamePrefix,
					AssetSpec.AssetNameSuffix,
					AssetPath,
					OutError))
			{
				return false;
			}
			AssetPath.DisplayName = AssetSpec.DisplayName;
			OutGeneratedAssets.Add(MoveTemp(AssetPath));
		}

		if (PreflightResult.MeshOutputSelection.OutputMode
			!= EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			return true;
		}
		if (PreflightResult.SeparateMeshAssetSuffix.IsEmpty())
		{
			OutError = TEXT(
				"Separate Mesh output requires a generated mesh asset suffix.");
			return false;
		}

		FFoliageBakerGeneratedAssetPath MeshPath;
		if (!FFoliageBakerAssetBuilder::BuildGeneratedStaticMeshAssetPath(
				SourceStaticMesh,
				PreflightResult.SeparateMeshAssetSuffix,
				MeshPath,
				OutError))
		{
			return false;
		}
		MeshPath.DisplayName = TEXT("Static Mesh");
		OutGeneratedAssets.Add(MoveTemp(MeshPath));
		return true;
	}
}

EFoliageBakerProxyPreflightStatus FFoliageBakerProxyPreflight::Run(
	const UStaticMesh& SourceStaticMesh,
	const FFoliageBakerProxyPreflightRequest& Request,
	const FFoliageBakerMeshOutputSelector& MeshOutputSelector,
	FFoliageBakerProxyPreflightResult& OutResult,
	FString& OutError)
{
	OutResult = FFoliageBakerProxyPreflightResult();
	OutError.Reset();
	const int32 SourceLODIndex = Request.SourceLODAssetParams.SourceLODIndex;
	if (!FFoliageBakerSourceMeshReader::ValidateSourceLOD(
			SourceStaticMesh,
			SourceLODIndex,
			OutError))
	{
		return EFoliageBakerProxyPreflightStatus::Failed;
	}
	if (!MeshOutputSelector.IsBound())
	{
		OutError = TEXT("Mesh output selector is not bound.");
		return EFoliageBakerProxyPreflightStatus::Failed;
	}

	const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
		MeshOutputSelector.Execute(
			SourceStaticMesh,
			SourceLODIndex);
	if (!MeshOutputSelection.IsSet())
	{
		return EFoliageBakerProxyPreflightStatus::Cancelled;
	}

	OutResult.MeshOutputSelection = MeshOutputSelection.GetValue();
	OutResult.SourceLODAssetParams = Request.SourceLODAssetParams;
	OutResult.SourceLODAssetParams.OutputMode =
		OutResult.MeshOutputSelection.OutputMode;
	OutResult.SourceLODAssetParams.RequestedReplaceLODIndex =
		OutResult.MeshOutputSelection.ReplaceLODIndex;
	OutResult.SourceLODAssetParams.RequestedInsertAfterLODIndex =
		OutResult.MeshOutputSelection.InsertAfterLODIndex;
	OutResult.SeparateMeshAssetSuffix = Request.SeparateMeshAssetSuffix;
	if (OutResult.MeshOutputSelection.OutputMode
			!= EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
		&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
			SourceStaticMesh,
			OutResult.SourceLODAssetParams,
			OutError))
	{
		return EFoliageBakerProxyPreflightStatus::Failed;
	}

	if (Request.bPlaceGeneratedAssetsNearReplacedLODAssets
		&& OutResult.MeshOutputSelection.OutputMode
			== EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
	{
		OutResult.OutputFolders =
			FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
				SourceStaticMesh,
				OutResult.MeshOutputSelection.ReplaceLODIndex);
	}

	TArray<FFoliageBakerGeneratedAssetPath> GeneratedAssets;
	if (!BuildGeneratedAssetPlan(
			SourceStaticMesh,
			Request,
			OutResult,
			GeneratedAssets,
			OutError))
	{
		return EFoliageBakerProxyPreflightStatus::Failed;
	}
	const TOptional<FFoliageBakerExistingAssetDecision> ExistingAssetDecision =
		FFoliageBakerExistingAssetDialog::OpenIfNeeded(
			GeneratedAssets,
			OutError);
	if (!ExistingAssetDecision.IsSet())
	{
		return OutError.IsEmpty()
			? EFoliageBakerProxyPreflightStatus::Cancelled
			: EFoliageBakerProxyPreflightStatus::Failed;
	}

	OutResult.ExistingAssetDecision = ExistingAssetDecision.GetValue();
	return EFoliageBakerProxyPreflightStatus::Succeeded;
}
