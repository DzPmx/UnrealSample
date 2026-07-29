#include "FoliageBakerAssetBuilder.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "FoliageBakerAtlasTools.h"
#include "ImageCore.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/MetaData.h"
#include "UObject/ObjectEditorOptionalSupport.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerAssetBuilder, Log, All);

namespace
{
	constexpr EObjectFlags PersistentAssetFlags =
		RF_Public | RF_Standalone | RF_Transactional;
	constexpr EObjectFlags ManagedAssetFlags =
		PersistentAssetFlags | RF_Transient;
	const FName OwnedTextureParametersMetadataKey(TEXT("FoliageBaker.OwnedTextureParameters"));
	const FName OwnedScalarParametersMetadataKey(TEXT("FoliageBaker.OwnedScalarParameters"));
	const FName OwnedVectorParametersMetadataKey(TEXT("FoliageBaker.OwnedVectorParameters"));
	const FName OwnedStaticSwitchParametersMetadataKey(TEXT("FoliageBaker.OwnedStaticSwitchParameters"));

	struct FOwnedMaterialParameterNames
	{
		TSet<FName> Texture;
		TSet<FName> Scalar;
		TSet<FName> Vector;
		TSet<FName> StaticSwitch;

		void Append(const FOwnedMaterialParameterNames& Other)
		{
			Texture.Append(Other.Texture);
			Scalar.Append(Other.Scalar);
			Vector.Append(Other.Vector);
			StaticSwitch.Append(Other.StaticSwitch);
		}
	};

	void AddOwnedParameterName(TSet<FName>& Names, const FName Name)
	{
		if (!Name.IsNone())
		{
			Names.Add(Name);
		}
	}

	void AddOwnedParameterNames(TSet<FName>& Names, const TArray<FName>& AdditionalNames)
	{
		for (const FName Name : AdditionalNames)
		{
			AddOwnedParameterName(Names, Name);
		}
	}

	void ParseOwnedParameterNames(const FString& SerializedNames, TSet<FName>& OutNames)
	{
		TArray<FString> Names;
		SerializedNames.ParseIntoArrayLines(Names, true);
		for (const FString& Name : Names)
		{
			AddOwnedParameterName(OutNames, FName(*Name));
		}
	}

	FString SerializeOwnedParameterNames(const TSet<FName>& Names)
	{
		TArray<FString> SortedNames;
		SortedNames.Reserve(Names.Num());
		for (const FName Name : Names)
		{
			SortedNames.Add(Name.ToString());
		}
		SortedNames.Sort();
		return FString::Join(SortedNames, TEXT("\n"));
	}

	FOwnedMaterialParameterNames ReadOwnedMaterialParameterNames(
		const UMaterialInstanceConstant& MaterialInstance)
	{
		FOwnedMaterialParameterNames Result;
		FMetaData& MetaData = MaterialInstance.GetPackage()->GetMetaData();
		auto ReadNames = [&MaterialInstance, &MetaData](
			const FName Key,
			TSet<FName>& OutNames)
		{
			if (MetaData.HasValue(&MaterialInstance, Key))
			{
				ParseOwnedParameterNames(
					MetaData.GetValue(&MaterialInstance, Key),
					OutNames);
			}
		};
		ReadNames(OwnedTextureParametersMetadataKey, Result.Texture);
		ReadNames(OwnedScalarParametersMetadataKey, Result.Scalar);
		ReadNames(OwnedVectorParametersMetadataKey, Result.Vector);
		ReadNames(OwnedStaticSwitchParametersMetadataKey, Result.StaticSwitch);
		return Result;
	}

	FOwnedMaterialParameterNames BuildOwnedMaterialParameterNames(
		const FFoliageBakerMaterialInstanceAssetParams& Params)
	{
		FOwnedMaterialParameterNames Result;
		AddOwnedParameterName(Result.Texture, Params.BaseColorOpacityTextureParameterName);
		AddOwnedParameterName(Result.Texture, Params.NormalDepthTextureParameterName);
		AddOwnedParameterName(Result.Texture, Params.MixTextureParameterName);
		for (const FFoliageBakerMaterialInstanceAssetParams::FTextureParameterValue& Parameter :
			Params.AdditionalTextureParameterValues)
		{
			AddOwnedParameterName(Result.Texture, Parameter.ParameterName);
		}
		for (const UE::FoliageBaker::MaterialResolver::FMaterialScalarParameterValue& Parameter :
			Params.ScalarParameterValues)
		{
			AddOwnedParameterName(Result.Scalar, Parameter.ParameterName);
		}
		for (const FFoliageBakerMaterialInstanceAssetParams::FVectorParameterValue& Parameter :
			Params.VectorParameterValues)
		{
			AddOwnedParameterName(Result.Vector, Parameter.ParameterName);
		}
		for (const FFoliageBakerMaterialInstanceAssetParams::FStaticSwitchParameterValue& Parameter :
			Params.StaticSwitchParameterValues)
		{
			AddOwnedParameterName(Result.StaticSwitch, Parameter.ParameterName);
		}
		AddOwnedParameterNames(Result.Texture, Params.OwnedTextureParameterNames);
		AddOwnedParameterNames(Result.Scalar, Params.OwnedScalarParameterNames);
		return Result;
	}

	bool IsOwnedGlobalParameter(
		const FMaterialParameterInfo& ParameterInfo,
		const TSet<FName>& OwnedNames)
	{
		return ParameterInfo.Association
				== EMaterialParameterAssociation::GlobalParameter
			&& ParameterInfo.Index == INDEX_NONE
			&& OwnedNames.Contains(ParameterInfo.Name);
	}

	void RemoveOwnedMaterialParameterOverrides(
		UMaterialInstanceConstant& MaterialInstance,
		const FOwnedMaterialParameterNames& OwnedNames)
	{
		MaterialInstance.TextureParameterValues.RemoveAll(
			[&OwnedNames](const FTextureParameterValue& Parameter)
			{
				return IsOwnedGlobalParameter(
					Parameter.ParameterInfo,
					OwnedNames.Texture);
			});
		MaterialInstance.ScalarParameterValues.RemoveAll(
			[&OwnedNames](const FScalarParameterValue& Parameter)
			{
				return IsOwnedGlobalParameter(
					Parameter.ParameterInfo,
					OwnedNames.Scalar);
			});
		MaterialInstance.VectorParameterValues.RemoveAll(
			[&OwnedNames](const FVectorParameterValue& Parameter)
			{
				return IsOwnedGlobalParameter(
					Parameter.ParameterInfo,
					OwnedNames.Vector);
			});
	}

	void ReplaceOwnedStaticSwitchParameterOverrides(
		UMaterialInstanceConstant& MaterialInstance,
		const TSet<FName>& OwnedNames,
		const TArray<FFoliageBakerMaterialInstanceAssetParams::FStaticSwitchParameterValue>&
			CurrentValues)
	{
		if (OwnedNames.IsEmpty() && CurrentValues.IsEmpty())
		{
			return;
		}

		FMaterialInstanceParameterUpdateContext UpdateContext(&MaterialInstance);
		TArray<FStaticSwitchParameter>& StaticSwitchParameters =
			UpdateContext.GetStaticParameters().StaticSwitchParameters;
		StaticSwitchParameters.RemoveAll(
			[&OwnedNames](const FStaticSwitchParameter& Parameter)
			{
				return IsOwnedGlobalParameter(
					Parameter.ParameterInfo,
					OwnedNames);
			});
		for (const FFoliageBakerMaterialInstanceAssetParams::FStaticSwitchParameterValue&
			CurrentValue : CurrentValues)
		{
			if (!CurrentValue.ParameterName.IsNone())
			{
				StaticSwitchParameters.Emplace(
					FMaterialParameterInfo(CurrentValue.ParameterName),
					CurrentValue.bValue,
					true,
					FGuid());
			}
		}
	}

	void WriteOwnedMaterialParameterNames(
		UMaterialInstanceConstant& MaterialInstance,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FOwnedMaterialParameterNames& OwnedNames)
	{
		FMetaData& MetaData = MaterialInstance.GetPackage()->GetMetaData();
		auto WriteNames = [
			&MaterialInstance,
			&AssetTransaction,
			&MetaData](
			const FName Key,
			const TSet<FName>& Names)
		{
			AssetTransaction.SnapshotMetadata(&MaterialInstance, Key);
			const FString SerializedNames = SerializeOwnedParameterNames(Names);
			if (SerializedNames.IsEmpty())
			{
				MetaData.RemoveValue(&MaterialInstance, Key);
			}
			else
			{
				MetaData.SetValue(&MaterialInstance, Key, *SerializedNames);
			}
		};
		WriteNames(OwnedTextureParametersMetadataKey, OwnedNames.Texture);
		WriteNames(OwnedScalarParametersMetadataKey, OwnedNames.Scalar);
		WriteNames(OwnedVectorParametersMetadataKey, OwnedNames.Vector);
		WriteNames(OwnedStaticSwitchParametersMetadataKey, OwnedNames.StaticSwitch);
	}

	FString NormalizeRelativeOutputFolder(FString OutputFolder)
	{
		OutputFolder.TrimStartAndEndInline();
		OutputFolder.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (OutputFolder.RemoveFromStart(TEXT("/")))
		{
		}
		while (OutputFolder.RemoveFromEnd(TEXT("/")))
		{
		}
		return OutputFolder;
	}

	FString GetGeneratedAssetSourceName(const UStaticMesh& SourceStaticMesh)
	{
		const FString SourceName = SourceStaticMesh.GetName();
		return SourceName.StartsWith(TEXT("SM_"), ESearchCase::CaseSensitive)
			&& SourceName.Len() > 3
			? SourceName.RightChop(3)
			: SourceName;
	}

	FString NormalizeLongPackageFolder(FString PackageFolder)
	{
		PackageFolder.TrimStartAndEndInline();
		PackageFolder.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (PackageFolder.RemoveFromEnd(TEXT("/")))
		{
		}
		return PackageFolder;
	}

	FString GetPackageRoot(const FString& PackagePath)
	{
		const int32 SecondSlashIndex = PackagePath.Find(
			TEXT("/"),
			ESearchCase::CaseSensitive,
			ESearchDir::FromStart,
			1);
		return SecondSlashIndex == INDEX_NONE
			? PackagePath
			: PackagePath.Left(SecondSlashIndex);
	}

	bool TryGetAssetPackageFolder(const UObject* Asset, FString& OutPackageFolder)
	{
		OutPackageFolder.Reset();
		if (!IsValid(Asset))
		{
			return false;
		}
		const FString PackageName = Asset->GetOutermost()->GetName();
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return false;
		}
		OutPackageFolder = FPackageName::GetLongPackagePath(PackageName);
		return !OutPackageFolder.IsEmpty();
	}

	int32 CountCommonPackagePathComponents(const FString& A, const FString& B)
	{
		TArray<FString> AComponents;
		TArray<FString> BComponents;
		A.ParseIntoArray(AComponents, TEXT("/"), true);
		B.ParseIntoArray(BComponents, TEXT("/"), true);
		const int32 ComponentCount = FMath::Min(AComponents.Num(), BComponents.Num());
		int32 CommonComponentCount = 0;
		while (CommonComponentCount < ComponentCount
			&& AComponents[CommonComponentCount] == BComponents[CommonComponentCount])
		{
			++CommonComponentCount;
		}
		return CommonComponentCount;
	}

	FString FindCommonPackageFolder(const TArray<FString>& PackageFolders)
	{
		if (PackageFolders.IsEmpty())
		{
			return FString();
		}
		TArray<FString> CommonComponents;
		PackageFolders[0].ParseIntoArray(CommonComponents, TEXT("/"), true);
		for (int32 FolderIndex = 1; FolderIndex < PackageFolders.Num() && !CommonComponents.IsEmpty(); ++FolderIndex)
		{
			TArray<FString> FolderComponents;
			PackageFolders[FolderIndex].ParseIntoArray(FolderComponents, TEXT("/"), true);
			const int32 SharedCount = FMath::Min(CommonComponents.Num(), FolderComponents.Num());
			int32 ComponentIndex = 0;
			while (ComponentIndex < SharedCount
				&& CommonComponents[ComponentIndex] == FolderComponents[ComponentIndex])
			{
				++ComponentIndex;
			}
			CommonComponents.SetNum(ComponentIndex);
		}
		return CommonComponents.IsEmpty()
			? FString()
			: TEXT("/") + FString::Join(CommonComponents, TEXT("/"));
	}

	FString SelectMaterialOutputFolder(
		const TArray<UMaterialInterface*>& Materials,
		const FString& SourcePackageRoot)
	{
		TArray<FString> MaterialFolders;
		for (const UMaterialInterface* Material : Materials)
		{
			FString MaterialFolder;
			if (TryGetAssetPackageFolder(Material, MaterialFolder)
				&& GetPackageRoot(MaterialFolder) == SourcePackageRoot)
			{
				MaterialFolders.AddUnique(MaterialFolder);
			}
		}
		const FString CommonFolder = FindCommonPackageFolder(MaterialFolders);
		if (!CommonFolder.IsEmpty() && CommonFolder != SourcePackageRoot)
		{
			return CommonFolder;
		}
		return MaterialFolders.IsEmpty() ? FString() : MaterialFolders[0];
	}

	FString SelectTextureOutputFolder(
		const TArray<UMaterialInterface*>& Materials,
		const FString& MaterialFolder,
		const FString& SourcePackageRoot)
	{
		TMap<FString, int32> FolderUseCounts;
		for (UMaterialInterface* Material : Materials)
		{
			TArray<UTexture*> UsedTextures;
			Material->GetUsedTextures(UsedTextures);
			TSet<FString> MaterialTextureFolders;
			for (const UTexture* Texture : UsedTextures)
			{
				FString TextureFolder;
				if (TryGetAssetPackageFolder(Texture, TextureFolder)
					&& GetPackageRoot(TextureFolder) == SourcePackageRoot)
				{
					MaterialTextureFolders.Add(TextureFolder);
				}
			}
			for (const FString& TextureFolder : MaterialTextureFolders)
			{
				++FolderUseCounts.FindOrAdd(TextureFolder);
			}
		}

		FString BestFolder;
		int32 BestCommonComponentCount = INDEX_NONE;
		int32 BestUseCount = INDEX_NONE;
		for (const TPair<FString, int32>& Entry : FolderUseCounts)
		{
			const int32 CommonComponentCount = MaterialFolder.IsEmpty()
				? 0
				: CountCommonPackagePathComponents(MaterialFolder, Entry.Key);
			if (CommonComponentCount > BestCommonComponentCount
				|| (CommonComponentCount == BestCommonComponentCount && Entry.Value > BestUseCount)
				|| (CommonComponentCount == BestCommonComponentCount
					&& Entry.Value == BestUseCount
					&& (BestFolder.IsEmpty() || Entry.Key < BestFolder)))
			{
				BestFolder = Entry.Key;
				BestCommonComponentCount = CommonComponentCount;
				BestUseCount = Entry.Value;
			}
		}
		return BestFolder;
	}

	bool ResolvePlannedAssetPath(
		const FFoliageBakerGeneratedAssetPath& AssetPath,
		const EFoliageBakerExistingAssetPolicy ExistingAssetPolicy,
		const int32 AssetNameVersion,
		FString& OutPackageName,
		FString& OutAssetName,
		FString& OutError)
	{
		const int32 EffectiveVersion =
			ExistingAssetPolicy == EFoliageBakerExistingAssetPolicy::CreateUnique
				? AssetNameVersion
				: 0;
		if (ExistingAssetPolicy == EFoliageBakerExistingAssetPolicy::CreateUnique
			&& EffectiveVersion <= 0)
		{
			OutError = TEXT("Create New requires a positive shared asset version.");
			return false;
		}

		OutAssetName = AssetPath.BuildAssetName(EffectiveVersion);
		OutPackageName = AssetPath.BuildPackageName(EffectiveVersion);
		return true;
	}

	void FinishCompilationForAssets(
		const TArray<TStrongObjectPtr<UObject>>& Assets)
	{
		TArray<UObject*> ValidAssets;
		ValidAssets.Reserve(Assets.Num());
		for (const TStrongObjectPtr<UObject>& Asset : Assets)
		{
			if (IsValid(Asset.Get()))
			{
				ValidAssets.AddUnique(Asset.Get());
			}
		}
		if (!ValidAssets.IsEmpty())
		{
			FAssetCompilingManager::Get().FinishCompilationForObjects(ValidAssets);
		}
	}

	UMaterialInterface* ResolveProxyMaterial(UMaterialInterface* ProxyMaterial)
	{
		return ProxyMaterial ? ProxyMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
	}

	bool EnsureMaterialInstanceEditorOnlyData(
		UMaterialInstanceConstant& MaterialInstance,
		FString& OutError)
	{
#if WITH_EDITORONLY_DATA
		FObjectProperty* EditorOnlyDataProperty = FindFProperty<FObjectProperty>(
			UMaterialInterface::StaticClass(),
			TEXT("EditorOnlyData"));
		if (!EditorOnlyDataProperty
			|| !UMaterialInstanceEditorOnlyData::StaticClass()->IsChildOf(
				EditorOnlyDataProperty->PropertyClass))
		{
			OutError = FString::Printf(
				TEXT("Could not repair %s because the MaterialInterface EditorOnlyData property is unavailable or incompatible."),
				*MaterialInstance.GetPathName());
			return false;
		}

		const FString EditorOnlyDataName =
			MaterialInstance.GetName() + TEXT("EditorOnlyData");
		UMaterialInstanceEditorOnlyData* AttachedEditorOnlyData =
			MaterialInstance.GetEditorOnlyData();
		if (AttachedEditorOnlyData
			&& AttachedEditorOnlyData->GetOuter() == &MaterialInstance
			&& AttachedEditorOnlyData->GetName() == EditorOnlyDataName)
		{
			AttachedEditorOnlyData->ClearFlags(RF_Transient);
			AttachedEditorOnlyData->SetFlags(
				PersistentAssetFlags & RF_PropagateToSubObjects);
			return true;
		}

		UObject* ExistingObject =
			StaticFindObject(nullptr, &MaterialInstance, *EditorOnlyDataName);
		UMaterialInstanceEditorOnlyData* RepairedEditorOnlyData =
			Cast<UMaterialInstanceEditorOnlyData>(ExistingObject);
		if (ExistingObject && !RepairedEditorOnlyData)
		{
			OutError = FString::Printf(
				TEXT("Could not repair %s because subobject %s exists with incompatible class %s."),
				*MaterialInstance.GetPathName(),
				*EditorOnlyDataName,
				*ExistingObject->GetClass()->GetPathName());
			return false;
		}

		if (!RepairedEditorOnlyData)
		{
			RepairedEditorOnlyData =
				Cast<UMaterialInstanceEditorOnlyData>(
					UE::EditorOptional::CreateEditorOptionalObject(
						&MaterialInstance,
						UMaterialInstanceEditorOnlyData::StaticClass(),
						*EditorOnlyDataName));
		}
		if (!RepairedEditorOnlyData)
		{
			OutError = FString::Printf(
				TEXT("Could not recreate MaterialInstanceEditorOnlyData for %s."),
				*MaterialInstance.GetPathName());
			return false;
		}
		if (AttachedEditorOnlyData
			&& AttachedEditorOnlyData != RepairedEditorOnlyData)
		{
			RepairedEditorOnlyData->StaticParameters =
				AttachedEditorOnlyData->StaticParameters;
		}
		RepairedEditorOnlyData->ClearFlags(RF_Transient);
		RepairedEditorOnlyData->SetFlags(
			PersistentAssetFlags & RF_PropagateToSubObjects);

		EditorOnlyDataProperty->SetObjectPropertyValue_InContainer(
			&MaterialInstance,
			RepairedEditorOnlyData);
		if (!MaterialInstance.IsEditorOnlyDataValid())
		{
			OutError = FString::Printf(
				TEXT("MaterialInstanceEditorOnlyData repair did not attach to %s."),
				*MaterialInstance.GetPathName());
			return false;
		}
		UE_LOG(
			LogFoliageBakerAssetBuilder,
			Warning,
			TEXT("Recreated missing MaterialInstanceEditorOnlyData for legacy damaged MIC %s."),
			*MaterialInstance.GetPathName());
#endif
		return true;
	}

	bool RestoreSnapshotProperties(UObject& Backup, UObject& Original)
	{
		UEngine::FCopyPropertiesForUnrelatedObjectsParams RestoreParams;
		RestoreParams.bDoDelta = false;

#if WITH_EDITORONLY_DATA
		UMaterialInstanceConstant* OriginalMaterialInstance =
			Cast<UMaterialInstanceConstant>(&Original);
		UMaterialInstanceConstant* BackupMaterialInstance =
			Cast<UMaterialInstanceConstant>(&Backup);
		if (OriginalMaterialInstance && BackupMaterialInstance)
		{
			FString RepairError;
			if (!EnsureMaterialInstanceEditorOnlyData(
					*OriginalMaterialInstance,
					RepairError)
				|| !BackupMaterialInstance->IsEditorOnlyDataValid())
			{
				UE_LOG(
					LogFoliageBakerAssetBuilder,
					Error,
					TEXT("Could not safely restore MIC snapshot for %s: %s"),
					*Original.GetPathName(),
					RepairError.IsEmpty()
						? TEXT("the snapshot has no MaterialInstanceEditorOnlyData")
						: *RepairError);
				return false;
			}

			UMaterialInstanceEditorOnlyData* OriginalEditorOnlyData =
				OriginalMaterialInstance->GetEditorOnlyData();
			UMaterialInstanceEditorOnlyData* BackupEditorOnlyData =
				BackupMaterialInstance->GetEditorOnlyData();
			TMap<UObject*, UObject*> ReplacementMappings;
			ReplacementMappings.Add(&Backup, &Original);
			ReplacementMappings.Add(
				BackupEditorOnlyData,
				OriginalEditorOnlyData);
			RestoreParams.OptionalReplacementMappings =
				&ReplacementMappings;
			RestoreParams.bReplaceInternalReferenceUponRead = true;
			UEngine::CopyPropertiesForUnrelatedObjects(
				&Backup,
				&Original,
				RestoreParams);

			if (!EnsureMaterialInstanceEditorOnlyData(
					*OriginalMaterialInstance,
					RepairError))
			{
				UE_LOG(
					LogFoliageBakerAssetBuilder,
					Error,
					TEXT("MIC snapshot restore detached MaterialInstanceEditorOnlyData for %s: %s"),
					*Original.GetPathName(),
					*RepairError);
				return false;
			}
			OriginalMaterialInstance->GetEditorOnlyData()->StaticParameters =
				BackupEditorOnlyData->StaticParameters;
			return true;
		}
#endif

		UEngine::CopyPropertiesForUnrelatedObjects(
			&Backup,
			&Original,
			RestoreParams);
		return true;
	}

	void PrepareMeshDescriptionMaterialSlotNames(
		FMeshDescription& MeshDescription,
		const FName ProxyMaterialSlotName,
		const FName LegacyProxyMaterialSlotName,
		const bool bPreserveAdditionalMaterialSlots)
	{
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register(true);
		TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID PolygonGroupID : MeshDescription.PolygonGroups().GetElementIDs())
		{
			if (!bPreserveAdditionalMaterialSlots
				|| MaterialSlotNames[PolygonGroupID].IsNone()
				|| MaterialSlotNames[PolygonGroupID] == LegacyProxyMaterialSlotName)
			{
				MaterialSlotNames[PolygonGroupID] = ProxyMaterialSlotName;
			}
		}
	}

	int32 FindMaterialSlotIndex(const TArray<FStaticMaterial>& StaticMaterials, const FName MaterialSlotName)
	{
		for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
		{
			const FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
			if (StaticMaterial.MaterialSlotName == MaterialSlotName
				|| StaticMaterial.ImportedMaterialSlotName == MaterialSlotName)
			{
				return MaterialIndex;
			}
		}
		return INDEX_NONE;
	}

	void AssignMaterialSlot(
		FStaticMaterial& StaticMaterial,
		UMaterialInterface* Material,
		const FName MaterialSlotName)
	{
		StaticMaterial.MaterialInterface = Material;
		StaticMaterial.MaterialSlotName = MaterialSlotName;
		StaticMaterial.ImportedMaterialSlotName = MaterialSlotName;
	}

	int32 EnsureNamedMaterialSlot(
		UStaticMesh& StaticMesh,
		UMaterialInterface* MaterialInterface,
		const FName RequestedSlotName)
	{
		UMaterialInterface* Material = ResolveProxyMaterial(MaterialInterface);
		const FName MaterialSlotName = RequestedSlotName.IsNone()
			? Material->GetFName()
			: RequestedSlotName;
		TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
		const int32 MaterialIndex = FindMaterialSlotIndex(StaticMaterials, MaterialSlotName);
		if (MaterialIndex != INDEX_NONE)
		{
			AssignMaterialSlot(StaticMaterials[MaterialIndex], Material, MaterialSlotName);
			return MaterialIndex;
		}

		return StaticMaterials.Add(FStaticMaterial(Material, MaterialSlotName, MaterialSlotName));
	}

	void EnsureAdditionalMaterialSlots(
		UStaticMesh& StaticMesh,
		const TArray<FFoliageBakerMeshMaterialSlot>& AdditionalMaterialSlots)
	{
		for (const FFoliageBakerMeshMaterialSlot& MaterialSlot : AdditionalMaterialSlots)
		{
			if (!MaterialSlot.MaterialSlotName.IsNone())
			{
				EnsureNamedMaterialSlot(StaticMesh, MaterialSlot.Material, MaterialSlot.MaterialSlotName);
			}
		}
	}

	int32 EnsureProxyMaterialSlot(UStaticMesh& StaticMesh, UMaterialInterface* ProxyMaterial, const FName LegacySlotName)
	{
		UMaterialInterface* Material = ResolveProxyMaterial(ProxyMaterial);
		const FName MaterialSlotName = Material->GetFName();
		TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
		int32 MaterialIndex = FindMaterialSlotIndex(StaticMaterials, MaterialSlotName);
		if (MaterialIndex == INDEX_NONE && LegacySlotName != MaterialSlotName)
		{
			MaterialIndex = FindMaterialSlotIndex(StaticMaterials, LegacySlotName);
		}
		if (MaterialIndex != INDEX_NONE)
		{
			AssignMaterialSlot(StaticMaterials[MaterialIndex], Material, MaterialSlotName);
			return MaterialIndex;
		}

		return StaticMaterials.Add(FStaticMaterial(Material, MaterialSlotName, MaterialSlotName));
	}

	void AddMaterialIndexIfValid(
		const UStaticMesh& StaticMesh,
		const int32 MaterialIndex,
		TSet<int32>& OutMaterialIndices)
	{
		if (StaticMesh.GetStaticMaterials().IsValidIndex(MaterialIndex))
		{
			OutMaterialIndices.Add(MaterialIndex);
		}
	}

	void AddMeshDescriptionMaterialIndices(
		const UStaticMesh& StaticMesh,
		const int32 LODIndex,
		TSet<int32>& OutMaterialIndices)
	{
		const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription(LODIndex);
		if (!MeshDescription
			|| !MeshDescription->PolygonGroupAttributes().HasAttribute(
				MeshAttribute::PolygonGroup::ImportedMaterialSlotName))
		{
			return;
		}

		const TPolygonGroupAttributesConstRef<FName> MaterialSlotNames =
			FStaticMeshConstAttributes(*MeshDescription).GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID PolygonGroupID : MeshDescription->PolygonGroups().GetElementIDs())
		{
			const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
			int32 MaterialIndex = StaticMesh.GetMaterialIndex(MaterialSlotName);
			if (MaterialIndex == INDEX_NONE)
			{
				MaterialIndex = StaticMesh.GetMaterialIndexFromImportedMaterialSlotName(
					MaterialSlotName);
			}
			AddMaterialIndexIfValid(StaticMesh, MaterialIndex, OutMaterialIndices);
		}
	}

	void AddLODMaterialIndices(
		const UStaticMesh& StaticMesh,
		const int32 LODIndex,
		TSet<int32>& OutMaterialIndices)
	{
		if (const FStaticMeshRenderData* RenderData = StaticMesh.GetRenderData();
			RenderData && RenderData->LODResources.IsValidIndex(LODIndex))
		{
			for (const FStaticMeshSection& Section : RenderData->LODResources[LODIndex].Sections)
			{
				AddMaterialIndexIfValid(StaticMesh, Section.MaterialIndex, OutMaterialIndices);
			}
		}

		auto AddSectionInfoMapIndices = [&](const FMeshSectionInfoMap& SectionInfoMap)
		{
			for (const TPair<uint32, FMeshSectionInfo>& Entry : SectionInfoMap.Map)
			{
				if (static_cast<int32>(Entry.Key >> 16) == LODIndex)
				{
					AddMaterialIndexIfValid(
						StaticMesh,
						Entry.Value.MaterialIndex,
						OutMaterialIndices);
				}
			}
		};
		AddSectionInfoMapIndices(StaticMesh.GetSectionInfoMap());
		AddSectionInfoMapIndices(StaticMesh.GetOriginalSectionInfoMap());
		AddMeshDescriptionMaterialIndices(StaticMesh, LODIndex, OutMaterialIndices);
	}

	void AddCurrentMaterialReferences(
		const UStaticMesh& StaticMesh,
		TSet<int32>& OutMaterialIndices)
	{
		auto AddSectionInfoMapIndices = [&](const FMeshSectionInfoMap& SectionInfoMap)
		{
			for (const TPair<uint32, FMeshSectionInfo>& Entry : SectionInfoMap.Map)
			{
				AddMaterialIndexIfValid(
					StaticMesh,
					Entry.Value.MaterialIndex,
					OutMaterialIndices);
			}
		};
		AddSectionInfoMapIndices(StaticMesh.GetSectionInfoMap());
		AddSectionInfoMapIndices(StaticMesh.GetOriginalSectionInfoMap());

		for (int32 LODIndex = 0; LODIndex < StaticMesh.GetNumSourceModels(); ++LODIndex)
		{
			AddMeshDescriptionMaterialIndices(
				StaticMesh,
				LODIndex,
				OutMaterialIndices);
		}
	}

	void RemapSectionInfoAfterMaterialSlotRemoval(
		FMeshSectionInfoMap& SectionInfoMap,
		const int32 RemovedMaterialIndex)
	{
		for (TPair<uint32, FMeshSectionInfo>& Entry : SectionInfoMap.Map)
		{
			if (Entry.Value.MaterialIndex > RemovedMaterialIndex)
			{
				--Entry.Value.MaterialIndex;
			}
		}
	}

	void RemoveReplacedLODExclusiveMaterialSlots(
		UStaticMesh& StaticMesh,
		const TSet<int32>& ReplacedLODMaterialIndices,
		const TSet<int32>& OtherLODMaterialIndices)
	{
		TSet<int32> ReferencedMaterialIndices = OtherLODMaterialIndices;
		AddCurrentMaterialReferences(StaticMesh, ReferencedMaterialIndices);

		TArray<int32> MaterialIndicesToRemove;
		for (const int32 MaterialIndex : ReplacedLODMaterialIndices)
		{
			if (StaticMesh.GetStaticMaterials().IsValidIndex(MaterialIndex)
				&& !ReferencedMaterialIndices.Contains(MaterialIndex))
			{
				MaterialIndicesToRemove.Add(MaterialIndex);
			}
		}
		MaterialIndicesToRemove.Sort(TGreater<int32>());

		for (const int32 MaterialIndex : MaterialIndicesToRemove)
		{
			StaticMesh.GetStaticMaterials().RemoveAt(MaterialIndex);
			RemapSectionInfoAfterMaterialSlotRemoval(
				StaticMesh.GetSectionInfoMap(),
				MaterialIndex);
			RemapSectionInfoAfterMaterialSlotRemoval(
				StaticMesh.GetOriginalSectionInfoMap(),
				MaterialIndex);
		}
	}

	void ConfigureProxySourceModel(
		UStaticMesh& StaticMesh,
		const int32 LODIndex,
		const bool bPreserveExistingScreenSize,
		const bool bRecomputeNormals,
		const bool bRecomputeTangents,
		const int32 BaseLODModel)
	{
		if (!StaticMesh.IsSourceModelValid(LODIndex))
		{
			return;
		}

		FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(LODIndex);
		const float ExistingScreenSize = SourceModel.ScreenSize.Default;
		SourceModel.BuildSettings.bRecomputeNormals = bRecomputeNormals;
		SourceModel.BuildSettings.bRecomputeTangents = bRecomputeTangents;
		if (bRecomputeTangents)
		{
			SourceModel.BuildSettings.bUseMikkTSpace = true;
		}
		SourceModel.BuildSettings.bRemoveDegenerates = false;
		SourceModel.BuildSettings.bGenerateLightmapUVs = false;
		SourceModel.BuildSettings.SrcLightmapIndex = 0;
		SourceModel.BuildSettings.DstLightmapIndex = 0;
		SourceModel.BuildSettings.bUseFullPrecisionUVs = false;
		SourceModel.BuildSettings.DistanceFieldResolutionScale = 1.0f;
		SourceModel.ReductionSettings.PercentTriangles = 1.0f;
		SourceModel.ReductionSettings.PercentVertices = 1.0f;
		SourceModel.ReductionSettings.BaseLODModel = BaseLODModel == INDEX_NONE
			? LODIndex
			: BaseLODModel;

		if (bPreserveExistingScreenSize)
		{
			SourceModel.ScreenSize.Default = ExistingScreenSize;
		}
		else if (LODIndex == 0)
		{
			SourceModel.ScreenSize.Default = 1.0f;
		}
		else if (StaticMesh.IsSourceModelValid(LODIndex - 1))
		{
			const float PreviousScreenSize = StaticMesh.GetSourceModel(LODIndex - 1).ScreenSize.Default;
			SourceModel.ScreenSize.Default = FMath::Clamp(PreviousScreenSize * 0.5f, 0.01f, 0.99f);
		}
		else
		{
			SourceModel.ScreenSize.Default = 0.01f;
		}
	}

	void KeepOnlyUVChannels(
		UStaticMesh& StaticMesh,
		const int32 LODIndex,
		const int32 DesiredChannelCount,
		const bool bSetGlobalLightMapCoordinateIndex)
	{
		if (bSetGlobalLightMapCoordinateIndex)
		{
			StaticMesh.SetLightMapCoordinateIndex(0);
		}
		const int32 ClampedDesiredChannelCount = FMath::Clamp(DesiredChannelCount, 1, 8);

		if (StaticMesh.IsSourceModelValid(LODIndex))
		{
			FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(LODIndex);
			SourceModel.BuildSettings.bGenerateLightmapUVs = false;
			SourceModel.BuildSettings.SrcLightmapIndex = 0;
			SourceModel.BuildSettings.DstLightmapIndex = 0;
			SourceModel.BuildSettings.bUseFullPrecisionUVs = false;
		}

		if (FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription(LODIndex))
		{
			TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = FStaticMeshAttributes(*MeshDescription).GetVertexInstanceUVs();
			if (VertexInstanceUVs.GetNumChannels() != ClampedDesiredChannelCount)
			{
				VertexInstanceUVs.SetNumChannels(ClampedDesiredChannelCount);
				StaticMesh.CommitMeshDescription(LODIndex);
			}
		}

		for (int32 Guard = 0; Guard < 8 && StaticMesh.GetNumUVChannels(LODIndex) > ClampedDesiredChannelCount; ++Guard)
		{
			if (!StaticMesh.RemoveUVChannel(LODIndex, ClampedDesiredChannelCount))
			{
				break;
			}
		}

		if (bSetGlobalLightMapCoordinateIndex)
		{
			StaticMesh.SetLightMapCoordinateIndex(0);
		}
	}

	void RemoveSectionInfoForLOD(FMeshSectionInfoMap& SectionInfoMap, const int32 LODIndex)
	{
		for (auto It = SectionInfoMap.Map.CreateIterator(); It; ++It)
		{
			if (static_cast<int32>(It.Key() >> 16) == LODIndex)
			{
				It.RemoveCurrent();
			}
		}
	}

	void ClearSectionInfoForLOD(UStaticMesh& StaticMesh, const int32 LODIndex)
	{
		RemoveSectionInfoForLOD(StaticMesh.GetSectionInfoMap(), LODIndex);
		RemoveSectionInfoForLOD(StaticMesh.GetOriginalSectionInfoMap(), LODIndex);
	}

	void ConfigureSectionMaterialsFromMeshDescription(
		UStaticMesh& StaticMesh,
		const int32 LODIndex,
		const FMeshDescription& MeshDescription,
		const int32 FallbackMaterialIndex)
	{
		const TPolygonGroupAttributesConstRef<FName> MaterialSlotNames =
			FStaticMeshConstAttributes(MeshDescription).GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID PolygonGroupID : MeshDescription.PolygonGroups().GetElementIDs())
		{
			const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
			int32 MaterialIndex = StaticMesh.GetMaterialIndex(MaterialSlotName);
			if (MaterialIndex == INDEX_NONE)
			{
				MaterialIndex = StaticMesh.GetMaterialIndexFromImportedMaterialSlotName(MaterialSlotName);
			}
			if (MaterialIndex == INDEX_NONE)
			{
				MaterialIndex = FallbackMaterialIndex;
			}

			FMeshSectionInfo SectionInfo;
			SectionInfo.MaterialIndex = MaterialIndex;
			const int32 SectionIndex = PolygonGroupID.GetValue();
			StaticMesh.GetSectionInfoMap().Set(LODIndex, SectionIndex, SectionInfo);
			StaticMesh.GetOriginalSectionInfoMap().Set(LODIndex, SectionIndex, SectionInfo);
		}
	}

	void ShiftSectionInfoMapForInsertedLOD(
		FMeshSectionInfoMap& SectionInfoMap,
		const int32 OldLODCount,
		const int32 InsertLODIndex)
	{
		for (int32 SourceLODIndex = OldLODCount - 1; SourceLODIndex >= InsertLODIndex; --SourceLODIndex)
		{
			const int32 DestinationLODIndex = SourceLODIndex + 1;
			const int32 SourceSectionCount = SectionInfoMap.GetSectionNumber(SourceLODIndex);
			TArray<FMeshSectionInfo> SourceSections;
			SourceSections.Reserve(SourceSectionCount);
			for (int32 SectionIndex = 0; SectionIndex < SourceSectionCount; ++SectionIndex)
			{
				SourceSections.Add(SectionInfoMap.Get(SourceLODIndex, SectionIndex));
			}

			RemoveSectionInfoForLOD(SectionInfoMap, DestinationLODIndex);
			for (int32 SectionIndex = 0; SectionIndex < SourceSections.Num(); ++SectionIndex)
			{
				SectionInfoMap.Set(DestinationLODIndex, SectionIndex, SourceSections[SectionIndex]);
			}
			RemoveSectionInfoForLOD(SectionInfoMap, SourceLODIndex);
		}
	}

	void ShiftLODIndexForInsertion(int32& LODIndex, const int32 InsertLODIndex)
	{
		if (LODIndex >= InsertLODIndex)
		{
			++LODIndex;
		}
	}

	void ShiftStaticMeshLODReferencesForInsertion(UStaticMesh& StaticMesh, const int32 InsertLODIndex)
	{
		FPerPlatformInt MinimumLOD = StaticMesh.GetMinLOD();
		ShiftLODIndexForInsertion(MinimumLOD.Default, InsertLODIndex);
		for (TPair<FName, int32>& Entry : MinimumLOD.PerPlatform)
		{
			ShiftLODIndexForInsertion(Entry.Value, InsertLODIndex);
		}
		StaticMesh.SetMinLOD(MoveTemp(MinimumLOD));

		FPerQualityLevelInt QualityMinimumLOD = StaticMesh.GetQualityLevelMinLOD();
		ShiftLODIndexForInsertion(QualityMinimumLOD.Default, InsertLODIndex);
		for (TPair<int32, int32>& Entry : QualityMinimumLOD.PerQuality)
		{
			ShiftLODIndexForInsertion(Entry.Value, InsertLODIndex);
		}
		StaticMesh.SetQualityLevelMinLOD(MoveTemp(QualityMinimumLOD));
		ShiftLODIndexForInsertion(StaticMesh.LODForCollision, InsertLODIndex);
	}

	void InsertSourceModel(UStaticMesh& StaticMesh, const int32 InsertLODIndex)
	{
		const int32 OldLODCount = StaticMesh.GetNumSourceModels();
		ShiftSectionInfoMapForInsertedLOD(StaticMesh.GetSectionInfoMap(), OldLODCount, InsertLODIndex);
		ShiftSectionInfoMapForInsertedLOD(StaticMesh.GetOriginalSectionInfoMap(), OldLODCount, InsertLODIndex);

		TArray<FStaticMeshSourceModel> SourceModels = StaticMesh.MoveSourceModels();
		SourceModels.InsertDefaulted(InsertLODIndex);
		SourceModels[InsertLODIndex].CreateSubObjects(&StaticMesh);
		StaticMesh.SetSourceModels(MoveTemp(SourceModels));

		for (int32 LODIndex = 0; LODIndex < StaticMesh.GetNumSourceModels(); ++LODIndex)
		{
			if (LODIndex == InsertLODIndex)
			{
				continue;
			}
			FMeshReductionSettings& ReductionSettings = StaticMesh.GetSourceModel(LODIndex).ReductionSettings;
			if (ReductionSettings.BaseLODModel >= InsertLODIndex)
			{
				++ReductionSettings.BaseLODModel;
			}
		}
		ShiftStaticMeshLODReferencesForInsertion(StaticMesh, InsertLODIndex);
	}

	struct FGeneratedLODMetadataValue
	{
		FName Key = NAME_None;
		int32 LODIndex = INDEX_NONE;
	};

	TArray<FGeneratedLODMetadataValue> FindGeneratedLODMetadataAtOrAfter(
		const UStaticMesh& StaticMesh,
		const int32 MinimumLODIndex)
	{
		TArray<FGeneratedLODMetadataValue> Result;
		const TMap<FName, FString>* ObjectMetadata = FMetaData::GetMapForObject(&StaticMesh);
		if (!ObjectMetadata)
		{
			return Result;
		}

		for (const TPair<FName, FString>& Entry : *ObjectMetadata)
		{
			const FString KeyString = Entry.Key.ToString();
			int32 ExistingLODIndex = INDEX_NONE;
			if (KeyString.StartsWith(TEXT("FoliageBaker."), ESearchCase::CaseSensitive)
				&& KeyString.EndsWith(TEXT("LOD"), ESearchCase::CaseSensitive)
				&& LexTryParseString(ExistingLODIndex, *Entry.Value)
				&& ExistingLODIndex >= MinimumLODIndex
				&& StaticMesh.IsSourceModelValid(ExistingLODIndex))
			{
				Result.Add({ Entry.Key, ExistingLODIndex });
			}
		}
		return Result;
	}

	void NormalizeEncodedNormalPixels(FColor* Pixels, const int32 PixelCount)
	{
		if (!Pixels || PixelCount <= 0)
		{
			return;
		}

		for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
		{
			FColor& Pixel = Pixels[PixelIndex];
			const FVector Normal(
				static_cast<double>(Pixel.R) / 255.0 * 2.0 - 1.0,
				static_cast<double>(Pixel.G) / 255.0 * 2.0 - 1.0,
				static_cast<double>(Pixel.B) / 255.0 * 2.0 - 1.0);
			const FVector SafeNormal = Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			Pixel.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.X * 0.5 + 0.5) * 255.0), 0, 255));
			Pixel.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.Y * 0.5 + 0.5) * 255.0), 0, 255));
			Pixel.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((SafeNormal.Z * 0.5 + 0.5) * 255.0), 0, 255));
		}
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

	FIntPoint EncodeOctahedralNormalRG(const FVector& InNormal)
	{
		const FVector Normal =
			InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const double L1Norm = FMath::Abs(Normal.X)
			+ FMath::Abs(Normal.Y)
			+ FMath::Abs(Normal.Z);
		const FVector Projected =
			Normal / FMath::Max(L1Norm, UE_DOUBLE_SMALL_NUMBER);
		FVector2D Octahedral(Projected.X, Projected.Y);
		if (Projected.Z < 0.0)
		{
			const double OldX = Octahedral.X;
			Octahedral.X = (1.0 - FMath::Abs(Octahedral.Y))
				* (OldX >= 0.0 ? 1.0 : -1.0);
			Octahedral.Y = (1.0 - FMath::Abs(OldX))
				* (Octahedral.Y >= 0.0 ? 1.0 : -1.0);
		}
		return FIntPoint(
			FMath::Clamp(
				FMath::RoundToInt((Octahedral.X * 0.5 + 0.5) * 255.0),
				0,
				255),
			FMath::Clamp(
				FMath::RoundToInt((Octahedral.Y * 0.5 + 0.5) * 255.0),
				0,
				255));
	}

	void GenerateImpostorNormalMaskDepthMip(
		const FColor* SourcePixels,
		const int32 SourceWidth,
		const int32 SourceHeight,
		FColor* DestinationPixels,
		const int32 DestinationWidth,
		const int32 DestinationHeight)
	{
		if (!SourcePixels
			|| !DestinationPixels
			|| SourceWidth <= 0
			|| SourceHeight <= 0
			|| DestinationWidth <= 0
			|| DestinationHeight <= 0)
		{
			return;
		}

		constexpr uint8 TrunkMaskThreshold = 64;
		constexpr uint8 LeafMaskThreshold = 192;
		for (int32 DestinationY = 0; DestinationY < DestinationHeight; ++DestinationY)
		{
			const int32 SourceMinY = FMath::Clamp(
				DestinationY * SourceHeight / DestinationHeight,
				0,
				SourceHeight - 1);
			const int32 SourceMaxY = FMath::Clamp(
				FMath::DivideAndRoundUp(
					(DestinationY + 1) * SourceHeight,
					DestinationHeight),
				SourceMinY + 1,
				SourceHeight);
			for (int32 DestinationX = 0; DestinationX < DestinationWidth; ++DestinationX)
			{
				const int32 SourceMinX = FMath::Clamp(
					DestinationX * SourceWidth / DestinationWidth,
					0,
					SourceWidth - 1);
				const int32 SourceMaxX = FMath::Clamp(
					FMath::DivideAndRoundUp(
						(DestinationX + 1) * SourceWidth,
						DestinationWidth),
					SourceMinX + 1,
					SourceWidth);

				FVector NormalSum = FVector::ZeroVector;
				int32 TrunkSampleCount = 0;
				int32 LeafSampleCount = 0;
				for (int32 SourceY = SourceMinY; SourceY < SourceMaxY; ++SourceY)
				{
					for (int32 SourceX = SourceMinX; SourceX < SourceMaxX; ++SourceX)
					{
						const FColor& SourcePixel =
							SourcePixels[SourceY * SourceWidth + SourceX];
						NormalSum += DecodeOctahedralNormal(SourcePixel);
						if (SourcePixel.B >= LeafMaskThreshold)
						{
							++LeafSampleCount;
						}
						else if (SourcePixel.B >= TrunkMaskThreshold)
						{
							++TrunkSampleCount;
						}
					}
				}

				FColor& DestinationPixel =
					DestinationPixels[DestinationY * DestinationWidth + DestinationX];
				const FIntPoint EncodedNormal =
					EncodeOctahedralNormalRG(NormalSum);
				DestinationPixel.R = static_cast<uint8>(EncodedNormal.X);
				DestinationPixel.G = static_cast<uint8>(EncodedNormal.Y);
				DestinationPixel.B =
					LeafSampleCount + TrunkSampleCount == 0
						? 0
						: LeafSampleCount >= TrunkSampleCount
							? 255
							: 128;
			}
		}
	}

	void GenerateSemanticMaskMipAlpha(
		const uint8* SourceAlphaValues,
		const int32 SourceWidth,
		const int32 SourceHeight,
		FColor* DestinationPixels,
		const int32 DestinationWidth,
		const int32 DestinationHeight,
		const float CoverageThreshold)
	{
		if (!SourceAlphaValues
			|| !DestinationPixels
			|| SourceWidth <= 0
			|| SourceHeight <= 0
			|| DestinationWidth <= 0
			|| DestinationHeight <= 0)
		{
			return;
		}

		const double SafeCoverageThreshold = FMath::Clamp(
			static_cast<double>(CoverageThreshold),
			0.01,
			1.0);
		constexpr uint8 TrunkAlphaThreshold = 64;
		constexpr uint8 LeafAlphaThreshold = 192;

		for (int32 DestinationY = 0; DestinationY < DestinationHeight; ++DestinationY)
		{
			const int32 SourceMinY = FMath::Clamp(
				DestinationY * SourceHeight / DestinationHeight,
				0,
				SourceHeight - 1);
			const int32 SourceMaxY = FMath::Clamp(
				FMath::DivideAndRoundUp(
					(DestinationY + 1) * SourceHeight,
					DestinationHeight),
				SourceMinY + 1,
				SourceHeight);

			for (int32 DestinationX = 0; DestinationX < DestinationWidth; ++DestinationX)
			{
				const int32 SourceMinX = FMath::Clamp(
					DestinationX * SourceWidth / DestinationWidth,
					0,
					SourceWidth - 1);
				const int32 SourceMaxX = FMath::Clamp(
					FMath::DivideAndRoundUp(
						(DestinationX + 1) * SourceWidth,
						DestinationWidth),
					SourceMinX + 1,
					SourceWidth);

				int32 LeafSampleCount = 0;
				int32 TrunkSampleCount = 0;
				for (int32 SourceY = SourceMinY; SourceY < SourceMaxY; ++SourceY)
				{
					for (int32 SourceX = SourceMinX; SourceX < SourceMaxX; ++SourceX)
					{
						const uint8 SourceAlpha =
							SourceAlphaValues[SourceY * SourceWidth + SourceX];
						if (SourceAlpha >= LeafAlphaThreshold)
						{
							++LeafSampleCount;
						}
						else if (SourceAlpha >= TrunkAlphaThreshold)
						{
							++TrunkSampleCount;
						}
					}
				}

				const int32 SampleCount =
					(SourceMaxX - SourceMinX) * (SourceMaxY - SourceMinY);
				const int32 CoveredSampleCount = LeafSampleCount + TrunkSampleCount;
				const double Coverage = SampleCount > 0
					? static_cast<double>(CoveredSampleCount) / static_cast<double>(SampleCount)
					: 0.0;
				FColor& DestinationPixel =
					DestinationPixels[DestinationY * DestinationWidth + DestinationX];
				if (Coverage < SafeCoverageThreshold)
				{
					DestinationPixel.A = 0;
				}
				else
				{
					DestinationPixel.A = LeafSampleCount >= TrunkSampleCount
						? 255
						: 128;
				}
			}
		}
	}

	bool InitializeTileIsolatedTextureSource(
		UTexture2D& Texture,
		const FFoliageBakerTextureAssetParams& Params,
		const TArray<FColor>& Pixels,
		FString& OutError)
	{
		if (Params.Width <= 0
			|| Params.Height <= 0
			|| Pixels.Num() != Params.Width * Params.Height)
		{
			OutError = TEXT("The atlas pixel count does not match the requested texture dimensions.");
			return false;
		}

		TArray<FIntRect> TileRects;
		TileRects.Reserve(Params.MipTileRects.Num());
		for (const FIntRect& RawTileRect : Params.MipTileRects)
		{
			const FIntRect TileRect(
				FIntPoint(
					FMath::Clamp(RawTileRect.Min.X, 0, Params.Width),
					FMath::Clamp(RawTileRect.Min.Y, 0, Params.Height)),
				FIntPoint(
					FMath::Clamp(RawTileRect.Max.X, 0, Params.Width),
					FMath::Clamp(RawTileRect.Max.Y, 0, Params.Height)));
			if (TileRect.Width() <= 0 || TileRect.Height() <= 0)
			{
				continue;
			}
			TileRects.Add(TileRect);
		}
		if (TileRects.IsEmpty())
		{
			OutError = TEXT("No valid atlas tile rectangles were provided for mip generation.");
			return false;
		}

		// Freeze UV-island padding ownership at mip 0. Every atlas pixel and
		// every lower mip samples exactly one independently filtered tile.
		TArray<uint16> TileOwners;
		if (!UE::FoliageBaker::Atlas::BuildTileOwnerMap(
				Params.Width,
				Params.Height,
				TileRects,
				TileOwners))
		{
			OutError = TEXT("The mip-0 atlas tile rectangles overlap or could not be assigned.");
			return false;
		}

		const EGammaSpace GammaSpace =
			Params.bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		const float SemanticMaskMipCoverageThreshold =
			Params.SemanticMaskMipCoverageThreshold > 0.0f
				? FMath::Clamp(
					Params.SemanticMaskMipCoverageThreshold,
					0.01f,
					1.0f)
				: 0.0f;

		Texture.Source.Init2DWithMipChain(
			Params.Width,
			Params.Height,
			TSF_BGRA8);
		const int32 NumMips = Texture.Source.GetNumMips();
		if (NumMips <= 0)
		{
			OutError = TEXT("Could not allocate the texture source mip chain.");
			return false;
		}

		TArray<TArray<uint8>> MipZeroTileAlpha;
		if (SemanticMaskMipCoverageThreshold > 0.0f)
		{
			MipZeroTileAlpha.SetNum(TileRects.Num());
		}
		TArray<FImage> CurrentTileMips;
		CurrentTileMips.Reserve(TileRects.Num());
		auto ApplyTileMipSemantics = [&](
			const FImage& SourceTile,
			FImage& DestinationTile,
			const int32 TileIndex)
		{
			FColor* DestinationPixels =
				reinterpret_cast<FColor*>(DestinationTile.RawData.GetData());
			const int32 DestinationPixelCount =
				static_cast<int32>(DestinationTile.SizeX * DestinationTile.SizeY);
			if (Params.MipMode == EFoliageBakerTextureMipMode::NormalizeXYZNormal)
			{
				NormalizeEncodedNormalPixels(
					DestinationPixels,
					DestinationPixelCount);
			}
			else if (
				Params.MipMode
				== EFoliageBakerTextureMipMode::ImpostorOctaNormalMaskDepth)
			{
				GenerateImpostorNormalMaskDepthMip(
					reinterpret_cast<const FColor*>(
						SourceTile.RawData.GetData()),
					static_cast<int32>(SourceTile.SizeX),
					static_cast<int32>(SourceTile.SizeY),
					DestinationPixels,
					static_cast<int32>(DestinationTile.SizeX),
					static_cast<int32>(DestinationTile.SizeY));
			}
			if (SemanticMaskMipCoverageThreshold > 0.0f)
			{
				const FIntRect& TileRect = TileRects[TileIndex];
				GenerateSemanticMaskMipAlpha(
					MipZeroTileAlpha[TileIndex].GetData(),
					TileRect.Width(),
					TileRect.Height(),
					DestinationPixels,
					static_cast<int32>(DestinationTile.SizeX),
					static_cast<int32>(DestinationTile.SizeY),
					SemanticMaskMipCoverageThreshold);
			}
		};

		for (int32 TileIndex = 0; TileIndex < TileRects.Num(); ++TileIndex)
		{
			const FIntRect& TileRect = TileRects[TileIndex];
			FImage TileMipZero(
				TileRect.Width(),
				TileRect.Height(),
				1,
				ERawImageFormat::BGRA8,
				GammaSpace);
			FColor* TileMipZeroPixels =
				reinterpret_cast<FColor*>(TileMipZero.RawData.GetData());
			for (int32 LocalY = 0; LocalY < TileRect.Height(); ++LocalY)
			{
				FMemory::Memcpy(
					TileMipZeroPixels + LocalY * TileRect.Width(),
					Pixels.GetData()
						+ (TileRect.Min.Y + LocalY) * Params.Width
						+ TileRect.Min.X,
					static_cast<SIZE_T>(TileRect.Width()) * sizeof(FColor));
			}
			if (SemanticMaskMipCoverageThreshold > 0.0f)
			{
				TArray<uint8>& TileAlpha = MipZeroTileAlpha[TileIndex];
				TileAlpha.SetNumUninitialized(
					TileRect.Width() * TileRect.Height());
				for (int32 PixelIndex = 0;
					PixelIndex < TileAlpha.Num();
					++PixelIndex)
				{
					TileAlpha[PixelIndex] = TileMipZeroPixels[PixelIndex].A;
				}
			}

			if (NumMips > 1)
			{
				FImage TileMipOne(
					FMath::Max(1, TileRect.Width() >> 1),
					FMath::Max(1, TileRect.Height() >> 1),
					1,
					ERawImageFormat::BGRA8,
					GammaSpace);
				FImageCore::ResizeImage(
					TileMipZero,
					TileMipOne,
					FImageCore::EResizeImageFilter::Box);
				ApplyTileMipSemantics(
					TileMipZero,
					TileMipOne,
					TileIndex);
				CurrentTileMips.Add(MoveTemp(TileMipOne));
			}
		}

		TArray<FColor*> MipData;
		MipData.Reserve(NumMips);
		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			FColor* LockedMip =
				reinterpret_cast<FColor*>(Texture.Source.LockMip(MipIndex));
			if (!LockedMip)
			{
				for (int32 LockedMipIndex = MipData.Num() - 1;
					LockedMipIndex >= 0;
					--LockedMipIndex)
				{
					Texture.Source.UnlockMip(LockedMipIndex);
				}
				OutError = FString::Printf(
					TEXT("Could not lock generated texture source mip %d."),
					MipIndex);
				return false;
			}
			MipData.Add(LockedMip);
		}

		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			if (MipIndex > 1)
			{
				for (int32 TileIndex = 0;
					TileIndex < TileRects.Num();
					++TileIndex)
				{
					const FIntRect& TileRect = TileRects[TileIndex];
					FImage NextTileMip(
						FMath::Max(1, TileRect.Width() >> MipIndex),
						FMath::Max(1, TileRect.Height() >> MipIndex),
						1,
						ERawImageFormat::BGRA8,
						GammaSpace);
					FImageCore::ResizeImage(
						CurrentTileMips[TileIndex],
						NextTileMip,
						FImageCore::EResizeImageFilter::Box);
					ApplyTileMipSemantics(
						CurrentTileMips[TileIndex],
						NextTileMip,
						TileIndex);
					CurrentTileMips[TileIndex].Swap(NextTileMip);
				}
			}

			const int32 MipWidth = FMath::Max(1, Params.Width >> MipIndex);
			const int32 MipHeight = FMath::Max(1, Params.Height >> MipIndex);
			const int32 MipScale = 1 << MipIndex;
			for (int32 Y = 0; Y < MipHeight; ++Y)
			{
				const int32 BaseY = FMath::Min(
					Params.Height - 1,
					Y * MipScale + MipScale / 2);
				for (int32 X = 0; X < MipWidth; ++X)
				{
					const int32 BaseX = FMath::Min(
						Params.Width - 1,
						X * MipScale + MipScale / 2);
					const int32 TileIndex = static_cast<int32>(
						TileOwners[BaseY * Params.Width + BaseX]);
					check(TileRects.IsValidIndex(TileIndex));

					const FIntRect& TileRect = TileRects[TileIndex];
					const int32 TileSourceX = FMath::Clamp(
						BaseX,
						TileRect.Min.X,
						TileRect.Max.X - 1);
					const int32 TileSourceY = FMath::Clamp(
						BaseY,
						TileRect.Min.Y,
						TileRect.Max.Y - 1);

					FColor TileColor;
					if (MipIndex == 0)
					{
						TileColor =
							Pixels[
								TileSourceY * Params.Width
								+ TileSourceX];
					}
					else
					{
						const FImage& TileMip =
							CurrentTileMips[TileIndex];
						const FColor* TileMipData =
							reinterpret_cast<const FColor*>(
								TileMip.RawData.GetData());
						const int32 TileMipWidth =
							static_cast<int32>(TileMip.SizeX);
						const int32 TileMipHeight =
							static_cast<int32>(TileMip.SizeY);
						const int32 TileMipX = FMath::Min(
							TileMipWidth - 1,
							(TileSourceX - TileRect.Min.X)
								* TileMipWidth
								/ TileRect.Width());
						const int32 TileMipY = FMath::Min(
							TileMipHeight - 1,
							(TileSourceY - TileRect.Min.Y)
								* TileMipHeight
								/ TileRect.Height());
						TileColor =
							TileMipData[
								TileMipY * TileMipWidth
								+ TileMipX];
					}

					const bool bInsideTile =
						BaseX >= TileRect.Min.X
						&& BaseX < TileRect.Max.X
						&& BaseY >= TileRect.Min.Y
						&& BaseY < TileRect.Max.Y;
					if (!bInsideTile && !Params.bFillMipPaddingAlpha)
					{
						TileColor.A = Params.MipBackgroundColor.A;
					}
					MipData[MipIndex][Y * MipWidth + X] = TileColor;
				}
			}
		}

		for (int32 MipIndex = MipData.Num() - 1; MipIndex >= 0; --MipIndex)
		{
			Texture.Source.UnlockMip(MipIndex);
		}
		return true;
	}

	bool ResolveSourceLODTarget(
		const UStaticMesh& SourceStaticMesh,
		const FFoliageBakerSourceLODAssetParams& Params,
		int32& OutLODIndex,
		bool& bOutReusingGeneratedLOD,
		FString& OutError)
	{
		OutLODIndex = INDEX_NONE;
		bOutReusingGeneratedLOD = false;
		OutError.Reset();

		if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
		{
			if (!SourceStaticMesh.IsSourceModelValid(Params.RequestedReplaceLODIndex))
			{
				OutError = FString::Printf(
					TEXT("Cannot replace output LOD %d on %s because that source LOD does not exist."),
					Params.RequestedReplaceLODIndex,
					*SourceStaticMesh.GetName());
				return false;
			}
			if (Params.RequestedReplaceLODIndex <= Params.SourceLODIndex)
			{
				OutError = FString::Printf(
					TEXT("Cannot replace LOD %d because the generated proxy must be written after the selected source LOD %d."),
					Params.RequestedReplaceLODIndex,
					Params.SourceLODIndex);
				return false;
			}
			OutLODIndex = Params.RequestedReplaceLODIndex;
		}
		else if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD)
		{
			if (!Params.RebuildLODMetadataKey.IsNone())
			{
				const FString ExistingLODValue = SourceStaticMesh.GetPackage()->GetMetaData().GetValue(
					&SourceStaticMesh,
					Params.RebuildLODMetadataKey);
				int32 ExistingLODIndex = INDEX_NONE;
				if (LexTryParseString(ExistingLODIndex, *ExistingLODValue)
					&& ExistingLODIndex > Params.SourceLODIndex
					&& SourceStaticMesh.IsSourceModelValid(ExistingLODIndex))
				{
					OutLODIndex = ExistingLODIndex;
					bOutReusingGeneratedLOD = true;
				}
			}

			if (!bOutReusingGeneratedLOD)
			{
				OutLODIndex = SourceStaticMesh.GetNumSourceModels();
				if (OutLODIndex >= MAX_STATIC_MESH_LODS)
				{
					OutError = FString::Printf(
						TEXT("Cannot add a proxy LOD to %s because Static Meshes support at most %d LODs."),
						*SourceStaticMesh.GetName(),
						MAX_STATIC_MESH_LODS);
					return false;
				}
			}
		}
		else if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD)
		{
			if (!SourceStaticMesh.IsSourceModelValid(Params.RequestedInsertAfterLODIndex))
			{
				OutError = FString::Printf(
					TEXT("Cannot insert after LOD %d on %s because that source LOD does not exist."),
					Params.RequestedInsertAfterLODIndex,
					*SourceStaticMesh.GetName());
				return false;
			}
			if (Params.RequestedInsertAfterLODIndex < Params.SourceLODIndex)
			{
				OutError = FString::Printf(
					TEXT("Cannot insert after LOD %d because it is before the selected source LOD %d. Insert after the source LOD or a later LOD so the source geometry is not renumbered."),
					Params.RequestedInsertAfterLODIndex,
					Params.SourceLODIndex);
				return false;
			}
			if (SourceStaticMesh.GetNumSourceModels() >= MAX_STATIC_MESH_LODS)
			{
				OutError = FString::Printf(
					TEXT("Cannot insert a proxy LOD into %s because Static Meshes support at most %d LODs."),
					*SourceStaticMesh.GetName(),
					MAX_STATIC_MESH_LODS);
				return false;
			}
			OutLODIndex = Params.RequestedInsertAfterLODIndex + 1;
		}
		else
		{
			OutError = TEXT("A source-mesh LOD output mode is required.");
			return false;
		}

		if (Params.OutputMode != EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD
			&& OutLODIndex <= Params.SourceLODIndex)
		{
			OutError = FString::Printf(
				TEXT("Output LOD %d must be after the selected source LOD %d on %s."),
				OutLODIndex,
				Params.SourceLODIndex,
				*SourceStaticMesh.GetName());
			return false;
		}
		return true;
	}
}

FFoliageBakerAssetTransaction::~FFoliageBakerAssetTransaction()
{
	Rollback();
}

void FFoliageBakerAssetTransaction::Track(UObject* Asset)
{
	if (!bFinished && Asset)
	{
		if (!CreatedAssets.ContainsByPredicate(
				[Asset](const TStrongObjectPtr<UObject>& CreatedAsset)
				{
					return CreatedAsset.Get() == Asset;
				}))
		{
			CreatedAssets.Emplace(Asset);
		}
	}
}

bool FFoliageBakerAssetTransaction::Snapshot(UObject* Asset, FString& OutError)
{
	OutError.Reset();
	if (bFinished)
	{
		OutError = TEXT("Cannot snapshot an asset after its bake transaction has finished.");
		return false;
	}
	if (!IsValid(Asset))
	{
		OutError = TEXT("Cannot snapshot an invalid asset.");
		return false;
	}
	if (CreatedAssets.ContainsByPredicate(
			[Asset](const TStrongObjectPtr<UObject>& CreatedAsset)
			{
				return CreatedAsset.Get() == Asset;
			})
		|| ObjectSnapshots.ContainsByPredicate(
			[Asset](const FObjectSnapshot& Snapshot)
			{
				return Snapshot.Original.Get() == Asset;
			}))
	{
		return true;
	}

	TArray<TStrongObjectPtr<UObject>> AssetToFinish;
	AssetToFinish.Emplace(Asset);
	FinishCompilationForAssets(AssetToFinish);
	const FName BackupName = MakeUniqueObjectName(
		GetTransientPackage(),
		Asset->GetClass(),
		FName(*(Asset->GetName() + TEXT("_FoliageBakerSnapshot"))));
	UObject* Backup = DuplicateObject<UObject>(Asset, GetTransientPackage(), BackupName);
	if (!Backup)
	{
		OutError = FString::Printf(TEXT("Could not snapshot %s before rebaking it."), *Asset->GetPathName());
		return false;
	}
	Backup->ClearFlags(RF_Public | RF_Standalone);
	Backup->SetFlags(RF_Transient);

	FObjectSnapshot Snapshot;
	Snapshot.Original.Reset(Asset);
	Snapshot.Backup = TStrongObjectPtr<UObject>(Backup);
	Snapshot.bPackageWasDirty = Asset->GetOutermost()->IsDirty();
	Snapshot.ObjectFlags = Asset->GetFlags();
	ObjectSnapshots.Add(MoveTemp(Snapshot));
	return true;
}

void FFoliageBakerAssetTransaction::SnapshotMetadata(UObject* Asset, const FName Key)
{
	if (bFinished || !IsValid(Asset) || Key.IsNone()
		|| MetadataSnapshots.ContainsByPredicate([Asset, Key](const FMetadataSnapshot& Snapshot)
		{
			return Snapshot.Asset.Get() == Asset && Snapshot.Key == Key;
		}))
	{
		return;
	}

	FMetaData& MetaData = Asset->GetPackage()->GetMetaData();
	FMetadataSnapshot Snapshot;
	Snapshot.Asset.Reset(Asset);
	Snapshot.Key = Key;
	Snapshot.bHadValue = MetaData.HasValue(Asset, Key);
	if (Snapshot.bHadValue)
	{
		Snapshot.Value = MetaData.GetValue(Asset, Key);
	}
	MetadataSnapshots.Add(MoveTemp(Snapshot));
}

void FFoliageBakerAssetTransaction::Commit()
{
	if (bFinished)
	{
		return;
	}

	TArray<TStrongObjectPtr<UObject>> TouchedAssets = CreatedAssets;
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		TouchedAssets.Add(Snapshot.Original);
	}
	FinishCompilationForAssets(TouchedAssets);

	for (const TStrongObjectPtr<UObject>& Asset : CreatedAssets)
	{
		if (IsValid(Asset.Get()))
		{
			Asset->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Asset.Get());
		}
	}

	CreatedAssets.Reset();
	ObjectSnapshots.Reset();
	MetadataSnapshots.Reset();
	bFinished = true;
}

void FFoliageBakerAssetTransaction::Rollback()
{
	if (bFinished)
	{
		return;
	}

	TArray<TStrongObjectPtr<UObject>> TouchedAssets = CreatedAssets;
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		TouchedAssets.Add(Snapshot.Original);
	}
	FinishCompilationForAssets(TouchedAssets);

	for (int32 SnapshotIndex = ObjectSnapshots.Num() - 1; SnapshotIndex >= 0; --SnapshotIndex)
	{
		const FObjectSnapshot& Snapshot = ObjectSnapshots[SnapshotIndex];
		if (!IsValid(Snapshot.Original.Get()) || !Snapshot.Backup.IsValid())
		{
			continue;
		}

		Snapshot.Original->PreEditChange(nullptr);
		if (RestoreSnapshotProperties(
				*Snapshot.Backup,
				*Snapshot.Original))
		{
			Snapshot.Original->ClearFlags(ManagedAssetFlags);
			Snapshot.Original->SetFlags(
				Snapshot.ObjectFlags & ManagedAssetFlags);
		}
		Snapshot.Original->PostEditChange();
	}

	for (int32 SnapshotIndex = MetadataSnapshots.Num() - 1; SnapshotIndex >= 0; --SnapshotIndex)
	{
		const FMetadataSnapshot& Snapshot = MetadataSnapshots[SnapshotIndex];
		if (!IsValid(Snapshot.Asset.Get()))
		{
			continue;
		}
		FMetaData& MetaData = Snapshot.Asset->GetPackage()->GetMetaData();
		if (Snapshot.bHadValue)
		{
			MetaData.SetValue(
				Snapshot.Asset.Get(),
				Snapshot.Key,
				*Snapshot.Value);
		}
		else
		{
			MetaData.RemoveValue(Snapshot.Asset.Get(), Snapshot.Key);
		}
	}

	FinishCompilationForAssets(TouchedAssets);
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		if (IsValid(Snapshot.Original.Get()))
		{
			Snapshot.Original->GetOutermost()->SetDirtyFlag(Snapshot.bPackageWasDirty);
		}
	}

	TArray<UObject*> AssetsToDelete;
	for (const TStrongObjectPtr<UObject>& Asset : CreatedAssets)
	{
		if (IsValid(Asset.Get()))
		{
			AssetsToDelete.Add(Asset.Get());
		}
	}
	CreatedAssets.Reset();
	TouchedAssets.Reset();
	if (!AssetsToDelete.IsEmpty())
	{
		ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
	}

	ObjectSnapshots.Reset();
	MetadataSnapshots.Reset();
	bFinished = true;
}

FString FFoliageBakerGeneratedAssetPath::BuildAssetName(
	const int32 AssetNameVersion) const
{
	const FString VersionToken = AssetNameVersion > 0
		? FString::Printf(TEXT("%02d"), AssetNameVersion)
		: FString();
	return ObjectTools::SanitizeObjectName(
		AssetNameStem + VersionToken + AssetNameSuffix);
}

FString FFoliageBakerGeneratedAssetPath::BuildPackageName(
	const int32 AssetNameVersion) const
{
	return PackagePath / BuildAssetName(AssetNameVersion);
}

FString FFoliageBakerGeneratedAssetPath::BuildObjectPath(
	const int32 AssetNameVersion) const
{
	const FString AssetName = BuildAssetName(AssetNameVersion);
	return (PackagePath / AssetName) + TEXT(".") + AssetName;
}

bool FFoliageBakerAssetBuilder::BuildGeneratedAssetBasePath(
	const UStaticMesh& SourceStaticMesh,
	const FString& ConfiguredOutputFolder,
	const FString& AssetNamePrefix,
	const FString& AssetNameSuffix,
	FString& OutBasePackageName,
	FString& OutBaseAssetName,
	FString& OutError)
{
	return BuildGeneratedAssetBasePath(
		SourceStaticMesh,
		ConfiguredOutputFolder,
		FString(),
		AssetNamePrefix,
		AssetNameSuffix,
		OutBasePackageName,
		OutBaseAssetName,
		OutError);
}

bool FFoliageBakerAssetBuilder::BuildGeneratedAssetBasePath(
	const UStaticMesh& SourceStaticMesh,
	const FString& ConfiguredOutputFolder,
	const FString& OutputPackagePathOverride,
	const FString& AssetNamePrefix,
	const FString& AssetNameSuffix,
	FString& OutBasePackageName,
	FString& OutBaseAssetName,
	FString& OutError)
{
	FFoliageBakerGeneratedAssetPath AssetPath;
	if (!BuildGeneratedAssetPath(
			SourceStaticMesh,
			ConfiguredOutputFolder,
			OutputPackagePathOverride,
			AssetNamePrefix,
			AssetNameSuffix,
			AssetPath,
			OutError))
	{
		return false;
	}
	OutBasePackageName = AssetPath.BuildPackageName();
	OutBaseAssetName = AssetPath.BuildAssetName();
	return true;
}

bool FFoliageBakerAssetBuilder::BuildGeneratedAssetPath(
	const UStaticMesh& SourceStaticMesh,
	const FString& ConfiguredOutputFolder,
	const FString& OutputPackagePathOverride,
	const FString& AssetNamePrefix,
	const FString& AssetNameSuffix,
	FFoliageBakerGeneratedAssetPath& OutAssetPath,
	FString& OutError)
{
	OutError.Reset();
	OutAssetPath = {};
	const FString SourceFolderPath = FPackageName::GetLongPackagePath(SourceStaticMesh.GetOutermost()->GetName());
	FString ParentFolderPath = SourceFolderPath;
	int32 LastSeparatorIndex = INDEX_NONE;
	if (SourceFolderPath.FindLastChar(TEXT('/'), LastSeparatorIndex) && LastSeparatorIndex > 0)
	{
		ParentFolderPath = SourceFolderPath.Left(LastSeparatorIndex);
	}

	const FString NormalizedOutputPackagePathOverride =
		NormalizeLongPackageFolder(OutputPackagePathOverride);
	const FString RelativeOutputFolder = NormalizeRelativeOutputFolder(ConfiguredOutputFolder);
	const FString OutputFolderPath = !NormalizedOutputPackagePathOverride.IsEmpty()
		? NormalizedOutputPackagePathOverride
		: RelativeOutputFolder.IsEmpty()
			? ParentFolderPath
			: ParentFolderPath / RelativeOutputFolder;
	OutAssetPath.PackagePath = OutputFolderPath;
	OutAssetPath.AssetNameStem =
		AssetNamePrefix + GetGeneratedAssetSourceName(SourceStaticMesh);
	OutAssetPath.AssetNameSuffix = AssetNameSuffix;

	const FString BaseAssetName = OutAssetPath.BuildAssetName();
	const FString BasePackageName = OutAssetPath.BuildPackageName();
	FText InvalidNameReason;
	if (BaseAssetName.IsEmpty()
		|| !FName(*BaseAssetName).IsValidObjectName(InvalidNameReason)
		|| !FPackageName::IsValidLongPackageName(BasePackageName, false, &InvalidNameReason))
	{
		OutError = FString::Printf(
			TEXT("Invalid generated asset path '%s': %s"),
			*BasePackageName,
			*InvalidNameReason.ToString());
		OutAssetPath = {};
		return false;
	}
	return true;
}

bool FFoliageBakerAssetBuilder::BuildGeneratedStaticMeshAssetPath(
	const UStaticMesh& SourceStaticMesh,
	const FString& AssetNameSuffix,
	FFoliageBakerGeneratedAssetPath& OutAssetPath,
	FString& OutError)
{
	OutError.Reset();
	OutAssetPath = {};
	OutAssetPath.PackagePath = FPackageName::GetLongPackagePath(
		SourceStaticMesh.GetOutermost()->GetName());
	OutAssetPath.AssetNameStem = SourceStaticMesh.GetName();
	OutAssetPath.AssetNameSuffix = AssetNameSuffix;

	const FString BaseAssetName = OutAssetPath.BuildAssetName();
	const FString BasePackageName = OutAssetPath.BuildPackageName();
	FText InvalidNameReason;
	if (BaseAssetName.IsEmpty()
		|| !FName(*BaseAssetName).IsValidObjectName(InvalidNameReason)
		|| !FPackageName::IsValidLongPackageName(
			BasePackageName,
			false,
			&InvalidNameReason))
	{
		OutError = FString::Printf(
			TEXT("Invalid generated Static Mesh path '%s': %s"),
			*BasePackageName,
			*InvalidNameReason.ToString());
		OutAssetPath = {};
		return false;
	}
	return true;
}

FFoliageBakerGeneratedAssetOutputFolders
FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
	const UStaticMesh& SourceStaticMesh,
	const int32 LODIndex)
{
	FFoliageBakerGeneratedAssetOutputFolders Result;
	TArray<int32> UsedMaterialIndices;
	if (const FStaticMeshRenderData* RenderData = SourceStaticMesh.GetRenderData();
		RenderData && RenderData->LODResources.IsValidIndex(LODIndex))
	{
		for (const FStaticMeshSection& Section : RenderData->LODResources[LODIndex].Sections)
		{
			UsedMaterialIndices.AddUnique(Section.MaterialIndex);
		}
	}

	if (UsedMaterialIndices.IsEmpty())
	{
		if (const FMeshDescription* MeshDescription = SourceStaticMesh.GetMeshDescription(LODIndex);
			MeshDescription
			&& MeshDescription->PolygonGroupAttributes().HasAttribute(
				MeshAttribute::PolygonGroup::ImportedMaterialSlotName))
		{
			const TPolygonGroupAttributesConstRef<FName> MaterialSlotNames =
				FStaticMeshConstAttributes(*MeshDescription).GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID PolygonGroupID : MeshDescription->PolygonGroups().GetElementIDs())
			{
				const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
				int32 MaterialIndex = SourceStaticMesh.GetMaterialIndex(MaterialSlotName);
				if (MaterialIndex == INDEX_NONE)
				{
					MaterialIndex = SourceStaticMesh.GetMaterialIndexFromImportedMaterialSlotName(
						MaterialSlotName);
				}
				if (MaterialIndex != INDEX_NONE)
				{
					UsedMaterialIndices.AddUnique(MaterialIndex);
				}
			}
		}
	}

	TArray<UMaterialInterface*> UsedMaterials;
	const TArray<FStaticMaterial>& StaticMaterials = SourceStaticMesh.GetStaticMaterials();
	for (const int32 MaterialIndex : UsedMaterialIndices)
	{
		if (StaticMaterials.IsValidIndex(MaterialIndex)
			&& IsValid(StaticMaterials[MaterialIndex].MaterialInterface))
		{
			UsedMaterials.AddUnique(StaticMaterials[MaterialIndex].MaterialInterface);
		}
	}
	if (UsedMaterials.IsEmpty())
	{
		return Result;
	}

	const FString SourcePackagePath =
		FPackageName::GetLongPackagePath(SourceStaticMesh.GetOutermost()->GetName());
	const FString SourcePackageRoot = GetPackageRoot(SourcePackagePath);
	Result.MaterialPackagePath = SelectMaterialOutputFolder(UsedMaterials, SourcePackageRoot);
	Result.TexturePackagePath = SelectTextureOutputFolder(
		UsedMaterials,
		Result.MaterialPackagePath,
		SourcePackageRoot);
	return Result;
}

UTexture2D* FFoliageBakerAssetBuilder::CreatePlaneAtlasTextureAsset(
	const UStaticMesh& SourceStaticMesh,
	FFoliageBakerAssetTransaction& AssetTransaction,
	const FFoliageBakerPlaneAtlasTextureAssetParams& Params,
	const TArray<FColor>& Pixels,
	const int32 AtlasWidth,
	const int32 AtlasHeight,
	const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
	FString& OutError)
{
	FFoliageBakerTextureAssetParams TextureParams;
	TextureParams.OutputFolderName = Params.OutputFolderName;
	TextureParams.OutputPackagePathOverride = Params.OutputPackagePathOverride;
	TextureParams.AssetNamePrefix = Params.AssetNamePrefix;
	TextureParams.AssetNameSuffix = Params.AssetNameSuffix;
	TextureParams.Width = AtlasWidth;
	TextureParams.Height = AtlasHeight;
	TextureParams.CompressionSettings = Params.CompressionSettings;
	TextureParams.LODGroup = Params.LODGroup;
	TextureParams.bSRGB = Params.bSRGB;
	TextureParams.SemanticMaskMipCoverageThreshold =
		Params.SemanticMaskMipCoverageThreshold;
	TextureParams.MipBackgroundColor = Params.MipBackgroundColor;
	TextureParams.bFillMipPaddingAlpha = Params.bFillMipPaddingAlpha;
	TextureParams.MipMode =
		Params.LODGroup == TEXTUREGROUP_WorldNormalMap
			? EFoliageBakerTextureMipMode::NormalizeXYZNormal
			: EFoliageBakerTextureMipMode::Default;
	TextureParams.EmptyPixelsError = Params.EmptyPixelsError;
	TextureParams.ExistingAssetPolicy = Params.ExistingAssetPolicy;
	TextureParams.AssetNameVersion = Params.AssetNameVersion;

	TextureParams.MipTileRects.Reserve(PlaneInfos.Num() * 2);
	for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo :
		PlaneInfos)
	{
		TextureParams.MipTileRects.Add(FIntRect(
			PlaneInfo.AtlasPixelMin,
			PlaneInfo.AtlasPixelMin + PlaneInfo.AtlasTileSize));
		if (PlaneInfo.bHasBackFaceAtlas)
		{
			TextureParams.MipTileRects.Add(FIntRect(
				PlaneInfo.BackAtlasPixelMin,
				PlaneInfo.BackAtlasPixelMin + PlaneInfo.BackAtlasTileSize));
		}
	}

	return CreateTextureAsset(
		SourceStaticMesh,
		AssetTransaction,
		TextureParams,
		Pixels,
		OutError);
}

UTexture2D* FFoliageBakerAssetBuilder::CreateTextureAsset(
	const UStaticMesh& SourceStaticMesh,
	FFoliageBakerAssetTransaction& AssetTransaction,
	const FFoliageBakerTextureAssetParams& Params,
	const TArray<FColor>& Pixels,
	FString& OutError)
{
	OutError.Reset();
	if (Params.Width <= 0 || Params.Height <= 0
		|| Pixels.Num() != static_cast<int64>(Params.Width) * static_cast<int64>(Params.Height))
	{
		OutError = Params.EmptyPixelsError;
		return nullptr;
	}

	FFoliageBakerGeneratedAssetPath GeneratedAssetPath;
	if (!BuildGeneratedAssetPath(
		SourceStaticMesh,
		Params.OutputFolderName,
		Params.OutputPackagePathOverride,
		Params.AssetNamePrefix,
		Params.AssetNameSuffix,
		GeneratedAssetPath,
		OutError))
	{
		return nullptr;
	}

	FString PackageName;
	FString AssetName;
	if (!ResolvePlannedAssetPath(
			GeneratedAssetPath,
			Params.ExistingAssetPolicy,
			Params.AssetNameVersion,
			PackageName,
			AssetName,
			OutError))
	{
		return nullptr;
	}

	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	UTexture2D* Texture = nullptr;
	if (Params.ExistingAssetPolicy
		== EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		Texture = Cast<UTexture2D>(ExistingObject);
		if (ExistingObject && !Texture)
		{
			OutError = FString::Printf(
				TEXT("Cannot rebake %s because that object is not a Texture2D."),
				*ObjectPath);
			return nullptr;
		}
	}
	else if (ExistingObject)
	{
		OutError = FString::Printf(
			TEXT("Cannot create the new asset set because %s already exists. Please run the bake again."),
			*ObjectPath);
		return nullptr;
	}

	UPackage* Package =
		Texture ? Texture->GetOutermost() : CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(
			TEXT("Could not create package %s."),
			*PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	if (!Texture)
	{
		Texture = NewObject<UTexture2D>(
			Package,
			*AssetName,
			PersistentAssetFlags);
		if (!Texture)
		{
			OutError = FString::Printf(
				TEXT("Could not create Texture2D %s."),
				*AssetName);
			return nullptr;
		}
		AssetTransaction.Track(Texture);
	}
	else
	{
		if (!AssetTransaction.Snapshot(Texture, OutError))
		{
			return nullptr;
		}
		Texture->Modify();
	}

	Texture->PreEditChange(nullptr);
	const bool bUseTileIsolatedMips = !Params.MipTileRects.IsEmpty();
	if (bUseTileIsolatedMips)
	{
		if (!InitializeTileIsolatedTextureSource(*Texture, Params, Pixels, OutError))
		{
			return nullptr;
		}
	}
	else
	{
		Texture->Source.Init(Params.Width, Params.Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	}
	Texture->CompressionSettings = Params.CompressionSettings;
	Texture->LODGroup = Params.LODGroup;
	Texture->MipGenSettings = bUseTileIsolatedMips ? TMGS_LeaveExistingMips : TMGS_FromTextureGroup;
	Texture->SRGB = Params.bSRGB;
	Texture->bUseNewMipFilter = true;
	Texture->bDoScaleMipsForAlphaCoverage = false;
	Texture->AlphaCoverageThresholds = FVector4(0.0, 0.0, 0.0, 0.0);
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	return Texture;
}

UMaterialInstanceConstant* FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
	const UStaticMesh& SourceStaticMesh,
	FFoliageBakerAssetTransaction& AssetTransaction,
	const FFoliageBakerMaterialInstanceAssetParams& Params,
	UMaterialInstanceConstant* TemplateMaterialInstance,
	UTexture2D* BaseColorOpacityTexture,
	UTexture2D* NormalDepthTexture,
	UTexture2D* MixTexture,
	FString& OutError)
{
	OutError.Reset();
	if (!TemplateMaterialInstance)
	{
		OutError = Params.MissingTemplateError;
		return nullptr;
	}

	FFoliageBakerGeneratedAssetPath GeneratedAssetPath;
	if (!BuildGeneratedAssetPath(
		SourceStaticMesh,
		Params.OutputFolderName,
		Params.OutputPackagePathOverride,
		Params.AssetNamePrefix,
		Params.AssetNameSuffix,
		GeneratedAssetPath,
		OutError))
	{
		return nullptr;
	}

	FString PackageName;
	FString AssetName;
	if (!ResolvePlannedAssetPath(
			GeneratedAssetPath,
			Params.ExistingAssetPolicy,
			Params.AssetNameVersion,
			PackageName,
			AssetName,
			OutError))
	{
		return nullptr;
	}
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UObject* ExistingObject =
		StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	UMaterialInstanceConstant* MaterialInstance = nullptr;
	if (Params.ExistingAssetPolicy
		== EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		MaterialInstance = Cast<UMaterialInstanceConstant>(ExistingObject);
		if (ExistingObject && !MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Cannot rebake %s because that object is not a MaterialInstanceConstant."), *ObjectPath);
			return nullptr;
		}
	}
	else if (ExistingObject)
	{
		OutError = FString::Printf(
			TEXT("Cannot create the new asset set because %s already exists. Please run the bake again."),
			*ObjectPath);
		return nullptr;
	}
	if (MaterialInstance
		&& TemplateMaterialInstance->IsChildOf(MaterialInstance))
	{
		OutError = FString::Printf(
			TEXT("Cannot use %s as the parent template for %s because that would create a material-instance parent cycle."),
			*TemplateMaterialInstance->GetPathName(),
			*ObjectPath);
		return nullptr;
	}

	UPackage* Package = MaterialInstance ? MaterialInstance->GetOutermost() : CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package %s."), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	if (!MaterialInstance)
	{
		MaterialInstance = NewObject<UMaterialInstanceConstant>(
			Package,
			*AssetName,
			PersistentAssetFlags);
		if (!MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Could not create Material Instance Constant %s."), *ObjectPath);
			return nullptr;
		}
		AssetTransaction.Track(MaterialInstance);
		if (!EnsureMaterialInstanceEditorOnlyData(
				*MaterialInstance,
				OutError))
		{
			return nullptr;
		}
	}
	else
	{
		if (!EnsureMaterialInstanceEditorOnlyData(
				*MaterialInstance,
				OutError))
		{
			return nullptr;
		}
		if (!AssetTransaction.Snapshot(MaterialInstance, OutError))
		{
			return nullptr;
		}
		MaterialInstance->Modify();
	}
	MaterialInstance->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	MaterialInstance->ClearFlags(RF_Transient);
	const FOwnedMaterialParameterNames CurrentOwnedParameterNames =
		BuildOwnedMaterialParameterNames(Params);
	FOwnedMaterialParameterNames ParameterNamesToReplace =
		ReadOwnedMaterialParameterNames(*MaterialInstance);
	// Current names clean legacy assets; metadata also finds parameters renamed since the previous bake.
	ParameterNamesToReplace.Append(CurrentOwnedParameterNames);
	MaterialInstance->PreEditChange(nullptr);
	MaterialInstance->SetParentEditorOnly(TemplateMaterialInstance, false);
	RemoveOwnedMaterialParameterOverrides(
		*MaterialInstance,
		ParameterNamesToReplace);
	ReplaceOwnedStaticSwitchParameterOverrides(
		*MaterialInstance,
		ParameterNamesToReplace.StaticSwitch,
		Params.StaticSwitchParameterValues);
	if (Params.TwoSidedOverride.IsSet())
	{
		FMaterialInstanceBasePropertyOverrides Overrides = MaterialInstance->BasePropertyOverrides;
		Overrides.bOverride_TwoSided = true;
		Overrides.TwoSided = Params.TwoSidedOverride.GetValue();
		FMaterialInstanceParameterUpdateContext OverrideContext(MaterialInstance);
		OverrideContext.SetBasePropertyOverrides(Overrides);
	}
	if (BaseColorOpacityTexture)
	{
		MaterialInstance->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo(Params.BaseColorOpacityTextureParameterName),
			BaseColorOpacityTexture);
	}
	if (NormalDepthTexture)
	{
		MaterialInstance->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo(Params.NormalDepthTextureParameterName),
			NormalDepthTexture);
	}
	if (MixTexture)
	{
		MaterialInstance->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo(Params.MixTextureParameterName),
			MixTexture);
	}
	for (const FFoliageBakerMaterialInstanceAssetParams::FTextureParameterValue& TextureParameter
		: Params.AdditionalTextureParameterValues)
	{
		if (!TextureParameter.ParameterName.IsNone() && TextureParameter.Texture)
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(
				FMaterialParameterInfo(TextureParameter.ParameterName),
				TextureParameter.Texture);
		}
	}
	for (const UE::FoliageBaker::MaterialResolver::FMaterialScalarParameterValue& ScalarParameter
		: Params.ScalarParameterValues)
	{
		MaterialInstance->SetScalarParameterValueEditorOnly(
			FMaterialParameterInfo(ScalarParameter.ParameterName),
			ScalarParameter.Value);
	}
	for (const FFoliageBakerMaterialInstanceAssetParams::FVectorParameterValue& VectorParameter
		: Params.VectorParameterValues)
	{
		if (!VectorParameter.ParameterName.IsNone())
		{
			MaterialInstance->SetVectorParameterValueEditorOnly(
				FMaterialParameterInfo(VectorParameter.ParameterName),
				VectorParameter.Value);
		}
	}
	WriteOwnedMaterialParameterNames(
		*MaterialInstance,
		AssetTransaction,
		CurrentOwnedParameterNames);
	MaterialInstance->PostEditChange();
	MaterialInstance->MarkPackageDirty();
	return MaterialInstance;
}

UStaticMesh* FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
	const UStaticMesh& SourceStaticMesh,
	FFoliageBakerAssetTransaction& AssetTransaction,
	const FFoliageBakerStaticMeshAssetParams& Params,
	const FMeshDescription& MeshDescription,
	UMaterialInterface* ProxyMaterial,
	FString& OutError)
{
	OutError.Reset();
	FFoliageBakerGeneratedAssetPath GeneratedAssetPath;
	if (!BuildGeneratedStaticMeshAssetPath(
			SourceStaticMesh,
			Params.AssetNameSuffix,
			GeneratedAssetPath,
			OutError))
	{
		return nullptr;
	}

	FString PackageName;
	FString AssetName;
	if (!ResolvePlannedAssetPath(
			GeneratedAssetPath,
			Params.ExistingAssetPolicy,
			Params.AssetNameVersion,
			PackageName,
			AssetName,
			OutError))
	{
		return nullptr;
	}
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UObject* ExistingObject =
		StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	UStaticMesh* ProxyMesh = nullptr;
	if (Params.ExistingAssetPolicy
		== EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		ProxyMesh = Cast<UStaticMesh>(ExistingObject);
		if (ExistingObject && !ProxyMesh)
		{
			OutError = FString::Printf(TEXT("Cannot rebake %s because that object is not a StaticMesh."), *ObjectPath);
			return nullptr;
		}
	}
	else if (ExistingObject)
	{
		OutError = FString::Printf(
			TEXT("Cannot create the new asset set because %s already exists. Please run the bake again."),
			*ObjectPath);
		return nullptr;
	}

	UPackage* Package = ProxyMesh ? ProxyMesh->GetOutermost() : CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package %s."), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	if (!ProxyMesh)
	{
		ProxyMesh = NewObject<UStaticMesh>(
			Package,
			*AssetName,
			PersistentAssetFlags);
		if (!ProxyMesh)
		{
			OutError = FString::Printf(TEXT("Could not create StaticMesh %s."), *AssetName);
			return nullptr;
		}
		AssetTransaction.Track(ProxyMesh);
	}
	else
	{
		if (!AssetTransaction.Snapshot(ProxyMesh, OutError))
		{
			return nullptr;
		}
		ProxyMesh->Modify();
		ProxyMesh->ModifyAllMeshDescriptions(false);
		if (UBodySetup* ExistingBodySetup = ProxyMesh->GetBodySetup())
		{
			ExistingBodySetup->Modify(false);
		}
		ProxyMesh->PreEditChange(nullptr);
		ProxyMesh->GetStaticMaterials().Reset();
	}

	ProxyMesh->InitResources();
	ProxyMesh->SetLightingGuid();
	UMaterialInterface* Material = ResolveProxyMaterial(ProxyMaterial);
	const FName MaterialSlotName = Material->GetFName();
	EnsureProxyMaterialSlot(*ProxyMesh, Material, Params.MaterialSlotName);
	EnsureAdditionalMaterialSlots(*ProxyMesh, Params.AdditionalMaterialSlots);
	ProxyMesh->SetLightMapCoordinateIndex(0);
	ProxyMesh->SetLightMapResolution(64);
	ProxyMesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);

	FMeshDescription MeshDescriptionCopy = MeshDescription;
	PrepareMeshDescriptionMaterialSlotNames(
		MeshDescriptionCopy,
		MaterialSlotName,
		Params.MaterialSlotName,
		!Params.AdditionalMaterialSlots.IsEmpty());
	if (MeshDescriptionCopy.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		MeshDescriptionCopy.Compact(Remappings);
	}
	TArray<const FMeshDescription*> MeshDescriptions;
	MeshDescriptions.Add(&MeshDescriptionCopy);
	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = false;

	BuildParams.bFastBuild = false;
	BuildParams.bCommitMeshDescription = true;
	BuildParams.bMarkPackageDirty = true;
	if (!ProxyMesh->BuildFromMeshDescriptions(MeshDescriptions, BuildParams))
	{
		OutError = FString::Printf(TEXT("Could not build generated StaticMesh %s."), *AssetName);
		return nullptr;
	}

	if (UBodySetup* BodySetup = ProxyMesh->GetBodySetup())
	{
		BodySetup->Modify(false);
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->bNeverNeedsCookedCollisionData = true;
		BodySetup->RemoveSimpleCollision();
	}
	ConfigureProxySourceModel(
		*ProxyMesh,
		0,
		false,
		Params.bRecomputeNormals,
		Params.bRecomputeTangents,
		Params.BaseLODModel);
	KeepOnlyUVChannels(*ProxyMesh, 0, Params.DesiredUVChannelCount, true);
	ProxyMesh->SetNegativeBoundsExtension(FVector::ZeroVector);
	ProxyMesh->SetPositiveBoundsExtension(FVector::ZeroVector);
	ProxyMesh->PostEditChange();
	ProxyMesh->MarkPackageDirty();
	return ProxyMesh;
}

bool FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
	const UStaticMesh& SourceStaticMesh,
	const FFoliageBakerSourceLODAssetParams& Params,
	FString& OutError)
{
	int32 OutputLODIndex = INDEX_NONE;
	bool bReusingGeneratedLOD = false;
	return ResolveSourceLODTarget(SourceStaticMesh, Params, OutputLODIndex, bReusingGeneratedLOD, OutError);
}

bool FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
	UStaticMesh& SourceStaticMesh,
	FFoliageBakerAssetTransaction& AssetTransaction,
	const FFoliageBakerSourceLODAssetParams& Params,
	const FMeshDescription& MeshDescription,
	UMaterialInterface* ProxyMaterial,
	int32& OutLODIndex,
	FString& OutError)
{
	bool bReusingGeneratedLOD = false;
	if (!ResolveSourceLODTarget(SourceStaticMesh, Params, OutLODIndex, bReusingGeneratedLOD, OutError))
	{
		return false;
	}
	if (!AssetTransaction.Snapshot(&SourceStaticMesh, OutError))
	{
		OutLODIndex = INDEX_NONE;
		return false;
	}
	// Generated nonzero LODs must not redefine the source asset's bounds.
	TOptional<FBoxSphereBounds> PreservedBounds;
	if (OutLODIndex != 0)
	{
		PreservedBounds = SourceStaticMesh.GetExtendedBounds();
	}
	const bool bInsertingLOD =
		Params.OutputMode == EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD;
	const bool bReplacingLOD =
		Params.OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD;
	TSet<int32> ReplacedLODMaterialIndices;
	TSet<int32> OtherLODMaterialIndices;
	if (bReplacingLOD)
	{
		for (int32 LODIndex = 0;
			LODIndex < SourceStaticMesh.GetNumSourceModels();
			++LODIndex)
		{
			AddLODMaterialIndices(
				SourceStaticMesh,
				LODIndex,
				LODIndex == OutLODIndex
					? ReplacedLODMaterialIndices
					: OtherLODMaterialIndices);
		}
	}
	const bool bTrackGeneratedLOD =
		OutLODIndex > 0
		&& !Params.RebuildLODMetadataKey.IsNone();
	const TArray<FGeneratedLODMetadataValue> GeneratedLODMetadataToUpdate =
		bInsertingLOD || bReplacingLOD
		? FindGeneratedLODMetadataAtOrAfter(SourceStaticMesh, OutLODIndex)
		: TArray<FGeneratedLODMetadataValue>();
	if (bTrackGeneratedLOD)
	{
		AssetTransaction.SnapshotMetadata(&SourceStaticMesh, Params.RebuildLODMetadataKey);
	}
	for (const FGeneratedLODMetadataValue& MetadataValue : GeneratedLODMetadataToUpdate)
	{
		if (bInsertingLOD || MetadataValue.LODIndex == OutLODIndex)
		{
			AssetTransaction.SnapshotMetadata(&SourceStaticMesh, MetadataValue.Key);
		}
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor
		? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		: nullptr;
	const bool bStaticMeshWasEdited = AssetEditorSubsystem
		&& AssetEditorSubsystem->FindEditorForAsset(&SourceStaticMesh, false);
	if (bStaticMeshWasEdited)
	{
		AssetEditorSubsystem->CloseAllEditorsForAsset(&SourceStaticMesh);
	}

	auto ReopenSourceMeshEditor = [&]()
	{
		if (bStaticMeshWasEdited && AssetEditorSubsystem)
		{
			AssetEditorSubsystem->OpenEditorForAsset(&SourceStaticMesh);
		}
	};
	auto FailAndRollback = [&]()
	{
		OutLODIndex = INDEX_NONE;
		AssetTransaction.Rollback();
		ReopenSourceMeshEditor();
		return false;
	};

	SourceStaticMesh.Modify();
	SourceStaticMesh.ModifyAllMeshDescriptions(false);
	if (UBodySetup* BodySetup = SourceStaticMesh.GetBodySetup())
	{
		BodySetup->Modify(false);
	}
	SourceStaticMesh.PreEditChange(nullptr);

	if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD
		&& !bReusingGeneratedLOD)
	{
		SourceStaticMesh.AddSourceModel();
	}
	else if (bInsertingLOD)
	{
		InsertSourceModel(SourceStaticMesh, OutLODIndex);
	}
	if (!SourceStaticMesh.IsSourceModelValid(OutLODIndex))
	{
		OutError = FString::Printf(TEXT("Could not allocate source LOD %d on %s."), OutLODIndex, *SourceStaticMesh.GetName());
		return FailAndRollback();
	}

	UMaterialInterface* Material = ResolveProxyMaterial(ProxyMaterial);
	const FName MaterialSlotName = Material->GetFName();
	FMeshDescription MeshDescriptionCopy = MeshDescription;
	PrepareMeshDescriptionMaterialSlotNames(
		MeshDescriptionCopy,
		MaterialSlotName,
		Params.MaterialSlotName,
		!Params.AdditionalMaterialSlots.IsEmpty());
	if (MeshDescriptionCopy.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		MeshDescriptionCopy.Compact(Remappings);
	}
	if (!SourceStaticMesh.CreateMeshDescription(OutLODIndex, MoveTemp(MeshDescriptionCopy)))
	{
		OutError = FString::Printf(TEXT("Could not create MeshDescription for source LOD %d on %s."), OutLODIndex, *SourceStaticMesh.GetName());
		return FailAndRollback();
	}

	const int32 MaterialIndex = EnsureProxyMaterialSlot(SourceStaticMesh, Material, Params.MaterialSlotName);
	EnsureAdditionalMaterialSlots(SourceStaticMesh, Params.AdditionalMaterialSlots);
	ClearSectionInfoForLOD(SourceStaticMesh, OutLODIndex);

	UStaticMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = true;
	CommitParams.bUseHashAsGuid = false;
	SourceStaticMesh.CommitMeshDescription(OutLODIndex, CommitParams);

	const int32 AdjustedBaseLODModel = bInsertingLOD
		&& Params.BaseLODModel >= OutLODIndex
		? Params.BaseLODModel + 1
		: Params.BaseLODModel;
	ConfigureProxySourceModel(
		SourceStaticMesh,
		OutLODIndex,
		Params.OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD || bReusingGeneratedLOD,
		Params.bRecomputeNormals,
		Params.bRecomputeTangents,
		AdjustedBaseLODModel);
	if (bInsertingLOD
		&& SourceStaticMesh.IsSourceModelValid(OutLODIndex - 1)
		&& SourceStaticMesh.IsSourceModelValid(OutLODIndex + 1))
	{
		const float PreviousScreenSize = SourceStaticMesh.GetSourceModel(OutLODIndex - 1).ScreenSize.Default;
		const float NextScreenSize = SourceStaticMesh.GetSourceModel(OutLODIndex + 1).ScreenSize.Default;
		SourceStaticMesh.GetSourceModel(OutLODIndex).ScreenSize.Default =
			FMath::Clamp((PreviousScreenSize + NextScreenSize) * 0.5f, 0.01f, 0.99f);
	}
	KeepOnlyUVChannels(SourceStaticMesh, OutLODIndex, Params.DesiredUVChannelCount, false);

	if (const FMeshDescription* InstalledMeshDescription = SourceStaticMesh.GetMeshDescription(OutLODIndex))
	{
		ConfigureSectionMaterialsFromMeshDescription(
			SourceStaticMesh,
			OutLODIndex,
			*InstalledMeshDescription,
			MaterialIndex);
	}
	if (bReplacingLOD)
	{
		// Remove only slots owned exclusively by the old target LOD.
		RemoveReplacedLODExclusiveMaterialSlots(
			SourceStaticMesh,
			ReplacedLODMaterialIndices,
			OtherLODMaterialIndices);
	}
	SourceStaticMesh.SetImportVersion(EImportStaticMeshVersion::LastVersion);
	SourceStaticMesh.PostEditChange();
	if (PreservedBounds.IsSet())
	{
		SourceStaticMesh.SetExtendedBounds(PreservedBounds.GetValue());
	}
	SourceStaticMesh.MarkPackageDirty();

	if (bInsertingLOD)
	{
		FMetaData& Metadata = SourceStaticMesh.GetPackage()->GetMetaData();
		for (const FGeneratedLODMetadataValue& MetadataValue : GeneratedLODMetadataToUpdate)
		{
			Metadata.SetValue(
				&SourceStaticMesh,
				MetadataValue.Key,
				*LexToString(MetadataValue.LODIndex + 1));
		}
	}
	else if (bReplacingLOD)
	{
		// Clear previous feature ownership before assigning the replacement.
		FMetaData& Metadata = SourceStaticMesh.GetPackage()->GetMetaData();
		for (const FGeneratedLODMetadataValue& MetadataValue : GeneratedLODMetadataToUpdate)
		{
			if (MetadataValue.LODIndex == OutLODIndex)
			{
				Metadata.RemoveValue(&SourceStaticMesh, MetadataValue.Key);
			}
		}
	}
	if (bTrackGeneratedLOD)
	{
		SourceStaticMesh.GetPackage()->GetMetaData().SetValue(
			&SourceStaticMesh,
			Params.RebuildLODMetadataKey,
			*LexToString(OutLODIndex));
	}

	ReopenSourceMeshEditor();
	return true;
}
