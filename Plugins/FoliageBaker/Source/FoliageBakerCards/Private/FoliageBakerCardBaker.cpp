#include "FoliageBakerCardBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerCardAtlas.h"
#include "FoliageBakerCardGeometry.h"
#include "FoliageBakerL1Visibility.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerMultiBillboardLayout.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedAtlasBake.h"
#include "FoliageBakerProxyGeometry.h"
#include "FoliageBakerSourceMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerCards, Log, All);

namespace
{
	namespace CardAtlas = UE::FoliageBaker::Cards::Atlas;
	namespace CardGeometry = UE::FoliageBaker::Cards::Geometry;
	namespace MultiBillboardLayout = UE::FoliageBaker::Cards::MultiBillboardLayout;

	bool UsesDoublePlanesBillboard(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardMode::SingleBillboard
			&& Request.BillboardPlaneMode == EFoliageBakerBillboardMode::DoublePlanes;
	}

	bool UsesSeparateOneSidedCrossFaces(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardMode::CrossCards
			&& Request.CrossCardGeometryMode == EFoliageBakerCrossCardFaceMode::SeparateOneSidedFaces;
	}

	bool UsesMultiBillboard(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardMode::MultiBillboard;
	}

	int32 GetDesiredCardUVChannelCount(const FFoliageBakerCardBakeRequest& Request)
	{
		if (UsesDoublePlanesBillboard(Request) || UsesMultiBillboard(Request))
		{
			return 3;
		}
		return UsesSeparateOneSidedCrossFaces(Request) ? 1 : 2;
	}

	FString GetCardMeshAssetSuffix(
		const FFoliageBakerCardBakeRequest& Request)
	{
		if (Request.Mode == EFoliageBakerCardMode::CrossCards)
		{
			return TEXT("_CrossCards");
		}
		if (UsesMultiBillboard(Request))
		{
			return TEXT("_MultiBillboard");
		}
		return UsesDoublePlanesBillboard(Request)
			? TEXT("_DoubleBillboard")
			: TEXT("_Billboard");
	}

	bool BuildCardGeneratedAssetPlan(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& Settings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection,
		const FFoliageBakerGeneratedAssetOutputFolders& OutputFolders,
		TArray<FFoliageBakerGeneratedAssetPath>& OutGeneratedAssets,
		FString& OutError)
	{
		OutGeneratedAssets.Reset();
		auto AddGeneratedAsset = [
			&StaticMesh,
			&OutGeneratedAssets,
			&OutError](
			const FString& DisplayName,
			const FString& ConfiguredOutputFolder,
			const FString& OutputPackagePathOverride,
			const FString& Prefix,
			const FString& Suffix)
		{
			FFoliageBakerGeneratedAssetPath& AssetPath =
				OutGeneratedAssets.AddDefaulted_GetRef();
			if (!FFoliageBakerAssetBuilder::BuildGeneratedAssetPath(
					StaticMesh,
					ConfiguredOutputFolder,
					OutputPackagePathOverride,
					Prefix,
					Suffix,
					AssetPath,
					OutError))
			{
				OutGeneratedAssets.Pop();
				return false;
			}
			AssetPath.DisplayName = DisplayName;
			return true;
		};

		if (Settings.bBakeBaseColorOpacity
			&& !AddGeneratedAsset(
				TEXT("Base Color / Opacity"),
				Settings.TextureOutputFolderName,
				OutputFolders.TexturePackagePath,
				Settings.TextureNamePrefix,
				Settings.BaseColorOpacityTextureSuffix))
		{
			return false;
		}
		if (Settings.bBakeNormalDepth
			&& !AddGeneratedAsset(
				TEXT("Normal / Mask"),
				Settings.TextureOutputFolderName,
				OutputFolders.TexturePackagePath,
				Settings.TextureNamePrefix,
				Settings.NormalDepthTextureSuffix))
		{
			return false;
		}
		if (Settings.bBakeMix
			&& !AddGeneratedAsset(
				TEXT("Mix"),
				Settings.TextureOutputFolderName,
				OutputFolders.TexturePackagePath,
				Settings.TextureNamePrefix,
				Settings.MixTextureSuffix))
		{
			return false;
		}
		if (Settings.bBakeUpperHemisphereL1Visibility
			&& !AddGeneratedAsset(
				TEXT("Upper Hemisphere L1 Visibility"),
				Settings.TextureOutputFolderName,
				OutputFolders.TexturePackagePath,
				Settings.TextureNamePrefix,
				Settings.UpperHemisphereL1VisibilityTextureSuffix))
		{
			return false;
		}
		if (!AddGeneratedAsset(
				TEXT("Material Instance"),
				Settings.MaterialOutputFolderName,
				OutputFolders.MaterialPackagePath,
				Settings.MaterialInstanceNamePrefix,
				Settings.MaterialInstanceNameSuffix))
		{
			return false;
		}

		if (MeshOutputSelection.OutputMode
			== EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerGeneratedAssetPath& MeshPath =
				OutGeneratedAssets.AddDefaulted_GetRef();
			MeshPath.DisplayName = TEXT("Static Mesh");
			if (!FFoliageBakerAssetBuilder::BuildGeneratedStaticMeshAssetPath(
					StaticMesh,
					GetCardMeshAssetSuffix(Settings),
					MeshPath,
					OutError))
			{
				OutGeneratedAssets.Pop();
				return false;
			}
			MeshPath.DisplayName = TEXT("Static Mesh");
		}
		return true;
	}

	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const FFoliageBakerCardBakeRequest& Request)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		const int32 ClampedTextureResolution = FMath::Clamp(
			Request.TextureResolution,
			UE::FoliageBaker::TextureResolution::MinimumSupportedAtlasResolution,
			UE::FoliageBaker::TextureResolution::MaximumSupportedAtlasResolution);
		Settings.TextureResolutionMode = Request.TextureResolutionMode;
		Settings.TargetWorldTexelSizeCm = Request.TargetWorldTexelSizeCm;
		Settings.MinimumTextureAtlasResolution = Request.MinimumTextureAtlasResolution;
		Settings.TextureAtlasResolution = Request.bTrimUnusedAtlasSpace
			? ClampedTextureResolution
			: static_cast<int32>(
				1u << FMath::FloorLog2(static_cast<uint32>(ClampedTextureResolution)));
		Settings.DoubleSidedBakeMode = Request.Mode == EFoliageBakerCardMode::CrossCards
			? UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes
			: UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
		Settings.bEmitBackFaceGeometry = UsesSeparateOneSidedCrossFaces(Request);
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
		Settings.TrunkCardAtlasScale = 1.0;
		FBoxSphereBounds SourceBounds(ForceInitToZero);
		FFoliageBakerSourceMeshReader::ComputeBounds(
			SourceTriangles,
			SourceBounds);
		Settings.ErrorTolerance = FMath::Max(0.01, static_cast<double>(SourceBounds.SphereRadius) * 1.0e-6);
		Settings.bEnableAlphaAwareTileCrop = true;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
		return Settings;
	}

	struct FTrunkLeafClassification
	{
		int32 MatchedMaterialCount = 0;
		int32 TrunkTriangleCount = 0;
		int32 LeafTriangleCount = 0;
		int32 LeafComponentCount = 0;
		int32 GeneratedClusterCount = 0;
		int32 GeneratedBillboardCount = 0;
	};

	FTrunkLeafClassification ClassifyTrianglesForTrunkLeafMask(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<FString>& RawKeywords)
	{
		FTrunkLeafClassification Classification;
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(StaticMesh, RawKeywords);
		Classification.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;

		for (UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : SourceTriangles)
		{
			const bool bIsTrunk = MaterialMatches.IsMatch(Triangle.MaterialIndex);
			Triangle.bIsTrunk = bIsTrunk;
			if (bIsTrunk)
			{
				++Classification.TrunkTriangleCount;
			}
		}

		return Classification;
	}

	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;

	using FAtlasBakeStats = UE::FoliageBaker::ProjectedAtlasBake::FStats;

	UE::FoliageBaker::ProjectedAtlasBake::FPolicy BuildCardAtlasPolicy(
		const FAtlasOutputSelection& OutputSelection,
		const bool bConvertNormalsToCaptureFrame,
		const bool bCaptureSourceDepth)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FPolicy Policy;
		Policy.OutputSelection = OutputSelection;
		Policy.NormalAlphaMode =
			UE::FoliageBaker::ProjectedAtlasBake::ENormalAlphaMode::TrunkLeafClassification;
		Policy.bConvertNormalsToCaptureFrame = bConvertNormalsToCaptureFrame;
		Policy.bCaptureSourceTriangleIdAndDepth = bCaptureSourceDepth;
		Policy.DiagnosticName = TEXT("Card atlas");
		Policy.MaterialAlphaPolicyDetails =
			TEXT("\n    card BaseColor/source-triangle-id/normal/Mix=per-tile source masked shader with shared GPU depth; all materials compete in one depth buffer; no CPU material-property fallback");
		return Policy;
	}

	bool BakeCardAtlasOrthographic(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& SourceLODBounds,
		const FBoxSphereBounds& FixedFrameWPOBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats& ProxyStats,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const FAtlasOutputSelection& OutputSelection,
		const bool bConvertNormalsToCaptureFrame,
		const bool bCaptureSourceDepth,
		TArray<FColor>& OutPixels,
		TArray<FColor>& OutNormalPixels,
		TArray<FColor>& OutMixPixels,
		TArray<FColor>& OutSourceTriangleIdAndDepth,
		FAtlasBakeStats& OutStats,
		FString& OutError)
	{
		const UE::FoliageBaker::ProjectedAtlasBake::FInputs Inputs(
			SourceStaticMesh,
			SourceLODBounds,
			FixedFrameWPOBounds,
			Triangles,
			BakeMaterialOverrides,
			PlaneInfos,
			ProxyStats,
			Settings);
		const UE::FoliageBaker::ProjectedAtlasBake::FPolicy Policy =
			BuildCardAtlasPolicy(
				OutputSelection,
				bConvertNormalsToCaptureFrame,
				bCaptureSourceDepth);

		UE::FoliageBaker::ProjectedAtlasBake::FResult Result;
		if (!UE::FoliageBaker::ProjectedAtlasBake::Bake(
				Inputs,
				Policy,
				Result,
				OutError))
		{
			return false;
		}

		OutPixels = MoveTemp(Result.BaseColorOpacityPixels);
		OutNormalPixels = MoveTemp(Result.NormalPixels);
		OutMixPixels = MoveTemp(Result.MixPixels);
		OutSourceTriangleIdAndDepth =
			MoveTemp(Result.SourceTriangleIdAndDepthPixels);
		OutStats = MoveTemp(Result.Stats);
		return true;
	}

	FFoliageBakerPlaneAtlasTextureAssetParams
	MakeCardAtlasTextureAssetRequest(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FString& AssetNameSuffix,
		const FFoliageBakerExistingAssetDecision& AssetDecision)
	{
		FFoliageBakerPlaneAtlasTextureAssetParams Request;
		Request.OutputFolderName = EditorSettings.TextureOutputFolderName;
		Request.OutputPackagePathOverride = OutputPackagePathOverride;
		Request.AssetNamePrefix = EditorSettings.TextureNamePrefix;
		Request.AssetNameSuffix = AssetNameSuffix;
		Request.ExistingAssetPolicy = AssetDecision.ExistingAssetPolicy;
		Request.AssetNameVersion = AssetDecision.AssetNameVersion;
		Request.CompressionSettings = TC_BC7;
		return Request;
	}

	TStrongObjectPtr<UTexture2D> CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		FFoliageBakerPlaneAtlasTextureAssetParams Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.BaseColorOpacityTextureSuffix,
				AssetDecision);
		Request.LODGroup = TEXTUREGROUP_World;
		Request.bSRGB = true;
		Request.SemanticMaskMipCoverageThreshold =
			EditorSettings.bPreserveAlphaMaskValues
				? EditorSettings.MipMaskCoverageThreshold
				: 0.0f;
		return FFoliageBakerAssetBuilder::CreatePlaneAtlasTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats.Width,
			AtlasStats.Height,
			PlaneInfos,
			OutError);
	}

	TStrongObjectPtr<UTexture2D> CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		FFoliageBakerPlaneAtlasTextureAssetParams Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.NormalDepthTextureSuffix,
				AssetDecision);
		Request.MipBackgroundColor = FColor(128, 128, 255, 0);
		Request.LODGroup = TEXTUREGROUP_WorldNormalMap;
		Request.bSRGB = false;
		Request.SemanticMaskMipCoverageThreshold =
			EditorSettings.bPreserveAlphaMaskValues
				? EditorSettings.MipMaskCoverageThreshold
				: 0.0f;
		Request.EmptyPixelsError = TEXT("No normal atlas pixels were generated.");
		return FFoliageBakerAssetBuilder::CreatePlaneAtlasTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats.Width,
			AtlasStats.Height,
			PlaneInfos,
			OutError);
	}

	TStrongObjectPtr<UTexture2D> CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		FFoliageBakerPlaneAtlasTextureAssetParams Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.MixTextureSuffix,
				AssetDecision);
		Request.MipBackgroundColor = FColor(255, 128, 0, 0);
		Request.LODGroup = TEXTUREGROUP_WorldSpecular;
		Request.bSRGB = false;
		Request.bFillMipPaddingAlpha = true;
		Request.EmptyPixelsError = TEXT("No mix atlas pixels were generated.");
		return FFoliageBakerAssetBuilder::CreatePlaneAtlasTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats.Width,
			AtlasStats.Height,
			PlaneInfos,
			OutError);
	}

	TStrongObjectPtr<UTexture2D> CreateUpperHemisphereL1VisibilityTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		FFoliageBakerPlaneAtlasTextureAssetParams Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.UpperHemisphereL1VisibilityTextureSuffix,
				AssetDecision);
		Request.MipBackgroundColor = FColor(128, 128, 128, 255);
		Request.LODGroup = TEXTUREGROUP_WorldSpecular;
		Request.bSRGB = false;
		Request.bFillMipPaddingAlpha = true;
		Request.EmptyPixelsError =
			TEXT("No upper-hemisphere L1 visibility pixels were generated.");
		return FFoliageBakerAssetBuilder::CreatePlaneAtlasTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats.Width,
			AtlasStats.Height,
			PlaneInfos,
			OutError);
	}

	const TCHAR* GetMeshOutputModeText(const EFoliageBakerMeshAssetOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
		case EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD:
			return TEXT("inserted source mesh LOD");
		case EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD:
			return TEXT("replaced source mesh LOD");
		case EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset:
		default:
			return TEXT("separate mesh asset");
		}
	}

	struct FProxyPlaneCoverBuildData : FFoliageBakerSourceMeshData
	{
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> RetainedTrunkTriangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		FTrunkLeafClassification TrunkLeafClassification;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
		TArray<int32> MultiBillboardPlaneClusterIndices;
		TArray<FVector> MultiBillboardClusterCenters;
	};

	struct FProxyMeshBuildData : FFoliageBakerProxyGeometry
	{
		// The inherited geometry stays in capture space through atlas and L1 baking.
		TArray<int32> MultiBillboardPlaneGroupIndices;
		// Runtime-only UV payloads and retained trunk geometry live in this copy.
		FMeshDescription OutputMeshDescription;
		UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats OutputStats;
		TArray<FFoliageBakerMeshMaterialSlot> AdditionalMaterialSlots;
		int32 OriginalTrunkTriangleCount = 0;
		int32 ReducedTrunkTriangleCount = 0;
		int32 RetainedTrunkUVChannelCount = 0;
	};

	struct FProxyAtlasBuildData
	{
		FAtlasOutputSelection OutputSelection;
		TArray<FColor> AtlasPixels;
		TArray<FColor> NormalAtlasPixels;
		TArray<FColor> MixAtlasPixels;
		TArray<FColor> SourceTriangleIdAndDepthPixels;
		TArray<FColor> UpperHemisphereL1VisibilityPixels;
		FAtlasBakeStats AtlasStats;
		FAtlasBakeStats UpperHemisphereL1VisibilityStats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>
			UpperHemisphereL1VisibilityPlaneInfos;
	};

	struct FProxyMaterialRecipe
	{
		TArray<UE::FoliageBaker::MaterialResolver::FMaterialScalarParameterValue>
			ScalarParameterValues;
		TOptional<bool> TwoSidedOverride;
	};

	struct FProxyMaterialAssetData
	{
		TStrongObjectPtr<UTexture2D> AtlasTexture;
		TStrongObjectPtr<UTexture2D> NormalAtlasTexture;
		TStrongObjectPtr<UTexture2D> MixAtlasTexture;
		TStrongObjectPtr<UTexture2D> UpperHemisphereL1VisibilityTexture;
		TStrongObjectPtr<UMaterialInstanceConstant> Material;
	};

	bool ResolveMultiBillboardPlaneGroups(
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		TArray<int32>& OutPlaneGroupIndices,
		FString& OutError)
	{
		OutPlaneGroupIndices.Reset();
		OutPlaneGroupIndices.Reserve(MeshData.PlaneInfos.Num());
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo :
			MeshData.PlaneInfos)
		{
			if (!CoverData.MultiBillboardPlaneClusterIndices.IsValidIndex(
				PlaneInfo.SourcePlaneIndex))
			{
				OutError = FString::Printf(
					TEXT("MultiBillboard plane %d has no valid cluster mapping."),
					PlaneInfo.SourcePlaneIndex);
				return false;
			}
			const int32 ClusterIndex =
				CoverData.MultiBillboardPlaneClusterIndices[PlaneInfo.SourcePlaneIndex];
			if (!CoverData.MultiBillboardClusterCenters.IsValidIndex(ClusterIndex))
			{
				OutError = FString::Printf(
					TEXT("MultiBillboard plane %d references invalid cluster %d."),
					PlaneInfo.SourcePlaneIndex,
					ClusterIndex);
				return false;
			}
			OutPlaneGroupIndices.Add(ClusterIndex);
		}
		return true;
	}

	struct FProxyAssetBuildResult
	{
		bool bSucceeded = false;
		bool bCancelled = false;
		FString Report;
		TStrongObjectPtr<UStaticMesh> ProxyMesh;
		EFoliageBakerMeshAssetOutputMode MeshOutputMode = EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		TStrongObjectPtr<UTexture2D> AtlasTexture;
		TStrongObjectPtr<UTexture2D> NormalAtlasTexture;
		TStrongObjectPtr<UTexture2D> MixAtlasTexture;
		TStrongObjectPtr<UTexture2D> UpperHemisphereL1VisibilityTexture;
		TStrongObjectPtr<UMaterialInstanceConstant> Material;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh.GetName(), *Error);
		UE_LOG(LogFoliageBakerCards, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FProxyAssetBuildResult MakeProxyBuildCancelled(const UStaticMesh& StaticMesh)
	{
		FProxyAssetBuildResult Result;
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled: asset output was not confirmed and no generated assets were committed."),
			*StaticMesh.GetName());
		return Result;
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const FFoliageBakerCardBakeRequest& Settings)
	{
		FAtlasOutputSelection Selection;
		Selection.bBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Selection.bNormalMask = Settings.bBakeNormalDepth;
		Selection.bMix = Settings.bBakeMix;
		Selection.bMaterialScalarAverages = !Settings.bBakeMix;
		return Selection;
	}

	FVector ResolveSingleCaptureNormal(const EFoliageBakerSingleCaptureAxis Axis)
	{
		switch (Axis)
		{
		case EFoliageBakerSingleCaptureAxis::NegativeX: return FVector(-1.0, 0.0, 0.0);
		case EFoliageBakerSingleCaptureAxis::PositiveY: return FVector(0.0, 1.0, 0.0);
		case EFoliageBakerSingleCaptureAxis::NegativeY: return FVector(0.0, -1.0, 0.0);
		case EFoliageBakerSingleCaptureAxis::PositiveX:
		default: return FVector(1.0, 0.0, 0.0);
		}
	}

	FVector RotateHorizontalNormal90Degrees(const FVector& Normal)
	{
		return FVector(-Normal.Y, Normal.X, 0.0).GetSafeNormal();
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const FFoliageBakerCardBakeRequest& Request,
		const FFoliageBakerMeshOutputSelection& OutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = OutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = OutputSelection.ReplaceLODIndex;
		Params.RequestedInsertAfterLODIndex = OutputSelection.InsertAfterLODIndex;
		Params.SourceLODIndex = Request.SourceLODIndex;
		Params.DesiredUVChannelCount = GetDesiredCardUVChannelCount(Request);
		Params.RebuildLODMetadataKey = Request.Mode == EFoliageBakerCardMode::CrossCards
			? FName(TEXT("FoliageBaker.CrossCardsLOD"))
			: UsesMultiBillboard(Request)
				? FName(TEXT("FoliageBaker.MultiBillboardLOD"))
				: UsesDoublePlanesBillboard(Request)
					? FName(TEXT("FoliageBaker.DoublePlanesBillboardLOD"))
					: FName(TEXT("FoliageBaker.SingleBillboardLOD"));
		return Params;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		if (!FFoliageBakerSourceMeshReader::Read(
				StaticMesh,
				EditorSettings.SourceLODIndex,
				EditorSettings.bOverrideBakeStaticSwitch,
				EditorSettings.BakeStaticSwitchOverrides,
				OutData,
				OutError))
		{
			return false;
		}

		if (UsesMultiBillboard(EditorSettings))
		{
			const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult LeafMaterialMatches =
				UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(
					StaticMesh,
					EditorSettings.LeafMaterialKeywords);
			OutData.TrunkLeafClassification.MatchedMaterialCount =
				LeafMaterialMatches.MatchedMaterialCount;
			if (!LeafMaterialMatches.bEnabled)
			{
				OutError = TEXT("MultiBillboard requires at least one Leaf Material Keyword.");
				return false;
			}
			if (LeafMaterialMatches.MatchedMaterialCount == 0)
			{
				OutError = TEXT("No source material instance or parent material name matched the configured Leaf Material Keywords.");
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>
				LeafTriangles;
			TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>
				FixedFrameWPOLeafTriangles;
			LeafTriangles.Reserve(OutData.Triangles.Num());
			FixedFrameWPOLeafTriangles.Reserve(
				OutData.FixedFrameWPOTriangles.Num());
			if (EditorSettings.bIncludeReducedTrunk)
			{
				OutData.RetainedTrunkTriangles.Reserve(
					OutData.FixedFrameWPOTriangles.Num());
			}
			check(
				OutData.Triangles.Num()
				== OutData.FixedFrameWPOTriangles.Num());
			for (int32 TriangleIndex = 0;
				TriangleIndex < OutData.Triangles.Num();
				++TriangleIndex)
			{
				const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle =
					OutData.Triangles[TriangleIndex];
				if (LeafMaterialMatches.IsMatch(Triangle.MaterialIndex))
				{
					LeafTriangles.Add(Triangle);
					FixedFrameWPOLeafTriangles.Add(
						OutData.FixedFrameWPOTriangles[TriangleIndex]);
				}
				else
				{
					++OutData.TrunkLeafClassification.TrunkTriangleCount;
					if (EditorSettings.bIncludeReducedTrunk)
					{
						OutData.RetainedTrunkTriangles.Add(
							OutData.FixedFrameWPOTriangles[TriangleIndex]);
					}
				}
			}
			OutData.Triangles = MoveTemp(LeafTriangles);
			OutData.FixedFrameWPOTriangles =
				MoveTemp(FixedFrameWPOLeafTriangles);
			if (OutData.Triangles.IsEmpty())
			{
				OutError = TEXT("The matched leaf materials contain no bakeable triangles in the selected Source LOD.");
				return false;
			}
			OutData.TrunkLeafClassification.LeafTriangleCount = OutData.Triangles.Num();
		}
		else
		{
			OutData.TrunkLeafClassification = ClassifyTrianglesForTrunkLeafMask(
				StaticMesh,
				OutData.Triangles,
				EditorSettings.TrunkMaterialKeywords);
			check(
				OutData.Triangles.Num()
				== OutData.FixedFrameWPOTriangles.Num());
			for (int32 TriangleIndex = 0;
				TriangleIndex < OutData.Triangles.Num();
				++TriangleIndex)
			{
				OutData.FixedFrameWPOTriangles[TriangleIndex].bIsTrunk =
					OutData.Triangles[TriangleIndex].bIsTrunk;
			}
		}

		OutData.Settings = BuildSettingsForMesh(
			OutData.FixedFrameWPOTriangles,
			EditorSettings);
		OutData.ProxyResult.SourceTriangleCount = OutData.Triangles.Num();
		OutData.ProxyResult.CoveredTriangleCount = OutData.Triangles.Num();
		TArray<int32> AllTriangleIndices;
		AllTriangleIndices.Reserve(OutData.Triangles.Num());
		double SourceArea = 0.0;
		for (int32 TriangleIndex = 0; TriangleIndex < OutData.Triangles.Num(); ++TriangleIndex)
		{
			AllTriangleIndices.Add(TriangleIndex);
			SourceArea += OutData.Triangles[TriangleIndex].Area;
		}
		OutData.ProxyResult.SourceArea = SourceArea;
		OutData.ProxyResult.CoveredArea = SourceArea;

		const FVector PrimaryCaptureNormal =
			ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis);
		if (UsesMultiBillboard(EditorSettings))
		{
			const double PositionTolerance = FMath::Max(
				0.001,
				static_cast<double>(
					OutData.FixedFrameWPOBounds.SphereRadius)
					* 1.0e-6);
			const TArray<MultiBillboardLayout::FComponent> Components =
				MultiBillboardLayout::BuildConnectedLeafComponents(
					OutData.FixedFrameWPOTriangles,
					PositionTolerance);
			OutData.TrunkLeafClassification.LeafComponentCount = Components.Num();
			const TArray<MultiBillboardLayout::FCluster> Clusters =
				MultiBillboardLayout::ClusterLeafComponents(
					Components,
					EditorSettings.MultiBillboardClusterCount);
			if (Clusters.IsEmpty())
			{
				OutError = TEXT("MultiBillboard could not build any connected leaf clusters.");
				return false;
			}

			FVector AxisU = FVector::CrossProduct(FVector::UpVector, PrimaryCaptureNormal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}
			for (const MultiBillboardLayout::FCluster& Cluster : Clusters)
			{
				const TArray<MultiBillboardLayout::FLayer> Layers =
					MultiBillboardLayout::BuildClusterLayers(
						Components,
						Cluster,
						PrimaryCaptureNormal,
						EditorSettings.MultiBillboardsPerCluster);
				if (Layers.IsEmpty())
				{
					continue;
				}
				const int32 GeneratedClusterIndex =
					OutData.MultiBillboardClusterCenters.Add(Cluster.Center);
				for (const MultiBillboardLayout::FLayer& Layer : Layers)
				{
					UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane =
						OutData.ProxyResult.Planes.AddDefaulted_GetRef();
					Plane.Normal = PrimaryCaptureNormal;
					Plane.Rho = Layer.Rho;
					Plane.Score = Layer.Area;
					Plane.CoveredArea = Layer.Area;
					Plane.TriangleIndices = Layer.TriangleIndices;
					Plane.bIsTrunkCard = false;
					Plane.bUseFixedPlaneFrame = true;
					Plane.FixedAxisU = AxisU;
					Plane.FixedAxisV = FVector::UpVector;
					OutData.MultiBillboardPlaneClusterIndices.Add(GeneratedClusterIndex);
				}
			}
			OutData.TrunkLeafClassification.GeneratedClusterCount =
				OutData.MultiBillboardClusterCenters.Num();
			OutData.TrunkLeafClassification.GeneratedBillboardCount =
				OutData.ProxyResult.Planes.Num();
			if (OutData.ProxyResult.Planes.IsEmpty())
			{
				OutError = TEXT("MultiBillboard could not generate any non-empty depth layers.");
				return false;
			}
			return true;
		}

		const int32 PlaneCount = EditorSettings.Mode == EFoliageBakerCardMode::SingleBillboard
			? (UsesDoublePlanesBillboard(EditorSettings) ? 2 : 1)
			: FMath::Clamp(EditorSettings.CrossCardPlaneCount, 2, 5);
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneCount; ++PlaneIndex)
		{
			// Cross planes bake both sides, so their unique physical directions span 180 degrees.
			const double CrossCaptureAngle = static_cast<double>(PlaneIndex) * UE_DOUBLE_PI
				/ static_cast<double>(PlaneCount);
			const FVector Normal = EditorSettings.Mode == EFoliageBakerCardMode::SingleBillboard
				? PlaneIndex == 0
					? PrimaryCaptureNormal
					: RotateHorizontalNormal90Degrees(PrimaryCaptureNormal)
				: FVector(
					FMath::Cos(CrossCaptureAngle),
					FMath::Sin(CrossCaptureAngle),
					0.0);
			FVector AxisU = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane = OutData.ProxyResult.Planes.AddDefaulted_GetRef();
			Plane.Normal = Normal;
			Plane.Rho = 0.0;
			Plane.Score = SourceArea;
			Plane.CoveredArea = SourceArea;
			Plane.TriangleIndices = AllTriangleIndices;
			Plane.bIsTrunkCard = true;
			Plane.bUseFixedPlaneFrame = true;
			Plane.FixedAxisU = AxisU;
			Plane.FixedAxisV = FVector::UpVector;
		}
		return true;
	}

	bool BuildProxyMeshData(
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& OutData,
		FString& OutError)
	{
		return UE::FoliageBaker::PlaneCover::BuildPlaneProxyMeshDescription(
			CoverData.Triangles,
			CoverData.FixedFrameWPOTriangles,
			CoverData.ProxyResult,
			CoverData.Settings,
			OutData.MeshDescription,
			OutData.Stats,
			OutError,
			OutData.PlaneInfos);
	}

	bool AppendReducedMultiBillboardTrunk(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		UMaterialInterface& ProxyMaterial,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesMultiBillboard(EditorSettings)
			|| !EditorSettings.bIncludeReducedTrunk
			|| CoverData.RetainedTrunkTriangles.IsEmpty())
		{
			return true;
		}

		CardGeometry::FRetainedTrunkResult TrunkResult;
		if (!CardGeometry::AppendReducedTrunk(
			StaticMesh,
			CoverData.RetainedTrunkTriangles,
			EditorSettings.TrunkTrianglePercentage,
			ProxyMaterial,
			MeshData.OutputMeshDescription,
			TrunkResult,
			OutError))
		{
			return false;
		}
		MeshData.AdditionalMaterialSlots = MoveTemp(TrunkResult.MaterialSlots);
		MeshData.OriginalTrunkTriangleCount = TrunkResult.OriginalTriangleCount;
		MeshData.ReducedTrunkTriangleCount = TrunkResult.ReducedTriangleCount;
		MeshData.RetainedTrunkUVChannelCount = TrunkResult.UVChannelCount;
		return true;
	}

	bool BuildDoublePlanesOutputMesh(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& PlaneSettings,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesDoublePlanesBillboard(EditorSettings))
		{
			return true;
		}
		return CardGeometry::BuildDoublePlanesOutput(
			ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis),
			PlaneSettings,
			MeshData,
			MeshData.OutputMeshDescription,
			MeshData.OutputStats,
			OutError);
	}

	bool BuildMultiBillboardOutputMesh(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		const TArray<int32>& PlaneGroupIndices,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& PlaneSettings,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesMultiBillboard(EditorSettings))
		{
			return true;
		}
		return CardGeometry::BuildMultiBillboardOutput(
			PlaneGroupIndices,
			CoverData.MultiBillboardClusterCenters,
			PlaneSettings,
			MeshData,
			MeshData.OutputMeshDescription,
			MeshData.OutputStats,
			OutError);
	}

	bool PrepareProxyCaptureGeometry(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		MeshData.MultiBillboardPlaneGroupIndices.Reset();
		if (UsesMultiBillboard(EditorSettings))
		{
			if (!ResolveMultiBillboardPlaneGroups(
					CoverData,
					MeshData,
					MeshData.MultiBillboardPlaneGroupIndices,
					OutError))
			{
				return false;
			}
			if (!UE::FoliageBaker::PlaneCover::ApplyGroupedPlaneProxyBoundsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					MeshData.MultiBillboardPlaneGroupIndices,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError))
			{
				return false;
			}
		}

		if (UsesDoublePlanesBillboard(EditorSettings)
			&& !UE::FoliageBaker::PlaneCover::ApplySharedPlaneProxyBoundsAndRebuildMeshDescription(
				MeshData.PlaneInfos,
				CoverData.Settings,
				MeshData.MeshDescription,
				MeshData.Stats,
				OutError))
		{
			return false;
		}
		return true;
	}

	bool BakeProxyAtlasData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FProxyAtlasBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput()
			&& !EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			OutError = TEXT("No atlas outputs selected. Enable BaseColor/Opacity, Normal/TrunkLeafMask, Mix, or Upper Hemisphere L1 Visibility.");
			return false;
		}
		auto BakeFeatureAtlas = [
			&CoverData,
			&EditorSettings,
			&MeshData,
			&OutError,
			&StaticMesh](const FAtlasOutputSelection& OutputSelection,
			const bool bCaptureSourceDepth,
			TArray<FColor>& AtlasPixels,
			TArray<FColor>& NormalPixels,
			TArray<FColor>& MixPixels,
			TArray<FColor>& SourceTriangleIdAndDepthPixels,
			FAtlasBakeStats& AtlasStats) -> bool
		{
			return BakeCardAtlasOrthographic(
				StaticMesh,
				CoverData.SourceLODBounds,
				CoverData.FixedFrameWPOBounds,
				CoverData.Triangles,
				CoverData.BakeMaterialOverrides,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				OutputSelection,
				UsesDoublePlanesBillboard(EditorSettings),
				bCaptureSourceDepth,
				AtlasPixels,
				NormalPixels,
				MixPixels,
				SourceTriangleIdAndDepthPixels,
				AtlasStats,
				OutError);
		};

		int32 AlphaAwareCroppedPlaneCount = 0;
		const int32 EffectiveAlphaCropGuardPixels =
			CoverData.Settings.AlphaAwareTileCropGuardPixels;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			constexpr uint8 AlphaCropThreshold = 1;
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;
			CropOutputSelection.bNormalMask = false;
			CropOutputSelection.bMix = false;

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			if (CoverData.Settings.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize)
			{
				const UE::FoliageBaker::ProjectedAtlasBake::FInputs CropInputs(
					StaticMesh,
					CoverData.SourceLODBounds,
					CoverData.FixedFrameWPOBounds,
					CoverData.Triangles,
					CoverData.BakeMaterialOverrides,
					MeshData.PlaneInfos,
					MeshData.Stats,
					CoverData.Settings);
				const UE::FoliageBaker::ProjectedAtlasBake::FPolicy CropPolicy =
					BuildCardAtlasPolicy(
						CropOutputSelection,
						UsesDoublePlanesBillboard(EditorSettings),
						false);
				UE::FoliageBaker::ProjectedAtlasBake::FTargetDensityAlphaCropStats
					TargetDensityCropStats;
				if (!UE::FoliageBaker::ProjectedAtlasBake::BuildTargetDensityAlphaAwareTileCrops(
						CropInputs,
						CropPolicy,
						EffectiveAlphaCropGuardPixels,
						AlphaCropThreshold,
						TileCrops,
						TargetDensityCropStats,
						OutError))
				{
					return false;
				}
				AlphaAwareCroppedPlaneCount =
					TargetDensityCropStats.CroppedPlaneCount;
				if (TargetDensityCropStats.ResolutionLimitedPrepassPlaneCount > 0)
				{
					UE_LOG(
						LogFoliageBakerCards,
						Warning,
						TEXT("%s target-density alpha crop prepass limited %d plane(s) to %d pixels."),
						*StaticMesh.GetName(),
						TargetDensityCropStats.ResolutionLimitedPrepassPlaneCount,
						UE::FoliageBaker::TextureResolution::
							MaximumSupportedAtlasResolution);
				}
			}
			else
			{
				TArray<FColor> CropAtlasPixels;
				TArray<FColor> CropNormalPixels;
				TArray<FColor> CropMixPixels;
				TArray<FColor> CropSourceTriangleIdAndDepthPixels;
				FAtlasBakeStats CropStats;
				if (!BakeFeatureAtlas(
						CropOutputSelection,
						false,
						CropAtlasPixels,
						CropNormalPixels,
						CropMixPixels,
						CropSourceTriangleIdAndDepthPixels,
						CropStats))
				{
					return false;
				}
				AlphaAwareCroppedPlaneCount =
					UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
						CropAtlasPixels,
						CropStats.Width,
						CropStats.Height,
						MeshData.PlaneInfos,
						EffectiveAlphaCropGuardPixels,
						AlphaCropThreshold,
						TileCrops);
			}
			if (UsesDoublePlanesBillboard(EditorSettings))
			{
				AlphaAwareCroppedPlaneCount = CardAtlas::MergeDoublePlaneTileCrops(TileCrops);
			}
			else if (UsesMultiBillboard(EditorSettings))
			{
				AlphaAwareCroppedPlaneCount = CardAtlas::MergeGroupedTileCrops(
					TileCrops,
					MeshData.MultiBillboardPlaneGroupIndices);
			}

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::FoliageBaker::PlaneCover::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					TileCrops,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError,
					UsesDoublePlanesBillboard(EditorSettings)))
				{
					return false;
				}
			}
		}

		if (!BakeFeatureAtlas(
			OutData.OutputSelection,
			EditorSettings.bBakeUpperHemisphereL1Visibility,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.SourceTriangleIdAndDepthPixels,
			OutData.AtlasStats))
		{
			return false;
		}
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? EffectiveAlphaCropGuardPixels
			: 0;
		if (!CardAtlas::CropToUsedSpace(
				MeshData,
				OutData.AtlasPixels,
				OutData.NormalAtlasPixels,
				OutData.MixAtlasPixels,
				OutData.SourceTriangleIdAndDepthPixels,
				OutData.AtlasStats,
				EditorSettings.bTrimUnusedAtlasSpace
					? CardAtlas::EOuterCropMode::TightBlockAligned
					: CardAtlas::EOuterCropMode::PowerOfTwoUsedBounds,
				OutError))
		{
			return false;
		}
		if (EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			TArray<FColor> FullResolutionL1VisibilityPixels;
			if (!UE::FoliageBaker::L1Visibility::BakeUpperHemisphere(
					StaticMesh,
					CoverData.SourceLODBounds,
					CoverData.FixedFrameWPOBounds,
					CoverData.FixedFrameWPOTriangles,
					CoverData.Triangles,
					CoverData.BakeMaterialOverrides,
					MeshData.PlaneInfos,
					CoverData.Settings,
					OutData.SourceTriangleIdAndDepthPixels,
					OutData.AtlasStats.Width,
					OutData.AtlasStats.Height,
					EditorSettings.UpperHemisphereL1SampleCount,
					EditorSettings.UpperHemisphereL1ShadowMapResolution,
					FullResolutionL1VisibilityPixels,
					OutError))
			{
				return false;
			}
			if (!CardAtlas::ResizeTileIsolated(
					FullResolutionL1VisibilityPixels,
					OutData.AtlasStats,
					MeshData.PlaneInfos,
					EditorSettings.UpperHemisphereL1TextureResolution,
					FColor(128, 128, 128, 255),
					OutData.UpperHemisphereL1VisibilityPixels,
					OutData.UpperHemisphereL1VisibilityStats,
					OutData.UpperHemisphereL1VisibilityPlaneInfos,
					OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool FinalizeProxyRuntimeGeometry(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		MeshData.OutputMeshDescription = MeshData.MeshDescription;
		MeshData.OutputStats = MeshData.Stats;
		if (!BuildDoublePlanesOutputMesh(
				EditorSettings,
				CoverData.Settings,
				MeshData,
				OutError))
		{
			return false;
		}
		return BuildMultiBillboardOutputMesh(
			EditorSettings,
			CoverData,
			MeshData.MultiBillboardPlaneGroupIndices,
			CoverData.Settings,
			MeshData,
			OutError);
	}

	bool BuildProxyMaterialRecipe(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyAtlasBuildData& AtlasData,
		FProxyMaterialRecipe& OutRecipe,
		FString& OutError)
	{
		OutRecipe = FProxyMaterialRecipe();
		if (AtlasData.OutputSelection.bMaterialScalarAverages)
		{
			const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
				ParameterNames = {
					EditorSettings.LeafRoughnessParameterName,
					EditorSettings.LeafSpecularParameterName,
					EditorSettings.TrunkRoughnessParameterName,
					EditorSettings.TrunkSpecularParameterName,
				};
			if (!UE::FoliageBaker::MaterialResolver::ResolveTrunkLeafMaterialScalarParameters(
					AtlasData.AtlasStats.MaterialAverages,
					ParameterNames,
					OutRecipe.ScalarParameterValues,
					OutError))
			{
				return false;
			}
		}

		if (EditorSettings.Mode == EFoliageBakerCardMode::CrossCards)
		{
			OutRecipe.TwoSidedOverride = !UsesSeparateOneSidedCrossFaces(EditorSettings);
		}
		else if (UsesMultiBillboard(EditorSettings))
		{
			OutRecipe.TwoSidedOverride = true;
		}
		return true;
	}

	bool CreateProxyMaterialAssets(
		const UStaticMesh& StaticMesh,
		UMaterialInstanceConstant& TemplateMaterialInstance,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyAtlasBuildData& AtlasData,
		const FProxyMaterialRecipe& MaterialRecipe,
		const FFoliageBakerGeneratedAssetOutputFolders& OutputFolders,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		FProxyMaterialAssetData& OutAssets,
		FString& OutError)
	{
		if (AtlasData.OutputSelection.bBaseColorOpacity)
		{
			OutAssets.AtlasTexture = CreateAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				AssetDecision,
				AtlasData.AtlasPixels,
				AtlasData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutAssets.AtlasTexture)
			{
				return false;
			}
		}

		if (AtlasData.OutputSelection.bNormalMask)
		{
			OutAssets.NormalAtlasTexture = CreateNormalAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				AssetDecision,
				AtlasData.NormalAtlasPixels,
				AtlasData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutAssets.NormalAtlasTexture)
			{
				return false;
			}
		}

		if (AtlasData.OutputSelection.bMix)
		{
			OutAssets.MixAtlasTexture = CreateMixAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				AssetDecision,
				AtlasData.MixAtlasPixels,
				AtlasData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutAssets.MixAtlasTexture)
			{
				return false;
			}
		}

		FFoliageBakerMaterialInstanceAssetParams MaterialParams;
		MaterialParams.OutputFolderName = EditorSettings.MaterialOutputFolderName;
		MaterialParams.OutputPackagePathOverride = OutputFolders.MaterialPackagePath;
		MaterialParams.AssetNamePrefix = EditorSettings.MaterialInstanceNamePrefix;
		MaterialParams.AssetNameSuffix = EditorSettings.MaterialInstanceNameSuffix;
		MaterialParams.ExistingAssetPolicy =
			AssetDecision.ExistingAssetPolicy;
		MaterialParams.AssetNameVersion = AssetDecision.AssetNameVersion;
		MaterialParams.BaseColorOpacityTextureParameterName = EditorSettings.BaseColorOpacityTextureParameterName;
		MaterialParams.NormalDepthTextureParameterName = EditorSettings.NormalDepthTextureParameterName;
		MaterialParams.MixTextureParameterName = EditorSettings.MixTextureParameterName;
		MaterialParams.OwnedTextureParameterNames = {
			EditorSettings.UpperHemisphereL1VisibilityTextureParameterName
		};
		MaterialParams.OwnedScalarParameterNames = {
			EditorSettings.LeafRoughnessParameterName,
			EditorSettings.LeafSpecularParameterName,
			EditorSettings.TrunkRoughnessParameterName,
			EditorSettings.TrunkSpecularParameterName
		};
		MaterialParams.ScalarParameterValues = MaterialRecipe.ScalarParameterValues;
		if (EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			OutAssets.UpperHemisphereL1VisibilityTexture =
				CreateUpperHemisphereL1VisibilityTextureAsset(
					StaticMesh,
					AssetTransaction,
					EditorSettings,
					OutputFolders.TexturePackagePath,
					AssetDecision,
					AtlasData.UpperHemisphereL1VisibilityPixels,
					AtlasData.UpperHemisphereL1VisibilityStats,
					AtlasData.UpperHemisphereL1VisibilityPlaneInfos,
					OutError);
			if (!OutAssets.UpperHemisphereL1VisibilityTexture)
			{
				return false;
			}
		}
		if (OutAssets.UpperHemisphereL1VisibilityTexture)
		{
			FFoliageBakerMaterialInstanceAssetParams::FTextureParameterValue&
				L1VisibilityParameter =
					MaterialParams.AdditionalTextureParameterValues.AddDefaulted_GetRef();
			L1VisibilityParameter.ParameterName =
				EditorSettings.UpperHemisphereL1VisibilityTextureParameterName;
			L1VisibilityParameter.Texture =
				OutAssets.UpperHemisphereL1VisibilityTexture.Get();
		}
		MaterialParams.TwoSidedOverride = MaterialRecipe.TwoSidedOverride;
		OutAssets.Material = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
			StaticMesh,
			AssetTransaction,
			MaterialParams,
			TemplateMaterialInstance,
			OutAssets.AtlasTexture,
			OutAssets.NormalAtlasTexture,
			OutAssets.MixAtlasTexture,
			OutError);
		return OutAssets.Material != nullptr;
	}

	bool CreateProxyMeshAssetBundle(
		UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection,
		const FFoliageBakerExistingAssetDecision& AssetDecision,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyMaterialAssetData& MaterialAssets,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = MeshOutputSelection.OutputMode;
		const int32 AvailableUVChannelCount =
			FStaticMeshConstAttributes(MeshData.OutputMeshDescription)
				.GetVertexInstanceUVs()
				.GetNumChannels();
		const int32 OutputUVChannelCount = FMath::Clamp(
			FMath::Max(
				GetDesiredCardUVChannelCount(EditorSettings),
				MeshData.RetainedTrunkUVChannelCount),
			1,
			UE::FoliageBaker::PlaneCover::MaxSourceMeshUVChannels);
		if (AvailableUVChannelCount < OutputUVChannelCount)
		{
			OutError = FString::Printf(
				TEXT("Generated card mesh contains %d UV channels, but %d are required by its runtime payload and retained trunk."),
				AvailableUVChannelCount,
				OutputUVChannelCount);
			return false;
		}

		if (MeshOutputSelection.OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix =
				GetCardMeshAssetSuffix(EditorSettings);
			MeshParams.ExistingAssetPolicy =
				AssetDecision.ExistingAssetPolicy;
			MeshParams.AssetNameVersion = AssetDecision.AssetNameVersion;
			MeshParams.DesiredUVChannelCount = OutputUVChannelCount;
			MeshParams.AdditionalMaterialSlots = MeshData.AdditionalMaterialSlots;
			OutResult.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
				StaticMesh,
				AssetTransaction,
				MeshParams,
				MeshData.OutputMeshDescription,
				*MaterialAssets.Material,
				OutError);
			if (!OutResult.ProxyMesh)
			{
				return false;
			}
		}
		else
		{
			int32 InstalledLODIndex = INDEX_NONE;
			FFoliageBakerSourceLODAssetParams LODParams =
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection);
			LODParams.DesiredUVChannelCount = OutputUVChannelCount;
			LODParams.AdditionalMaterialSlots = MeshData.AdditionalMaterialSlots;
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				LODParams,
				MeshData.OutputMeshDescription,
				*MaterialAssets.Material,
				InstalledLODIndex,
				OutError))
			{
				return false;
			}

			OutResult.ProxyMesh.Reset(&StaticMesh);
			OutResult.SourceMeshLODIndex = InstalledLODIndex;
		}

		OutResult.AtlasTexture = MaterialAssets.AtlasTexture;
		OutResult.NormalAtlasTexture = MaterialAssets.NormalAtlasTexture;
		OutResult.MixAtlasTexture = MaterialAssets.MixAtlasTexture;
		OutResult.UpperHemisphereL1VisibilityTexture =
			MaterialAssets.UpperHemisphereL1VisibilityTexture;
		OutResult.Material = MaterialAssets.Material;
		return true;
	}

	FString BuildProxySuccessReport(
		const UStaticMesh& StaticMesh,
		const UMaterialInstanceConstant& MaterialTemplate,
		const FFoliageBakerCardBakeRequest& Request,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyAtlasBuildData& AtlasData,
		const FProxyMaterialAssetData& MaterialAssets,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = AtlasData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *AtlasData.AtlasStats.MaterialAlphaPolicyDetails);
		const FString CrossFaceDetails = Request.Mode != EFoliageBakerCardMode::CrossCards
			? FString()
			: FString::Printf(
				TEXT("\n  cross face mode: %s"),
				UsesSeparateOneSidedCrossFaces(Request)
					? TEXT("separate one-sided front/back quads, generated material Two Sided=false")
					: TEXT("one two-sided quad per direction, generated material Two Sided=true"));
		const TCHAR* AtlasUVDetails = UsesDoublePlanesBillboard(Request)
			? TEXT("UV0 stores each plane's baked tile; UV1.xy stores its local capture direction; UV2.x stores plane selector 0 or 1")
			: UsesMultiBillboard(Request)
				? TEXT("UV0 stores each depth layer's baked tile; UV1.xy stores the vertex U/V offset from the shared cluster center and UV2.x stores the signed layer-depth offset in centimeters")
				: Request.Mode == EFoliageBakerCardMode::SingleBillboard
					? TEXT("UV0 stores the single baked tile")
					: UsesSeparateOneSidedCrossFaces(Request)
						? TEXT("each physical face stores its own front/back tile in UV0; generated mesh keeps one UV channel")
						: TEXT("UV0 stores the front-side tile and UV1 stores the back-side tile");
		const TCHAR* WindingDetails = UsesDoublePlanesBillboard(Request)
			? TEXT("two overlapping parallel quads in the primary capture frame; dedicated material controls camera-facing rotation, Dither weights, and spacing")
			: UsesMultiBillboard(Request)
				? TEXT("multiple parallel two-sided quads per spatial leaf cluster; the material rotates every layer stack around its reconstructed shared cluster center")
				: UsesSeparateOneSidedCrossFaces(Request)
					? TEXT("opposed UE front-face orders with opposed source-facing normals")
					: TEXT("reversed UE front-face order with source-facing normals");
		const TCHAR* FeatureName = Request.Mode == EFoliageBakerCardMode::CrossCards
			? TEXT("Cross Cards")
			: UsesMultiBillboard(Request)
				? TEXT("MultiBillboard")
				: UsesDoublePlanesBillboard(Request)
					? TEXT("Double Planes Billboard")
					: TEXT("Single Plane Billboard");
		const TCHAR* CaptureDetails = Request.Mode == EFoliageBakerCardMode::CrossCards
			? TEXT("equally spaced over 180 degrees, front and back baked")
			: UsesMultiBillboard(Request)
				? TEXT("connected leaf components clustered in source-local 3D space, then split into parallel fixed-axis depth layers inside every cluster")
				: UsesDoublePlanesBillboard(Request)
					? TEXT("primary selected axis plus a second local horizontal axis rotated +90 degrees, one baked side per view")
					: TEXT("one selected axis, one baked side");

		const FString ClassificationDetails = UsesMultiBillboard(Request)
			? FString::Printf(
				TEXT("leaf selection: material/parent keyword rule, matched materials=%d, leaf triangles=%d, non-leaf triangles=%d, connected components=%d, generated clusters=%d, generated billboards=%d"),
				CoverData.TrunkLeafClassification.MatchedMaterialCount,
				CoverData.TrunkLeafClassification.LeafTriangleCount,
				CoverData.TrunkLeafClassification.TrunkTriangleCount,
				CoverData.TrunkLeafClassification.LeafComponentCount,
				CoverData.TrunkLeafClassification.GeneratedClusterCount,
				CoverData.TrunkLeafClassification.GeneratedBillboardCount)
			: FString::Printf(
				TEXT("trunk/leaf classification: shared material/parent keyword rule, matched materials=%d, trunk triangles=%d"),
				CoverData.TrunkLeafClassification.MatchedMaterialCount,
				CoverData.TrunkLeafClassification.TrunkTriangleCount);
		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm\n  feature: %s, capture=%s, selected-LOD projected bounds, per-plane alpha crop\n  %s%s"),
			*StaticMesh.GetName(),
			CoverData.SourceLODIndex,
			CoverData.FixedFrameWPOBounds.SphereRadius,
			FeatureName,
			CaptureDetails,
			*ClassificationDetails,
			*CrossFaceDetails);
		const FString BaseAtlasPath = MaterialAssets.AtlasTexture
			? MaterialAssets.AtlasTexture->GetPathName()
			: TEXT("disabled");
		const FString NormalAtlasPath = MaterialAssets.NormalAtlasTexture
			? MaterialAssets.NormalAtlasTexture->GetPathName()
			: TEXT("disabled");
		const FString MixAtlasPath = MaterialAssets.MixAtlasTexture
			? MaterialAssets.MixAtlasTexture->GetPathName()
			: TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/Opacity=%s, Normal/TrunkLeafMask=%s, Mix=%s"),
			*Request.BaseColorOpacityTextureParameterName.ToString(),
			*Request.NormalDepthTextureParameterName.ToString(),
			*Request.MixTextureParameterName.ToString());
		const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
			MaterialScalarParameterNames = {
				Request.LeafRoughnessParameterName,
				Request.LeafSpecularParameterName,
				Request.TrunkRoughnessParameterName,
				Request.TrunkSpecularParameterName,
			};
		const FString MaterialScalarDetails =
			UE::FoliageBaker::MaterialResolver::BuildTrunkLeafMaterialAveragesReport(
				!Request.bBakeMix,
				AtlasData.AtlasStats.MaterialAverages,
				MaterialScalarParameterNames);
		const FString ResolutionDetails =
			Request.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize
			? FString::Printf(
				TEXT("auto world texel size, target=%.4f cm/texel, actual range=%.4f-%.4f cm/texel%s"),
				Request.TargetWorldTexelSizeCm,
				MeshData.OutputStats.MinimumWorldTexelSizeCm,
				MeshData.OutputStats.MaximumWorldTexelSizeCm,
				MeshData.OutputStats.MaximumWorldTexelSizeCm
						> Request.TargetWorldTexelSizeCm * 1.001
					? TEXT(", maximum atlas reached")
					: TEXT(""))
			: FString::Printf(
				TEXT("manual atlas resolution, actual range=%.4f-%.4f cm/texel"),
				MeshData.OutputStats.MinimumWorldTexelSizeCm,
				MeshData.OutputStats.MaximumWorldTexelSizeCm);
		const TCHAR* AlphaCropDetails =
			Request.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize
			? TEXT("target-density per-plane prepass before packing; front/back and grouped-view bounds are conservatively merged when present")
			: TEXT("packed-atlas prepass before repacking; front/back and grouped-view bounds are conservatively merged when present");

		FString Report = FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  source WPO: material shader GPU Time/RealTime=0, evaluated vertices=%d, non-finite culled triangles=%d, maximum displacement=%.3f cm\n  source bake static switches: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-cropped planes=%d, crop guard=%d px, rasterized refs=%d, masked refs=%d, shooting=%s, resolve=%s\n  resolution: %s\n  alpha crop: %s\n  base/color opacity atlas: %s, RGB=BaseColor, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  normal/trunk-leaf atlas: %s, RGB=%s, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission\n  material scalar averages: %s\n  atlas UVs: %s\n  material instance: %s (parent template: %s; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision off, lightmap UV generation off, distance fields on\n  proxy winding: %s"),
			*TechniqueSummary,
			*AlphaPolicyDetails,
			*MeshOutputDetails,
			CoverData.WorldPositionOffsetStats.EvaluatedVertexCount,
			CoverData.WorldPositionOffsetStats.NonFiniteCulledTriangleCount,
			CoverData.WorldPositionOffsetStats.MaximumDisplacement,
			*CoverData.BakeMaterialOverrides.BuildReportDetails(),
			MeshData.OutputStats.PlaneCount,
			MeshData.OutputStats.QuadCount,
			MeshData.OutputStats.TriangleCount,
			AtlasData.AtlasStats.Width,
			AtlasData.AtlasStats.Height,
			AtlasData.AtlasStats.TileResolution,
			AtlasData.AtlasStats.PackedTileUtilizationPercent,
			AtlasData.AtlasStats.FrontTileCount,
			AtlasData.AtlasStats.BackTileCount,
			AtlasData.AtlasStats.PaintedPixels,
			AtlasData.AtlasStats.AlphaAwareCroppedPlanes,
			AtlasData.AtlasStats.AlphaAwareTileCropGuardPixels,
			AtlasData.AtlasStats.RasterizedTriangleReferences,
			AtlasData.AtlasStats.MaskedMaterialBakeReferences,
			Request.Mode == EFoliageBakerCardMode::CrossCards
				? TEXT("dedicated fixed-angle orthographic capture, front and back per plane, all selected-LOD triangles after GPU Time=0 WPO")
				: UsesMultiBillboard(Request)
					? TEXT("dedicated fixed-axis orthographic capture per clustered leaf group, matched leaf triangles after GPU Time=0 WPO")
					: UsesDoublePlanesBillboard(Request)
						? TEXT("two dedicated fixed-axis orthographic captures separated by 90 degrees, all selected-LOD triangles after GPU Time=0 WPO")
						: TEXT("dedicated fixed-axis orthographic capture, all selected-LOD triangles after GPU Time=0 WPO"),
			UsesDoublePlanesBillboard(Request)
				? TEXT("one shared per-tile masked RDG depth winner supplies BaseColor, source object normal, source triangle ID, and packed Mix; each view normal is re-expressed in its capture Facing/Right/Up frame before atlas storage")
				: TEXT("one shared per-tile masked RDG depth winner supplies BaseColor, object normal, source triangle ID, and packed Mix; no CPU material-property fallback"),
			*ResolutionDetails,
			AlphaCropDetails,
			*BaseAtlasPath,
			*NormalAtlasPath,
			UsesDoublePlanesBillboard(Request)
				? TEXT("per-view capture-frame normal")
				: TEXT("object/local-space normal"),
			*MixAtlasPath,
			*MaterialScalarDetails,
			AtlasUVDetails,
			*MaterialAssets.Material->GetPathName(),
			*MaterialTemplate.GetPathName(),
			*MaterialParameterDetails,
			MeshData.OutputStats.SourceShadingNormalTriangleCount,
			MeshData.OutputStats.SourceTriangleCount,
			MeshData.OutputStats.AveragePlaneToShadingNormalDot,
			MeshData.OutputStats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails,
			WindingDetails);
		if (MaterialAssets.UpperHemisphereL1VisibilityTexture)
		{
			Report += FString::Printf(
				TEXT("\n  upper-hemisphere L1 visibility atlas: %s, size=%dx%d, configured maximum dimension=%d, RGB=capture-frame signed Cnormal/CaxisU/CaxisV remapped to 0..1, A=C0, samples=%d, internal shadow resolution=%d, material parameter=%s"),
				*MaterialAssets.UpperHemisphereL1VisibilityTexture->GetPathName(),
				MaterialAssets.UpperHemisphereL1VisibilityTexture->GetSizeX(),
				MaterialAssets.UpperHemisphereL1VisibilityTexture->GetSizeY(),
				Request.UpperHemisphereL1TextureResolution,
				Request.UpperHemisphereL1SampleCount,
				Request.UpperHemisphereL1ShadowMapResolution,
				*Request.UpperHemisphereL1VisibilityTextureParameterName.ToString());
		}
		if (UsesMultiBillboard(Request))
		{
			Report += Request.bIncludeReducedTrunk
				? FString::Printf(
					TEXT("\n  retained trunk: enabled, source triangles=%d, reduced triangles=%d, requested percentage=%.1f%%, material slots=%d, UV channels=%d, source UVs/materials preserved"),
					MeshData.OriginalTrunkTriangleCount,
					MeshData.ReducedTrunkTriangleCount,
					Request.TrunkTrianglePercentage * 100.0f,
					MeshData.AdditionalMaterialSlots.Num(),
					MeshData.RetainedTrunkUVChannelCount)
				: TEXT("\n  retained trunk: disabled");
		}
		return Report;
	}

	enum class EPipelineStageResult : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	class FCardBakePipeline final
	{
	public:
		FCardBakePipeline(
			UStaticMesh& InStaticMesh,
			UMaterialInstanceConstant& InMaterialTemplate,
			const FFoliageBakerCardBakeRequest& InSettings,
			const FFoliageBakerMeshOutputSelector& InMeshOutputSelector)
			: StaticMesh(InStaticMesh)
			, MaterialTemplate(InMaterialTemplate)
			, Settings(InSettings)
			, MeshOutputSelector(InMeshOutputSelector)
		{
		}

		FProxyAssetBuildResult Run()
		{
			if (!BuildSourceStage())
			{
				return Fail();
			}
			if (!BuildGeometryStage())
			{
				return Fail();
			}

			const EPipelineStageResult OutputSelectionResult = ResolveOutputStage();
			if (OutputSelectionResult == EPipelineStageResult::Cancelled)
			{
				return MakeProxyBuildCancelled(StaticMesh);
			}
			if (OutputSelectionResult == EPipelineStageResult::Failed)
			{
				return Fail();
			}

			if (!PrepareCaptureGeometryStage())
			{
				return Fail();
			}
			if (!BakeAtlasStage())
			{
				return Fail();
			}
			if (!BuildMaterialRecipeStage())
			{
				return Fail();
			}
			if (!FinalizeRuntimeGeometryStage())
			{
				return Fail();
			}
			if (!WriteMaterialAssetsStage())
			{
				return Fail();
			}
			if (!WriteMeshOutputStage())
			{
				return Fail();
			}

			AssetTransaction.Commit();
			Result.bSucceeded = true;
			Result.Report = BuildProxySuccessReport(
				StaticMesh,
				MaterialTemplate,
				Settings,
				Source,
				Geometry,
				Atlas,
				MaterialAssets,
				Result);
			UE_LOG(LogFoliageBakerCards, Display, TEXT("\n%s"), *Result.Report);
			return Result;
		}

	private:
		bool BuildSourceStage()
		{
			return BuildProxyPlaneCoverData(StaticMesh, Settings, Source, Error);
		}

		bool BuildGeometryStage()
		{
			return BuildProxyMeshData(Source, Geometry, Error);
		}

		EPipelineStageResult ResolveOutputStage()
		{
			MeshOutputSelection =
				MeshOutputSelector.Execute(StaticMesh, Settings.SourceLODIndex);
			if (!MeshOutputSelection.IsSet())
			{
				return EPipelineStageResult::Cancelled;
			}

			if (MeshOutputSelection->OutputMode
					!= EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
				&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
					StaticMesh,
					BuildSourceLODAssetParams(Settings, *MeshOutputSelection),
					Error))
			{
				return EPipelineStageResult::Failed;
			}

			if (Settings.bPlaceGeneratedAssetsNearReplacedLODAssets
				&& MeshOutputSelection->OutputMode
					== EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
			{
				OutputFolders = FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
					StaticMesh,
					MeshOutputSelection->ReplaceLODIndex);
			}

			TArray<FFoliageBakerGeneratedAssetPath> GeneratedAssets;
			if (!BuildCardGeneratedAssetPlan(
					StaticMesh,
					Settings,
					*MeshOutputSelection,
					OutputFolders,
					GeneratedAssets,
					Error))
			{
				return EPipelineStageResult::Failed;
			}
			ExistingAssetDecision =
				FFoliageBakerExistingAssetDialog::OpenIfNeeded(
					GeneratedAssets,
					Error);
			if (!ExistingAssetDecision.IsSet())
			{
				return Error.IsEmpty()
					? EPipelineStageResult::Cancelled
					: EPipelineStageResult::Failed;
			}
			return EPipelineStageResult::Succeeded;
		}

		bool PrepareCaptureGeometryStage()
		{
			return PrepareProxyCaptureGeometry(Settings, Source, Geometry, Error);
		}

		bool BakeAtlasStage()
		{
			return BakeProxyAtlasData(
				StaticMesh,
				Settings,
				Source,
				Geometry,
				Atlas,
				Error);
		}

		bool BuildMaterialRecipeStage()
		{
			return BuildProxyMaterialRecipe(Settings, Atlas, MaterialRecipe, Error);
		}

		bool FinalizeRuntimeGeometryStage()
		{
			return FinalizeProxyRuntimeGeometry(Settings, Source, Geometry, Error);
		}

		bool WriteMaterialAssetsStage()
		{
			if (!ExistingAssetDecision.IsSet())
			{
				Error = TEXT(
					"The Card bake pipeline reached asset output without an existing-asset decision.");
				return false;
			}
			return CreateProxyMaterialAssets(
				StaticMesh,
				MaterialTemplate,
				Settings,
				AssetTransaction,
				Geometry,
				Atlas,
				MaterialRecipe,
				OutputFolders,
				*ExistingAssetDecision,
				MaterialAssets,
				Error);
		}

		bool WriteMeshOutputStage()
		{
			if (!MeshOutputSelection.IsSet()
				|| !ExistingAssetDecision.IsSet())
			{
				Error = TEXT(
					"The Card bake pipeline reached mesh output without a complete output decision.");
				return false;
			}
			if (!AppendReducedMultiBillboardTrunk(
					StaticMesh,
					Settings,
					Source,
					*MaterialAssets.Material,
					Geometry,
					Error))
			{
				return false;
			}

			return CreateProxyMeshAssetBundle(
				StaticMesh,
				Settings,
				*MeshOutputSelection,
				*ExistingAssetDecision,
				AssetTransaction,
				Geometry,
				MaterialAssets,
				Result,
				Error);
		}

		FProxyAssetBuildResult Fail() const
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		UStaticMesh& StaticMesh;
		UMaterialInstanceConstant& MaterialTemplate;
		const FFoliageBakerCardBakeRequest& Settings;
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector;
		FString Error;
		FProxyPlaneCoverBuildData Source;
		FProxyMeshBuildData Geometry;
		FProxyAtlasBuildData Atlas;
		FProxyMaterialRecipe MaterialRecipe;
		FProxyMaterialAssetData MaterialAssets;
		TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection;
		TOptional<FFoliageBakerExistingAssetDecision>
			ExistingAssetDecision;
		FFoliageBakerGeneratedAssetOutputFolders OutputFolders;
		FFoliageBakerAssetTransaction AssetTransaction;
		FProxyAssetBuildResult Result;
	};

	void AppendCardCreatedAssets(
		const FProxyAssetBuildResult& BuildResult,
		TArray<TStrongObjectPtr<UObject>>& OutCreatedAssets)
	{
		if (BuildResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset && BuildResult.ProxyMesh)
		{
			OutCreatedAssets.Add(BuildResult.ProxyMesh);
		}
		if (BuildResult.AtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.AtlasTexture);
		}
		if (BuildResult.NormalAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.NormalAtlasTexture);
		}
		if (BuildResult.MixAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.MixAtlasTexture);
		}
		if (BuildResult.UpperHemisphereL1VisibilityTexture)
		{
			OutCreatedAssets.Add(BuildResult.UpperHemisphereL1VisibilityTexture);
		}
		if (BuildResult.Material)
		{
			OutCreatedAssets.Add(BuildResult.Material);
		}
	}

	bool ValidateCardBakeRequest(
		const UStaticMesh& SourceStaticMesh,
		const FFoliageBakerCardBakeRequest& Request,
		const FFoliageBakerMeshOutputSelector& MeshOutputSelector,
		FString& OutFailureReport)
	{
		auto Fail = [&OutFailureReport, &SourceStaticMesh](const FString& Message)
		{
			OutFailureReport = FString::Printf(
				TEXT("%s\n  failed: %s"),
				*SourceStaticMesh.GetName(),
				*Message);
			return false;
		};
		if (Request.SourceLODIndex < 0 || Request.SourceLODIndex >= MAX_STATIC_MESH_LODS)
		{
			return Fail(FString::Printf(
				TEXT("source LOD index %d is outside the supported range 0-%d."),
				Request.SourceLODIndex,
				MAX_STATIC_MESH_LODS - 1));
		}
		if (!Request.bBakeBaseColorOpacity
			&& !Request.bBakeNormalDepth
			&& !Request.bBakeMix
			&& !Request.bBakeUpperHemisphereL1Visibility)
		{
			return Fail(TEXT("no texture output is enabled."));
		}
		if (!MeshOutputSelector.IsBound())
		{
			return Fail(TEXT("mesh output selector is not bound."));
		}
		if (Request.bBakeUpperHemisphereL1Visibility
			&& Request.Mode != EFoliageBakerCardMode::SingleBillboard)
		{
			return Fail(TEXT("Upper Hemisphere L1 Visibility is currently supported only by Billboard modes."));
		}

		TSet<FName> UsedTextureParameterNames;
		auto ValidateTextureParameterName = [&Fail, &UsedTextureParameterNames](const bool bEnabled,
			const FName ParameterName,
			const TCHAR* OutputLabel) -> bool
		{
			if (!bEnabled)
			{
				return true;
			}
			if (ParameterName.IsNone())
			{
				return Fail(FString::Printf(
					TEXT("%s output is enabled, but its Material texture parameter name is None."),
					OutputLabel));
			}
			if (UsedTextureParameterNames.Contains(ParameterName))
			{
				return Fail(FString::Printf(
					TEXT("Material texture parameter '%s' is assigned to more than one enabled output."),
					*ParameterName.ToString()));
			}
			UsedTextureParameterNames.Add(ParameterName);
			return true;
		};
		if (!ValidateTextureParameterName(
				Request.bBakeBaseColorOpacity,
				Request.BaseColorOpacityTextureParameterName,
				TEXT("BaseColor/Opacity"))
			|| !ValidateTextureParameterName(
				Request.bBakeNormalDepth,
				Request.NormalDepthTextureParameterName,
				TEXT("Normal/TrunkLeafMask"))
			|| !ValidateTextureParameterName(
				Request.bBakeMix,
				Request.MixTextureParameterName,
				TEXT("Mix"))
			|| !ValidateTextureParameterName(
				Request.bBakeUpperHemisphereL1Visibility,
				Request.UpperHemisphereL1VisibilityTextureParameterName,
				TEXT("Upper Hemisphere L1 Visibility")))
		{
			return false;
		}
		if (Request.TextureResolution
				< UE::FoliageBaker::TextureResolution::MinimumSupportedAtlasResolution
			|| Request.TextureResolution
				> UE::FoliageBaker::TextureResolution::MaximumSupportedAtlasResolution)
		{
			return Fail(TEXT("texture resolution must be between 64 and 4096."));
		}
		if (Request.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize
			&& (!FMath::IsFinite(Request.TargetWorldTexelSizeCm)
				|| Request.TargetWorldTexelSizeCm <= 0.0))
		{
			return Fail(TEXT("target world texel size must be greater than zero."));
		}
		if (Request.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize
			&& (Request.MinimumTextureAtlasResolution
					< UE::FoliageBaker::TextureResolution::MinimumSupportedAtlasResolution
				|| Request.MinimumTextureAtlasResolution
					> UE::FoliageBaker::TextureResolution::MaximumSupportedAtlasResolution))
		{
			return Fail(TEXT("minimum texture resolution must be between 64 and 4096."));
		}
		return true;
	}

	FFoliageBakerCardBakeRequest SanitizeCardBakeRequest(
		const FFoliageBakerCardBakeRequest& Request)
	{
		FFoliageBakerCardBakeRequest Result = Request;
		Result.CrossCardPlaneCount = FMath::Clamp(Request.CrossCardPlaneCount, 2, 5);
		Result.MultiBillboardClusterCount =
			FMath::Clamp(Request.MultiBillboardClusterCount, 1, 128);
		Result.MultiBillboardsPerCluster =
			FMath::Clamp(Request.MultiBillboardsPerCluster, 2, 8);
		Result.TrunkTrianglePercentage =
			FMath::Clamp(Request.TrunkTrianglePercentage, 0.05f, 1.0f);
		Result.TargetWorldTexelSizeCm =
			FMath::Max(Request.TargetWorldTexelSizeCm, 0.01);
		Result.MinimumTextureAtlasResolution =
			FMath::Clamp(
				Request.MinimumTextureAtlasResolution,
				UE::FoliageBaker::TextureResolution::MinimumSupportedAtlasResolution,
				UE::FoliageBaker::TextureResolution::MaximumSupportedAtlasResolution);
		Result.TextureResolution =
			FMath::Clamp(
				Request.TextureResolution,
				UE::FoliageBaker::TextureResolution::MinimumSupportedAtlasResolution,
				UE::FoliageBaker::TextureResolution::MaximumSupportedAtlasResolution);
		Result.AlphaCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
		Result.MipMaskCoverageThreshold =
			FMath::Clamp(Request.MipMaskCoverageThreshold, 0.01f, 1.0f);
		Result.UpperHemisphereL1TextureResolution =
			FMath::Clamp(Request.UpperHemisphereL1TextureResolution, 64, 1024);
		Result.UpperHemisphereL1SampleCount =
			FMath::Clamp(Request.UpperHemisphereL1SampleCount, 4, 32);
		Result.UpperHemisphereL1ShadowMapResolution =
			FMath::Clamp(Request.UpperHemisphereL1ShadowMapResolution, 64, 1024);

		const FString FeatureSuffix = Request.Mode == EFoliageBakerCardMode::CrossCards
			? TEXT("_Cross")
			: UsesMultiBillboard(Request)
				? TEXT("_MultiBillboard")
				: UsesDoublePlanesBillboard(Request)
					? TEXT("_DoubleBillboard")
					: TEXT("_Billboard");
		Result.BaseColorOpacityTextureSuffix = FeatureSuffix + Request.BaseColorOpacityTextureSuffix;
		Result.NormalDepthTextureSuffix = FeatureSuffix + Request.NormalDepthTextureSuffix;
		Result.MixTextureSuffix = FeatureSuffix + Request.MixTextureSuffix;
		Result.UpperHemisphereL1VisibilityTextureSuffix =
			FeatureSuffix + Request.UpperHemisphereL1VisibilityTextureSuffix;
		Result.MaterialInstanceNameSuffix = FeatureSuffix + Request.MaterialInstanceNameSuffix;
		return Result;
	}

	FFoliageBakerCardBakeResult BuildCardBakeResult(const FProxyAssetBuildResult& BuildResult)
	{
		FFoliageBakerCardBakeResult Result;
		Result.bSucceeded = BuildResult.bSucceeded;
		Result.bCancelled = BuildResult.bCancelled;
		Result.ProxyMesh = BuildResult.ProxyMesh;
		Result.SourceMeshLODIndex = BuildResult.SourceMeshLODIndex;
		Result.ColorOpacityTexture = BuildResult.AtlasTexture;
		Result.NormalDepthTexture = BuildResult.NormalAtlasTexture;
		Result.MixTexture = BuildResult.MixAtlasTexture;
		Result.UpperHemisphereL1VisibilityTexture =
			BuildResult.UpperHemisphereL1VisibilityTexture;
		Result.MaterialInstance = BuildResult.Material;
		Result.Report = BuildResult.Report;
		AppendCardCreatedAssets(BuildResult, Result.CreatedAssets);
		return Result;
	}
}


FFoliageBakerCardBakeResult FFoliageBakerCardBaker::Bake(
	UStaticMesh& SourceStaticMesh,
	UMaterialInstanceConstant& MaterialTemplate,
	const FFoliageBakerCardBakeRequest& Request,
	const FFoliageBakerMeshOutputSelector& MeshOutputSelector)
{
	FString ValidationFailureReport;
	if (!ValidateCardBakeRequest(
			SourceStaticMesh,
			Request,
			MeshOutputSelector,
			ValidationFailureReport))
	{
		FFoliageBakerCardBakeResult Result;
		Result.Report = MoveTemp(ValidationFailureReport);
		return Result;
	}
	const FFoliageBakerCardBakeRequest SanitizedRequest = SanitizeCardBakeRequest(Request);

	FProxyAssetBuildResult InternalResult;
	{
		FCardBakePipeline Pipeline(
			SourceStaticMesh,
			MaterialTemplate,
			SanitizedRequest,
			MeshOutputSelector);
		InternalResult = Pipeline.Run();
	}
	return BuildCardBakeResult(InternalResult);
}
