#include "FoliageBakerAssetBuilder.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/MetaData.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	constexpr EObjectFlags ManagedAssetFlags = RF_Public | RF_Standalone | RF_Transactional | RF_Transient;

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

	void ResolveAssetPathForPolicy(
		const FString& BasePackageName,
		const FString& BaseAssetName,
		const EFoliageBakerExistingAssetPolicy ExistingAssetPolicy,
		FString& OutPackageName,
		FString& OutAssetName)
	{
		if (ExistingAssetPolicy == EFoliageBakerExistingAssetPolicy::CreateUnique)
		{
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), OutPackageName, OutAssetName);
			return;
		}

		OutPackageName = BasePackageName;
		OutAssetName = BaseAssetName;
	}

	void FinishCompilationForAssets(const TArray<UObject*>& Assets)
	{
		TArray<UObject*> ValidAssets;
		ValidAssets.Reserve(Assets.Num());
		for (UObject* Asset : Assets)
		{
			if (IsValid(Asset))
			{
				ValidAssets.AddUnique(Asset);
			}
		}
		if (!ValidAssets.IsEmpty())
		{
			FAssetCompilingManager::Get().FinishCompilationForObjects(ValidAssets);
		}
	}

	void ResetMaterialInstanceOverridesToDefaults(UMaterialInstanceConstant& MaterialInstance)
	{
		const UMaterialInstanceConstant* Defaults = GetDefault<UMaterialInstanceConstant>();
		MaterialInstance.PhysMaterial = Defaults->PhysMaterial;
		MaterialInstance.PhysMaterialMask = Defaults->PhysMaterialMask;
		for (int32 MaterialIndex = 0; MaterialIndex < EPhysicalMaterialMaskColor::MAX; ++MaterialIndex)
		{
			MaterialInstance.PhysicalMaterialMap[MaterialIndex] = Defaults->PhysicalMaterialMap[MaterialIndex];
		}

		MaterialInstance.NaniteOverrideMaterial = Defaults->NaniteOverrideMaterial;
		MaterialInstance.SubsurfaceProfile = Defaults->SubsurfaceProfile;
		MaterialInstance.SubsurfaceProfiles = Defaults->SubsurfaceProfiles;
		MaterialInstance.SpecularProfiles = Defaults->SpecularProfiles;
		MaterialInstance.SpecularProfileOverride = Defaults->SpecularProfileOverride;
		MaterialInstance.NeuralProfile = Defaults->NeuralProfile;
		MaterialInstance.bOverrideSubsurfaceProfile = Defaults->bOverrideSubsurfaceProfile;
		MaterialInstance.bOverrideSpecularProfile = Defaults->bOverrideSpecularProfile;

		MaterialInstance.bOverrideBlendableLocation = Defaults->bOverrideBlendableLocation;
		MaterialInstance.bOverrideBlendablePriority = Defaults->bOverrideBlendablePriority;
		MaterialInstance.BlendableLocationOverride = Defaults->BlendableLocationOverride;
		MaterialInstance.BlendablePriorityOverride = Defaults->BlendablePriorityOverride;
		MaterialInstance.UserSceneTextureOverrides = Defaults->UserSceneTextureOverrides;

		MaterialInstance.SetOverrideCastShadowAsMasked(Defaults->GetOverrideCastShadowAsMasked());
		MaterialInstance.SetCastShadowAsMasked(Defaults->GetCastShadowAsMasked());
		MaterialInstance.SetOverrideEmissiveBoost(Defaults->GetOverrideEmissiveBoost());
		MaterialInstance.SetEmissiveBoost(Defaults->GetEmissiveBoost());
		MaterialInstance.SetOverrideDiffuseBoost(Defaults->GetOverrideDiffuseBoost());
		MaterialInstance.SetDiffuseBoost(Defaults->GetDiffuseBoost());
		MaterialInstance.SetOverrideExportResolutionScale(Defaults->GetOverrideExportResolutionScale());
		MaterialInstance.SetExportResolutionScale(Defaults->GetExportResolutionScale());
	}

	int32 EnsureProxyMaterialSlot(UStaticMesh& StaticMesh, UMaterialInterface* ProxyMaterial, const FName MaterialSlotName)
	{
		UMaterialInterface* Material = ProxyMaterial ? ProxyMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
		TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
		for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
		{
			FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
			if (StaticMaterial.MaterialSlotName == MaterialSlotName
				|| StaticMaterial.ImportedMaterialSlotName == MaterialSlotName)
			{
				StaticMaterial.MaterialInterface = Material;
				StaticMaterial.MaterialSlotName = MaterialSlotName;
				StaticMaterial.ImportedMaterialSlotName = MaterialSlotName;
				return MaterialIndex;
			}
		}

		return StaticMaterials.Add(FStaticMaterial(Material, MaterialSlotName, MaterialSlotName));
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

	void ClearSectionInfoForLOD(UStaticMesh& StaticMesh, const int32 LODIndex)
	{
		int32 SectionCount = StaticMesh.GetSectionInfoMap().GetSectionNumber(LODIndex);
		for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			StaticMesh.GetSectionInfoMap().Remove(LODIndex, SectionIndex);
		}

		SectionCount = StaticMesh.GetOriginalSectionInfoMap().GetSectionNumber(LODIndex);
		for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			StaticMesh.GetOriginalSectionInfoMap().Remove(LODIndex, SectionIndex);
		}
	}

	void RemoveSectionInfoForLOD(FMeshSectionInfoMap& SectionInfoMap, const int32 LODIndex)
	{
		const int32 SectionCount = SectionInfoMap.GetSectionNumber(LODIndex);
		for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			SectionInfoMap.Remove(LODIndex, SectionIndex);
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

	TArray<FGeneratedLODMetadataValue> FindGeneratedLODMetadataToShift(
		const UStaticMesh& StaticMesh,
		const int32 InsertLODIndex)
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
				&& ExistingLODIndex >= InsertLODIndex
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
		Texture.Source.Init2DWithMipChain(Params.Width, Params.Height, TSF_BGRA8);
		const int32 NumMips = Texture.Source.GetNumMips();
		if (NumMips <= 0)
		{
			OutError = TEXT("Could not allocate the texture source mip chain.");
			return false;
		}

		TArray<FColor*> MipData;
		MipData.Reserve(NumMips);
		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			FColor* LockedMip = reinterpret_cast<FColor*>(Texture.Source.LockMip(MipIndex));
			if (!LockedMip)
			{
				for (int32 LockedMipIndex = MipData.Num() - 1; LockedMipIndex >= 0; --LockedMipIndex)
				{
					Texture.Source.UnlockMip(LockedMipIndex);
				}
				OutError = FString::Printf(TEXT("Could not lock generated texture source mip %d."), MipIndex);
				return false;
			}
			MipData.Add(LockedMip);

			const int32 MipWidth = FMath::Max(1, Params.Width >> MipIndex);
			const int32 MipHeight = FMath::Max(1, Params.Height >> MipIndex);
			for (int32 PixelIndex = 0; PixelIndex < MipWidth * MipHeight; ++PixelIndex)
			{
				LockedMip[PixelIndex] = Params.MipBackgroundColor;
			}
		}
		FMemory::Memcpy(MipData[0], Pixels.GetData(), static_cast<SIZE_T>(Pixels.Num()) * sizeof(FColor));

		const EGammaSpace GammaSpace = Params.bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		const float SemanticMaskMipCoverageThreshold =
			Params.SemanticMaskMipCoverageThreshold > 0.0f
				? FMath::Clamp(Params.SemanticMaskMipCoverageThreshold, 0.01f, 1.0f)
				: 0.0f;

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

		auto ProjectTileRectToMip = [&Params](const FIntRect& TileRect, const int32 MipIndex)
		{
			const int32 MipWidth = FMath::Max(1, Params.Width >> MipIndex);
			const int32 MipHeight = FMath::Max(1, Params.Height >> MipIndex);
			const int32 MipScale = 1 << MipIndex;
			return FIntRect(
				FIntPoint(
					FMath::Clamp(TileRect.Min.X >> MipIndex, 0, MipWidth),
					FMath::Clamp(TileRect.Min.Y >> MipIndex, 0, MipHeight)),
				FIntPoint(
					FMath::Clamp(FMath::DivideAndRoundUp(TileRect.Max.X, MipScale), 0, MipWidth),
					FMath::Clamp(FMath::DivideAndRoundUp(TileRect.Max.Y, MipScale), 0, MipHeight)));
		};
		auto RectsOverlap = [](const FIntRect& A, const FIntRect& B)
		{
			return A.Min.X < B.Max.X
				&& A.Max.X > B.Min.X
				&& A.Min.Y < B.Max.Y
				&& A.Max.Y > B.Min.Y;
		};

		int32 IsolatedMipCount = 1;
		for (int32 MipIndex = 1; MipIndex < NumMips && !TileRects.IsEmpty(); ++MipIndex)
		{
			TArray<FIntRect> MipTileRects;
			MipTileRects.Reserve(TileRects.Num());
			bool bMipKeepsTilesIsolated = true;
			for (const FIntRect& TileRect : TileRects)
			{
				const FIntRect MipTileRect = ProjectTileRectToMip(TileRect, MipIndex);
				if (MipTileRect.Width() <= 0 || MipTileRect.Height() <= 0)
				{
					bMipKeepsTilesIsolated = false;
					break;
				}
				for (const FIntRect& ExistingRect : MipTileRects)
				{
					if (RectsOverlap(MipTileRect, ExistingRect))
					{
						bMipKeepsTilesIsolated = false;
						break;
					}
				}
				if (!bMipKeepsTilesIsolated)
				{
					break;
				}
				MipTileRects.Add(MipTileRect);
			}
			if (!bMipKeepsTilesIsolated)
			{
				break;
			}
			IsolatedMipCount = MipIndex + 1;
		}

		for (const FIntRect& TileRect : TileRects)
		{

			FImage CurrentTile(
				TileRect.Width(),
				TileRect.Height(),
				1,
				ERawImageFormat::BGRA8,
				GammaSpace);
			FColor* CurrentTilePixels = reinterpret_cast<FColor*>(CurrentTile.RawData.GetData());
			for (int32 LocalY = 0; LocalY < TileRect.Height(); ++LocalY)
			{
				FMemory::Memcpy(
					CurrentTilePixels + LocalY * TileRect.Width(),
					Pixels.GetData() + (TileRect.Min.Y + LocalY) * Params.Width + TileRect.Min.X,
					static_cast<SIZE_T>(TileRect.Width()) * sizeof(FColor));
			}
			TArray<uint8> Mip0TileAlpha;
			if (SemanticMaskMipCoverageThreshold > 0.0f)
			{
				Mip0TileAlpha.SetNumUninitialized(TileRect.Width() * TileRect.Height());
				for (int32 PixelIndex = 0; PixelIndex < Mip0TileAlpha.Num(); ++PixelIndex)
				{
					Mip0TileAlpha[PixelIndex] = CurrentTilePixels[PixelIndex].A;
				}
			}

			for (int32 MipIndex = 1; MipIndex < IsolatedMipCount; ++MipIndex)
			{
				const int32 MipWidth = FMath::Max(1, Params.Width >> MipIndex);
				const FIntRect MipTileRect = ProjectTileRectToMip(TileRect, MipIndex);

				FImage NextTile(
					MipTileRect.Width(),
					MipTileRect.Height(),
					1,
					ERawImageFormat::BGRA8,
					GammaSpace);
				FImageCore::ResizeImage(CurrentTile, NextTile, FImageCore::EResizeImageFilter::Box);
				FColor* NextTilePixels = reinterpret_cast<FColor*>(NextTile.RawData.GetData());
				const int32 NextTilePixelCount = MipTileRect.Width() * MipTileRect.Height();
				if (Params.bNormalizeMipNormals)
				{
					NormalizeEncodedNormalPixels(NextTilePixels, NextTilePixelCount);
				}
				if (SemanticMaskMipCoverageThreshold > 0.0f)
				{
					GenerateSemanticMaskMipAlpha(
						Mip0TileAlpha.GetData(),
						TileRect.Width(),
						TileRect.Height(),
						NextTilePixels,
						MipTileRect.Width(),
						MipTileRect.Height(),
						SemanticMaskMipCoverageThreshold);
				}

				for (int32 LocalY = 0; LocalY < MipTileRect.Height(); ++LocalY)
				{
					FMemory::Memcpy(
						MipData[MipIndex] + (MipTileRect.Min.Y + LocalY) * MipWidth + MipTileRect.Min.X,
						NextTilePixels + LocalY * MipTileRect.Width(),
						static_cast<SIZE_T>(MipTileRect.Width()) * sizeof(FColor));
				}
				CurrentTile.Swap(NextTile);
			}
		}

		if (IsolatedMipCount < NumMips)
		{
			const int32 LastIsolatedMipIndex = IsolatedMipCount - 1;
			const int32 LastIsolatedMipWidth = FMath::Max(1, Params.Width >> LastIsolatedMipIndex);
			const int32 LastIsolatedMipHeight = FMath::Max(1, Params.Height >> LastIsolatedMipIndex);
			FImage CurrentAtlas(
				LastIsolatedMipWidth,
				LastIsolatedMipHeight,
				1,
				ERawImageFormat::BGRA8,
				GammaSpace);
			FMemory::Memcpy(
				CurrentAtlas.RawData.GetData(),
				MipData[LastIsolatedMipIndex],
				static_cast<SIZE_T>(LastIsolatedMipWidth * LastIsolatedMipHeight) * sizeof(FColor));
			TArray<uint8> Mip0AtlasAlpha;
			if (SemanticMaskMipCoverageThreshold > 0.0f)
			{
				Mip0AtlasAlpha.SetNumUninitialized(Pixels.Num());
				for (int32 PixelIndex = 0; PixelIndex < Pixels.Num(); ++PixelIndex)
				{
					Mip0AtlasAlpha[PixelIndex] = Pixels[PixelIndex].A;
				}
			}

			for (int32 MipIndex = IsolatedMipCount; MipIndex < NumMips; ++MipIndex)
			{
				const int32 MipWidth = FMath::Max(1, Params.Width >> MipIndex);
				const int32 MipHeight = FMath::Max(1, Params.Height >> MipIndex);
				FImage NextAtlas(
					MipWidth,
					MipHeight,
					1,
					ERawImageFormat::BGRA8,
					GammaSpace);
				FImageCore::ResizeImage(CurrentAtlas, NextAtlas, FImageCore::EResizeImageFilter::Box);
				FColor* NextAtlasPixels = reinterpret_cast<FColor*>(NextAtlas.RawData.GetData());
				const int32 NextAtlasPixelCount = MipWidth * MipHeight;
				if (Params.bNormalizeMipNormals)
				{
					NormalizeEncodedNormalPixels(NextAtlasPixels, NextAtlasPixelCount);
				}
				if (SemanticMaskMipCoverageThreshold > 0.0f)
				{
					GenerateSemanticMaskMipAlpha(
						Mip0AtlasAlpha.GetData(),
						Params.Width,
						Params.Height,
						NextAtlasPixels,
						MipWidth,
						MipHeight,
						SemanticMaskMipCoverageThreshold);
				}
				FMemory::Memcpy(
					MipData[MipIndex],
					NextAtlasPixels,
					static_cast<SIZE_T>(NextAtlasPixelCount) * sizeof(FColor));
				CurrentAtlas.Swap(NextAtlas);
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
					&& ExistingLODIndex > 0
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
			&& OutLODIndex == Params.SourceLODIndex)
		{
			OutError = FString::Printf(
				TEXT("Output LOD %d would overwrite the selected source LOD on %s. Choose a different output LOD so rebaking continues to use the original source geometry."),
				OutLODIndex,
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
		CreatedAssets.AddUnique(Asset);
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
	if (CreatedAssets.Contains(Asset)
		|| ObjectSnapshots.ContainsByPredicate([Asset](const FObjectSnapshot& Snapshot) { return Snapshot.Original == Asset; }))
	{
		return true;
	}

	TArray<UObject*> AssetToFinish;
	AssetToFinish.Add(Asset);
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
	Snapshot.Original = Asset;
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
			return Snapshot.Asset == Asset && Snapshot.Key == Key;
		}))
	{
		return;
	}

	FMetaData& MetaData = Asset->GetPackage()->GetMetaData();
	FMetadataSnapshot Snapshot;
	Snapshot.Asset = Asset;
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

	TArray<UObject*> TouchedAssets = CreatedAssets;
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		TouchedAssets.Add(Snapshot.Original);
	}
	FinishCompilationForAssets(TouchedAssets);

	for (UObject* Asset : CreatedAssets)
	{
		if (IsValid(Asset))
		{
			Asset->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Asset);
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

	TArray<UObject*> TouchedAssets = CreatedAssets;
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		TouchedAssets.Add(Snapshot.Original);
	}
	FinishCompilationForAssets(TouchedAssets);

	for (int32 SnapshotIndex = ObjectSnapshots.Num() - 1; SnapshotIndex >= 0; --SnapshotIndex)
	{
		const FObjectSnapshot& Snapshot = ObjectSnapshots[SnapshotIndex];
		if (!IsValid(Snapshot.Original) || !Snapshot.Backup.IsValid())
		{
			continue;
		}

		Snapshot.Original->PreEditChange(nullptr);
		UEngine::CopyPropertiesForUnrelatedObjects(Snapshot.Backup.Get(), Snapshot.Original);
		Snapshot.Original->ClearFlags(ManagedAssetFlags);
		Snapshot.Original->SetFlags(Snapshot.ObjectFlags & ManagedAssetFlags);
		Snapshot.Original->PostEditChange();
	}

	for (int32 SnapshotIndex = MetadataSnapshots.Num() - 1; SnapshotIndex >= 0; --SnapshotIndex)
	{
		const FMetadataSnapshot& Snapshot = MetadataSnapshots[SnapshotIndex];
		if (!IsValid(Snapshot.Asset))
		{
			continue;
		}
		FMetaData& MetaData = Snapshot.Asset->GetPackage()->GetMetaData();
		if (Snapshot.bHadValue)
		{
			MetaData.SetValue(Snapshot.Asset, Snapshot.Key, *Snapshot.Value);
		}
		else
		{
			MetaData.RemoveValue(Snapshot.Asset, Snapshot.Key);
		}
	}

	FinishCompilationForAssets(TouchedAssets);
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		if (IsValid(Snapshot.Original))
		{
			Snapshot.Original->GetOutermost()->SetDirtyFlag(Snapshot.bPackageWasDirty);
		}
	}

	TArray<UObject*> AssetsToDelete;
	for (UObject* Asset : CreatedAssets)
	{
		if (IsValid(Asset))
		{
			AssetsToDelete.Add(Asset);
		}
	}
	if (!AssetsToDelete.IsEmpty())
	{
		ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
	}

	CreatedAssets.Reset();
	ObjectSnapshots.Reset();
	MetadataSnapshots.Reset();
	bFinished = true;
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
	OutError.Reset();
	const FString SourceFolderPath = FPackageName::GetLongPackagePath(SourceStaticMesh.GetOutermost()->GetName());
	FString ParentFolderPath = SourceFolderPath;
	int32 LastSeparatorIndex = INDEX_NONE;
	if (SourceFolderPath.FindLastChar(TEXT('/'), LastSeparatorIndex) && LastSeparatorIndex > 0)
	{
		ParentFolderPath = SourceFolderPath.Left(LastSeparatorIndex);
	}

	const FString RelativeOutputFolder = NormalizeRelativeOutputFolder(ConfiguredOutputFolder);
	const FString OutputFolderPath = RelativeOutputFolder.IsEmpty()
		? ParentFolderPath
		: ParentFolderPath / RelativeOutputFolder;
	OutBaseAssetName = ObjectTools::SanitizeObjectName(
		AssetNamePrefix + GetGeneratedAssetSourceName(SourceStaticMesh) + AssetNameSuffix);
	OutBasePackageName = OutputFolderPath / OutBaseAssetName;

	FText InvalidNameReason;
	if (OutBaseAssetName.IsEmpty()
		|| !FName(*OutBaseAssetName).IsValidObjectName(InvalidNameReason)
		|| !FPackageName::IsValidLongPackageName(OutBasePackageName, false, &InvalidNameReason))
	{
		OutError = FString::Printf(
			TEXT("Invalid generated asset path '%s': %s"),
			*OutBasePackageName,
			*InvalidNameReason.ToString());
		return false;
	}
	return true;
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

	FString BasePackageName;
	FString BaseAssetName;
	if (!BuildGeneratedAssetBasePath(
		SourceStaticMesh,
		Params.OutputFolderName,
		Params.AssetNamePrefix,
		Params.AssetNameSuffix,
		BasePackageName,
		BaseAssetName,
		OutError))
	{
		return nullptr;
	}

	const FString ObjectPath = BasePackageName + TEXT(".") + BaseAssetName;
	UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	UTexture2D* Texture = Cast<UTexture2D>(ExistingObject);
	if (ExistingObject && !Texture)
	{
		OutError = FString::Printf(TEXT("Cannot rebake %s because that object is not a Texture2D."), *ObjectPath);
		return nullptr;
	}

	UPackage* Package = Texture ? Texture->GetOutermost() : CreatePackage(*BasePackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package %s."), *BasePackageName);
		return nullptr;
	}
	Package->FullyLoad();

	if (!Texture)
	{
		Texture = NewObject<UTexture2D>(Package, *BaseAssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Could not create Texture2D %s."), *BaseAssetName);
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

	FString BasePackageName;
	FString BaseAssetName;
	if (!BuildGeneratedAssetBasePath(
		SourceStaticMesh,
		Params.OutputFolderName,
		Params.AssetNamePrefix,
		Params.AssetNameSuffix,
		BasePackageName,
		BaseAssetName,
		OutError))
	{
		return nullptr;
	}

	FString PackageName;
	FString AssetName;
	ResolveAssetPathForPolicy(BasePackageName, BaseAssetName, Params.ExistingAssetPolicy, PackageName, AssetName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UMaterialInstanceConstant* MaterialInstance = nullptr;
	if (Params.ExistingAssetPolicy == EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		MaterialInstance = Cast<UMaterialInstanceConstant>(ExistingObject);
		if (ExistingObject && !MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Cannot rebake %s because that object is not a MaterialInstanceConstant."), *ObjectPath);
			return nullptr;
		}
	}
	if (MaterialInstance == TemplateMaterialInstance)
	{
		OutError = FString::Printf(
			TEXT("Cannot use %s as both the generated material instance and its parent."),
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

	const bool bReplacingExistingMaterialInstance = MaterialInstance != nullptr;
	if (!MaterialInstance)
	{
		MaterialInstance = NewObject<UMaterialInstanceConstant>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Could not create Material Instance Constant %s."), *ObjectPath);
			return nullptr;
		}
		AssetTransaction.Track(MaterialInstance);
	}
	else
	{
		if (!AssetTransaction.Snapshot(MaterialInstance, OutError))
		{
			return nullptr;
		}
		MaterialInstance->Modify();
	}
	MaterialInstance->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	MaterialInstance->ClearFlags(RF_Transient);
	MaterialInstance->PreEditChange(nullptr);
	MaterialInstance->SetParentEditorOnly(TemplateMaterialInstance, false);
	if (bReplacingExistingMaterialInstance)
	{
		FMaterialInstanceParameterUpdateContext ResetContext(
			MaterialInstance,
			EMaterialInstanceClearParameterFlag::All);
		ResetContext.SetBasePropertyOverrides(FMaterialInstanceBasePropertyOverrides());
		ResetMaterialInstanceOverridesToDefaults(*MaterialInstance);
	}
	if (Params.TwoSidedOverride.IsSet())
	{
		FMaterialInstanceBasePropertyOverrides Overrides;
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
	const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
	const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
	const FString BaseAssetName = ObjectTools::SanitizeObjectName(SourceStaticMesh.GetName() + Params.AssetNameSuffix);
	const FString BasePackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *BaseAssetName);

	FString PackageName;
	FString AssetName;
	ResolveAssetPathForPolicy(BasePackageName, BaseAssetName, Params.ExistingAssetPolicy, PackageName, AssetName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UStaticMesh* ProxyMesh = nullptr;
	if (Params.ExistingAssetPolicy == EFoliageBakerExistingAssetPolicy::ReuseOrCreate)
	{
		UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		ProxyMesh = Cast<UStaticMesh>(ExistingObject);
		if (ExistingObject && !ProxyMesh)
		{
			OutError = FString::Printf(TEXT("Cannot rebake %s because that object is not a StaticMesh."), *ObjectPath);
			return nullptr;
		}
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
		ProxyMesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
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
	EnsureProxyMaterialSlot(*ProxyMesh, ProxyMaterial, Params.MaterialSlotName);
	ProxyMesh->SetLightMapCoordinateIndex(0);
	ProxyMesh->SetLightMapResolution(64);
	ProxyMesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);

	TArray<const FMeshDescription*> MeshDescriptions;
	MeshDescriptions.Add(&MeshDescription);
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
	const bool bInsertingLOD =
		Params.OutputMode == EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD;
	const TArray<FGeneratedLODMetadataValue> GeneratedLODMetadataToShift = bInsertingLOD
		? FindGeneratedLODMetadataToShift(SourceStaticMesh, OutLODIndex)
		: TArray<FGeneratedLODMetadataValue>();
	if ((Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD || bInsertingLOD)
		&& !Params.RebuildLODMetadataKey.IsNone())
	{
		AssetTransaction.SnapshotMetadata(&SourceStaticMesh, Params.RebuildLODMetadataKey);
	}
	for (const FGeneratedLODMetadataValue& MetadataValue : GeneratedLODMetadataToShift)
	{
		AssetTransaction.SnapshotMetadata(&SourceStaticMesh, MetadataValue.Key);
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

	FMeshDescription MeshDescriptionCopy = MeshDescription;
	if (!SourceStaticMesh.CreateMeshDescription(OutLODIndex, MoveTemp(MeshDescriptionCopy)))
	{
		OutError = FString::Printf(TEXT("Could not create MeshDescription for source LOD %d on %s."), OutLODIndex, *SourceStaticMesh.GetName());
		return FailAndRollback();
	}

	const int32 MaterialIndex = EnsureProxyMaterialSlot(SourceStaticMesh, ProxyMaterial, Params.MaterialSlotName);
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

	FMeshSectionInfo SectionInfo;
	SectionInfo.MaterialIndex = MaterialIndex;
	SourceStaticMesh.GetSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
	SourceStaticMesh.GetOriginalSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
	SourceStaticMesh.SetImportVersion(EImportStaticMeshVersion::LastVersion);
	SourceStaticMesh.PostEditChange();
	SourceStaticMesh.MarkPackageDirty();

	if (bInsertingLOD)
	{
		FMetaData& Metadata = SourceStaticMesh.GetPackage()->GetMetaData();
		for (const FGeneratedLODMetadataValue& MetadataValue : GeneratedLODMetadataToShift)
		{
			Metadata.SetValue(
				&SourceStaticMesh,
				MetadataValue.Key,
				*LexToString(MetadataValue.LODIndex + 1));
		}
	}
	if ((Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD || bInsertingLOD)
		&& !Params.RebuildLODMetadataKey.IsNone())
	{
		SourceStaticMesh.GetPackage()->GetMetaData().SetValue(
			&SourceStaticMesh,
			Params.RebuildLODMetadataKey,
			*LexToString(OutLODIndex));
	}

	ReopenSourceMeshEditor();
	return true;
}
