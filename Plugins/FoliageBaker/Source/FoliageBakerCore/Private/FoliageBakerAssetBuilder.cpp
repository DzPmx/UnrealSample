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

	double ComputeAlphaCoverage(
		const FColor* Pixels,
		const int32 PixelCount,
		const uint8 Threshold,
		const float AlphaScale = 1.0f)
	{
		if (!Pixels || PixelCount <= 0)
		{
			return 0.0;
		}

		int32 CoveredPixelCount = 0;
		for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
		{
			if (static_cast<float>(Pixels[PixelIndex].A) * AlphaScale >= Threshold)
			{
				++CoveredPixelCount;
			}
		}
		return static_cast<double>(CoveredPixelCount) / static_cast<double>(PixelCount);
	}

	void ScaleAlphaToCoverage(
		FColor* Pixels,
		const int32 PixelCount,
		const uint8 Threshold,
		const double TargetCoverage)
	{
		if (!Pixels || PixelCount <= 0 || Threshold == 0 || TargetCoverage <= 0.0)
		{
			return;
		}

		float MinScale = 0.0f;
		float MaxScale = 4.0f;
		float AlphaScale = 1.0f;
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			const double Coverage = ComputeAlphaCoverage(Pixels, PixelCount, Threshold, AlphaScale);
			if (FMath::IsNearlyEqual(Coverage, TargetCoverage, 1.0 / FMath::Max(1, PixelCount)))
			{
				break;
			}
			if (Coverage < TargetCoverage)
			{
				MinScale = AlphaScale;
			}
			else
			{
				MaxScale = AlphaScale;
			}
			AlphaScale = 0.5f * (MinScale + MaxScale);
		}

		for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
		{
			Pixels[PixelIndex].A = static_cast<uint8>(FMath::Clamp(
				FMath::RoundToInt(static_cast<float>(Pixels[PixelIndex].A) * AlphaScale),
				0,
				255));
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
		const uint8 AlphaCoverageThreshold = Params.AlphaCoverageThreshold > 0.0f
			? static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Params.AlphaCoverageThreshold, 0.0f, 1.0f) * 255.0f), 1, 255))
			: 0;

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
			return FIntRect(
				FIntPoint(
					FMath::Clamp(TileRect.Min.X >> MipIndex, 0, MipWidth),
					FMath::Clamp(TileRect.Min.Y >> MipIndex, 0, MipHeight)),
				FIntPoint(
					FMath::Clamp(TileRect.Max.X >> MipIndex, 0, MipWidth),
					FMath::Clamp(TileRect.Max.Y >> MipIndex, 0, MipHeight)));
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
			const double TargetAlphaCoverage = AlphaCoverageThreshold > 0
				? ComputeAlphaCoverage(CurrentTilePixels, TileRect.Width() * TileRect.Height(), AlphaCoverageThreshold)
				: 0.0;

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
				if (AlphaCoverageThreshold > 0)
				{
					ScaleAlphaToCoverage(NextTilePixels, NextTilePixelCount, AlphaCoverageThreshold, TargetAlphaCoverage);
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
			const double TailAlphaCoverage = AlphaCoverageThreshold > 0
				? ComputeAlphaCoverage(
					reinterpret_cast<const FColor*>(CurrentAtlas.RawData.GetData()),
					LastIsolatedMipWidth * LastIsolatedMipHeight,
					AlphaCoverageThreshold)
				: 0.0;

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
				if (AlphaCoverageThreshold > 0)
				{
					ScaleAlphaToCoverage(
						NextAtlasPixels,
						NextAtlasPixelCount,
						AlphaCoverageThreshold,
						TailAlphaCoverage);
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
		else
		{
			OutError = TEXT("A source-mesh LOD output mode is required.");
			return false;
		}

		if (OutLODIndex == Params.SourceLODIndex)
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
	OutBaseAssetName = ObjectTools::SanitizeObjectName(AssetNamePrefix + SourceStaticMesh.GetName() + AssetNameSuffix);
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
	Texture->bDoScaleMipsForAlphaCoverage = !bUseTileIsolatedMips && Params.AlphaCoverageThreshold > 0.0f;
	Texture->AlphaCoverageThresholds = !bUseTileIsolatedMips && Params.AlphaCoverageThreshold > 0.0f
		? FVector4(0.0, 0.0, 0.0, FMath::Clamp(Params.AlphaCoverageThreshold, 0.01f, 0.99f))
		: FVector4(0.0, 0.0, 0.0, 0.0);
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
		MaterialInstance = DuplicateObject<UMaterialInstanceConstant>(TemplateMaterialInstance, Package, *AssetName);
		if (!MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Could not duplicate material instance %s."), *TemplateMaterialInstance->GetPathName());
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
	if (bReplacingExistingMaterialInstance)
	{
		UEngine::CopyPropertiesForUnrelatedObjects(TemplateMaterialInstance, MaterialInstance);
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
	if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD
		&& !Params.RebuildLODMetadataKey.IsNone())
	{
		AssetTransaction.SnapshotMetadata(&SourceStaticMesh, Params.RebuildLODMetadataKey);
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

	ConfigureProxySourceModel(
		SourceStaticMesh,
		OutLODIndex,
		Params.OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD || bReusingGeneratedLOD,
		Params.bRecomputeNormals,
		Params.bRecomputeTangents,
		Params.BaseLODModel);
	KeepOnlyUVChannels(SourceStaticMesh, OutLODIndex, Params.DesiredUVChannelCount, false);

	FMeshSectionInfo SectionInfo;
	SectionInfo.MaterialIndex = MaterialIndex;
	SourceStaticMesh.GetSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
	SourceStaticMesh.GetOriginalSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
	SourceStaticMesh.SetImportVersion(EImportStaticMeshVersion::LastVersion);
	SourceStaticMesh.PostEditChange();
	SourceStaticMesh.MarkPackageDirty();

	if (Params.OutputMode == EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD
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
