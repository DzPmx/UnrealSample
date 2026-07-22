#include "FoliageBakerBillboardCloudsBaker.h"

#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerKMeansPlaneCover.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedAtlasBake.h"
#include "FoliageBakerProxyGeometry.h"
#include "FoliageBakerSourceMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerBillboardClouds, Log, All);

namespace
{
	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(const FBoxSphereBounds& SourceLODBounds, const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		switch (EditorSettings.KMeansCrackReductionMode)
		{
		case EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip:
			Settings.CrackReductionMode = UE::FoliageBaker::PlaneCover::EPlaneProxyCrackReductionMode::ScaledEnvelopeClip;
			break;
		case EBillboardCloudsCrackReductionMode::Off:
		default:
			Settings.CrackReductionMode = UE::FoliageBaker::PlaneCover::EPlaneProxyCrackReductionMode::Off;
			break;
		}
		Settings.CrackReductionProjectionScale = FMath::Clamp(EditorSettings.KMeansCrackReductionProjectionScale, 0.0, 1.0);
		Settings.TextureAtlasResolution = FMath::Clamp(EditorSettings.TextureAtlasResolution, 256, 4096);
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMinV;
		switch (EditorSettings.DoubleSidedBakeMode)
		{
		case EBillboardCloudsDoubleSidedBakeMode::TrunkCardsOnly:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::TrunkCardsOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::BillboardPlanesOnly:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::BillboardPlanesOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::AllPlanes:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::Off:
		default:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
			break;
		}
		Settings.ErrorTolerance = FMath::Max(
			FMath::Max(0.0, EditorSettings.MinimumErrorCm),
			SourceLODBounds.SphereRadius * FMath::Max(0.0, EditorSettings.RelativeError));
		switch (EditorSettings.TrunkCardAtlasScale)
		{
		case EBillboardCloudsTrunkCardAtlasScale::HalfX:
			Settings.TrunkCardAtlasScale = 0.5;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::OneX:
			Settings.TrunkCardAtlasScale = 1.0;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::OnePointFiveX:
			Settings.TrunkCardAtlasScale = 1.5;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::TwoX:
			Settings.TrunkCardAtlasScale = 2.0;
			break;
		default:
			Settings.TrunkCardAtlasScale = 1.0;
			break;
		}
		Settings.bEnableAlphaAwareTileCrop = EditorSettings.bEnableAlphaAwareTileCrop;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(EditorSettings.AlphaAwareTileCropGuardPixels, 2, 16);
		return Settings;
	}

	UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings BuildKMeansSettings(
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings Settings;
		Settings.PlaneCount = FMath::Clamp(EditorSettings.KMeansPlaneCount, 1, 512);
		Settings.MaxIterations = FMath::Clamp(EditorSettings.KMeansMaxIterations, 1, 512);
		return Settings;
	}

	struct FTrunkCardTriangleSplit
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> BillboardTriangles;
		TArray<int32> BillboardToSourceTriangleIndices;
		TArray<int32> TrunkTriangleIndices;
	};

	FTrunkCardTriangleSplit SplitTrianglesForTrunkCards(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const bool bEnableTrunkCards,
		const TArray<FString>& RawKeywords)
	{
		FTrunkCardTriangleSplit Split;
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(
				StaticMesh,
				RawKeywords);
		Split.bEnabled = bEnableTrunkCards && MaterialMatches.bEnabled;
		Split.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;
		Split.BillboardTriangles.Reserve(SourceTriangles.Num());
		Split.BillboardToSourceTriangleIndices.Reserve(SourceTriangles.Num());

		for (int32 SourceTriangleIndex = 0; SourceTriangleIndex < SourceTriangles.Num(); ++SourceTriangleIndex)
		{
			UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = SourceTriangles[SourceTriangleIndex];
			Triangle.bIsTrunk = MaterialMatches.IsMatch(Triangle.MaterialIndex);
			const bool bUseTrunkCards = Split.bEnabled && Triangle.bIsTrunk;
			Triangle.bTrunkCardOnly = bUseTrunkCards;
			if (bUseTrunkCards)
			{
				Split.TrunkTriangleIndices.Add(SourceTriangleIndex);
			}
			else
			{
				Split.BillboardToSourceTriangleIndices.Add(SourceTriangleIndex);
				Split.BillboardTriangles.Add(Triangle);
			}
		}

		return Split;
	}

	UE::FoliageBaker::PlaneCover::FPlaneProxySet RemapPlaneCoverResultToSourceTriangles(
		const UE::FoliageBaker::PlaneCover::FPlaneProxySet& BillboardResult,
		const TArray<int32>& BillboardToSourceTriangleIndices,
		const int32 SourceTriangleCount)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySet RemappedResult = BillboardResult;
		RemappedResult.SourceTriangleCount = SourceTriangleCount;
		for (UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane : RemappedResult.Planes)
		{
			TArray<int32> RemappedTriangleIndices;
			RemappedTriangleIndices.Reserve(Plane.TriangleIndices.Num());
			for (const int32 BillboardTriangleIndex : Plane.TriangleIndices)
			{
				if (BillboardToSourceTriangleIndices.IsValidIndex(BillboardTriangleIndex))
				{
					RemappedTriangleIndices.Add(BillboardToSourceTriangleIndices[BillboardTriangleIndex]);
				}
			}
			Plane.TriangleIndices = MoveTemp(RemappedTriangleIndices);
		}
		return RemappedResult;
	}

	int32 AppendTrunkCrossCardPlanes(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<int32>& TrunkTriangleIndices,
		const int32 RequestedTrunkPlaneCount,
		UE::FoliageBaker::PlaneCover::FPlaneProxySet& InOutResult)
	{
		if (TrunkTriangleIndices.IsEmpty())
		{
			return 0;
		}

		FBox TrunkBounds(ForceInit);
		double TrunkArea = 0.0;
		for (const int32 TriangleIndex : TrunkTriangleIndices)
		{
			if (!SourceTriangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = SourceTriangles[TriangleIndex];
			TrunkArea += Triangle.Area;
			for (const FVector& Vertex : Triangle.Vertices)
			{
				TrunkBounds += Vertex;
			}
		}

		if (!TrunkBounds.IsValid)
		{
			return 0;
		}

		const int32 TrunkPlaneCount = FMath::Clamp(RequestedTrunkPlaneCount, 2, 8);
		for (int32 PlaneIndex = 0; PlaneIndex < TrunkPlaneCount; ++PlaneIndex)
		{
			const double AngleRadians = static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(TrunkPlaneCount);
			const FVector Normal(FMath::Cos(AngleRadians), FMath::Sin(AngleRadians), 0.0);
			const FVector AxisV = FVector::UpVector;
			FVector AxisU = FVector::CrossProduct(AxisV, Normal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane = InOutResult.Planes.AddDefaulted_GetRef();
			Plane.Normal = Normal;
			Plane.Rho = 0.0;
			Plane.Score = TrunkArea;
			Plane.CoveredArea = TrunkArea;
			Plane.TriangleIndices = TrunkTriangleIndices;
			Plane.bIsTrunkCard = true;
			Plane.bUseFixedPlaneFrame = true;
			Plane.FixedAxisU = AxisU;
			Plane.FixedAxisV = AxisV;
		}

		InOutResult.CoveredTriangleCount += TrunkTriangleIndices.Num();
		InOutResult.CoveredArea += TrunkArea;
		return TrunkPlaneCount;
	}

	FString BuildTrunkCrossCardSummary(const FTrunkCardTriangleSplit& Split, const int32 TrunkPlaneCount, const double TrunkCardAtlasScale)
	{
		if (!Split.bEnabled)
		{
			return TEXT("");
		}

		FString LayoutName = TEXT("off");
		if (TrunkPlaneCount == 2)
		{
			LayoutName = TEXT("cross card");
		}
		else if (TrunkPlaneCount == 3)
		{
			LayoutName = TEXT("three-way star");
		}
		else if (TrunkPlaneCount == 4)
		{
			LayoutName = TEXT("four-way star");
		}
		else if (TrunkPlaneCount > 4)
		{
			LayoutName = FString::Printf(TEXT("%d-way star"), TrunkPlaneCount);
		}

		return FString::Printf(
			TEXT("\n  trunk cards: enabled, matched materials=%d, trunk triangles=%d, billboard input triangles=%d, vertical planes=%d (%s), atlas scale=%.1fx, origin-centered, shooting=horizontal ortho trunk-only"),
			Split.MatchedMaterialCount,
			Split.TrunkTriangleIndices.Num(),
			Split.BillboardTriangles.Num(),
			TrunkPlaneCount,
			*LayoutName,
			FMath::Clamp(TrunkCardAtlasScale, 0.5, 2.0));
	}

	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;

	using FAtlasBakeStats = UE::FoliageBaker::ProjectedAtlasBake::FStats;

	bool BakeBillboardAtlasGPU(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats& ProxyStats,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const FAtlasOutputSelection& OutputSelection,
		TArray<FColor>& OutPixels,
		TArray<FColor>& OutNormalPixels,
		TArray<FColor>& OutMixPixels,
		FAtlasBakeStats& OutStats,
		FString& OutError)
	{
		const UE::FoliageBaker::ProjectedAtlasBake::FInputs Inputs(
			SourceStaticMesh,
			SourceLODBounds,
			Triangles,
			PlaneInfos,
			ProxyStats,
			Settings);
		UE::FoliageBaker::ProjectedAtlasBake::FPolicy Policy;
		Policy.OutputSelection = OutputSelection;
		Policy.NormalAlphaMode =
			UE::FoliageBaker::ProjectedAtlasBake::ENormalAlphaMode::SourceDepth;
		Policy.InvalidMaterialPolicy =
			UE::FoliageBaker::ProjectedAtlasBake::EInvalidMaterialPolicy::UseDefaultMaterial;
		Policy.bIncludeCrackReductionForTrunkCards = false;
		Policy.DiagnosticName = TEXT("BillboardClouds atlas");
		Policy.MaterialAlphaPolicyDetails =
			TEXT(" source masked-shader coverage controls one shared per-tile RDG depth competition; the winning fragment supplies BaseColor, object normal, source triangle ID, packed Mix, and shared-range depth; no CPU material-property fallback");

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
		OutStats = MoveTemp(Result.Stats);
		return true;
	}

	UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest
	MakeBillboardCloudsTextureAssetRequest(
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FString& AssetNameSuffix)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request;
		Request.OutputFolderName = EditorSettings.TextureOutputFolderName;
		Request.OutputPackagePathOverride = OutputPackagePathOverride;
		Request.AssetNamePrefix = EditorSettings.TextureNamePrefix;
		Request.AssetNameSuffix = AssetNameSuffix;
		Request.CompressionSettings = TC_BC7;
		return Request;
	}

	UTexture2D* CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeBillboardCloudsTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.BaseColorOpacityTextureSuffix);
		Request.LODGroup = TEXTUREGROUP_World;
		Request.bSRGB = true;
		Request.SemanticMaskMipCoverageThreshold =
			EditorSettings.bPreserveAlphaMaskValues
				? FMath::Clamp(
					EditorSettings.MipMaskCoverageThreshold,
					0.01f,
					1.0f)
				: 0.0f;
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
			PlaneInfos,
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeBillboardCloudsTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.NormalTextureSuffix);
		Request.MipBackgroundColor = FColor(128, 128, 255, 255);
		Request.LODGroup = TEXTUREGROUP_WorldNormalMap;
		Request.bSRGB = false;
		Request.EmptyPixelsError = TEXT("No normal atlas pixels were generated.");
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
			PlaneInfos,
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeBillboardCloudsTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.MixTextureSuffix);
		Request.MipBackgroundColor = FColor(255, 128, 0, 0);
		Request.LODGroup = TEXTUREGROUP_WorldSpecular;
		Request.bSRGB = false;
		Request.EmptyPixelsError = TEXT("No mix atlas pixels were generated.");
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
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

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const UFoliageBakerBillboardCloudsSettings& Settings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = MeshOutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = MeshOutputSelection.ReplaceLODIndex;
		Params.RequestedInsertAfterLODIndex = MeshOutputSelection.InsertAfterLODIndex;
		Params.SourceLODIndex = Settings.SourceLODIndex;
		Params.DesiredUVChannelCount = 3;

		Params.RebuildLODMetadataKey = FName(TEXT("FoliageBaker.BillboardCloudsLOD"));
		return Params;
	}

	struct FProxyPlaneCoverBuildData : FFoliageBakerSourceMeshData
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings KMeansSettings;
		FTrunkCardTriangleSplit TrunkSplit;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverResult BillboardResult;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
		bool bHasBillboardResult = false;
		int32 TrunkPlaneCount = 0;
	};

	using FProxyMeshBuildData = FFoliageBakerProxyGeometry;

	struct FProxyTextureBuildData
	{
		FAtlasOutputSelection OutputSelection;
		TArray<FColor> AtlasPixels;
		TArray<FColor> NormalAtlasPixels;
		TArray<FColor> MixAtlasPixels;
		FAtlasBakeStats AtlasStats;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	struct FProxyAssetBuildResult
	{
		bool bSucceeded = false;
		bool bCancelled = false;
		FString Report;
		UStaticMesh* ProxyMesh = nullptr;
		EFoliageBakerMeshAssetOutputMode MeshOutputMode = EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		const FString MeshName = StaticMesh.GetName();
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *MeshName, *Error);
		UE_LOG(LogFoliageBakerBillboardClouds, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FProxyAssetBuildResult MakeProxyBuildCancelled(const UStaticMesh& StaticMesh)
	{
		FProxyAssetBuildResult Result;
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled after bake: no mesh output was selected and no generated assets were committed."),
			*StaticMesh.GetName());
		return Result;
	}

	FString BuildBillboardCloudsOrTrunkSummary(
		const UStaticMesh& StaticMesh,
		const FProxyPlaneCoverBuildData& CoverData)
	{
		FString Summary;
		if (CoverData.bHasBillboardResult)
		{
			Summary = UE::FoliageBaker::BillboardClouds::SummarizeKMeansPlaneCover(
				StaticMesh.GetName(),
				CoverData.KMeansSettings,
				CoverData.Settings,
				CoverData.BillboardResult);
		}
		else
		{
			Summary = FString::Printf(
				TEXT("%s\n  algorithm: Billboard Clouds skipped; all matched triangles are routed to fixed trunk cross-card planes"),
				*StaticMesh.GetName());
		}

		Summary += BuildTrunkCrossCardSummary(CoverData.TrunkSplit, CoverData.TrunkPlaneCount, CoverData.Settings.TrunkCardAtlasScale);
		return Summary;
	}

	const TCHAR* GetTextureShootingMode()
	{
		return TEXT("GPU bake, per-plane tile UV, K-Means cluster projection");
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		FAtlasOutputSelection OutputSelection;
		OutputSelection.bBaseColorOpacity = EditorSettings.bBakeBaseColorOpacityAtlas;
		OutputSelection.bNormalMask = EditorSettings.bBakeNormalMaskAtlas;
		OutputSelection.bMix = EditorSettings.bBakeMixAtlas;
		OutputSelection.bMaterialScalarAverages = !EditorSettings.bBakeMixAtlas;
		return OutputSelection;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		if (!FFoliageBakerSourceMeshReader::Read(
				StaticMesh,
				EditorSettings.SourceLODIndex,
				OutData,
				OutError))
		{
			return false;
		}

		OutData.Settings = BuildSettingsForMesh(OutData.SourceLODBounds, EditorSettings);
		OutData.KMeansSettings = BuildKMeansSettings(EditorSettings);
		const int32 RequestedTrunkPlaneCount = FMath::Clamp(EditorSettings.TrunkCardPlaneCount, 2, 8);
		OutData.TrunkSplit = SplitTrianglesForTrunkCards(StaticMesh, OutData.Triangles, EditorSettings.bEnableTrunkCards, EditorSettings.TrunkCardMaterialKeywords);

		if (!OutData.TrunkSplit.BillboardTriangles.IsEmpty())
		{
			OutData.BillboardResult = UE::FoliageBaker::BillboardClouds::BuildKMeansPlaneCover(
				OutData.TrunkSplit.BillboardTriangles,
				OutData.KMeansSettings);
			OutData.ProxyResult = RemapPlaneCoverResultToSourceTriangles(OutData.BillboardResult, OutData.TrunkSplit.BillboardToSourceTriangleIndices, OutData.Triangles.Num());
			OutData.bHasBillboardResult = true;
		}
		else
		{
			OutData.ProxyResult.SourceTriangleCount = OutData.Triangles.Num();
		}

		OutData.TrunkPlaneCount = AppendTrunkCrossCardPlanes(OutData.Triangles, OutData.TrunkSplit.TrunkTriangleIndices, RequestedTrunkPlaneCount, OutData.ProxyResult);
		if (OutData.ProxyResult.Planes.IsEmpty())
		{
			OutError = TEXT("no Billboard Clouds planes or trunk card planes were generated.");
			return false;
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
			CoverData.ProxyResult,
			CoverData.Settings,
			OutData.MeshDescription,
			OutData.Stats,
			OutError,
			&OutData.PlaneInfos);
	}

	bool BuildProxyTextureData(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		const FFoliageBakerGeneratedAssetOutputFolders& OutputFolders,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable BaseColor/Opacity, Normal/Depth, or Mix in the Billboard Clouds tool panel.");
			return false;
		}

		TSet<FName> EnabledTextureParameterNames;
		auto ValidateTextureParameterName = [&EnabledTextureParameterNames, &OutError](bool bOutputEnabled, FName ParameterName, const TCHAR* OutputLabel) -> bool
		{
			if (!bOutputEnabled)
			{
				return true;
			}
			if (ParameterName.IsNone())
			{
				OutError = FString::Printf(TEXT("The %s texture output is enabled, but its material texture parameter name is None."), OutputLabel);
				return false;
			}
			if (EnabledTextureParameterNames.Contains(ParameterName))
			{
				OutError = FString::Printf(TEXT("Material texture parameter name '%s' is assigned to more than one enabled texture output."), *ParameterName.ToString());
				return false;
			}
			EnabledTextureParameterNames.Add(ParameterName);
			return true;
		};
		if (!ValidateTextureParameterName(OutData.OutputSelection.bBaseColorOpacity, EditorSettings.BaseColorOpacityTextureParameterName, TEXT("BaseColor/Opacity"))
			|| !ValidateTextureParameterName(OutData.OutputSelection.bNormalMask, EditorSettings.NormalDepthTextureParameterName, TEXT("Normal/Depth"))
			|| !ValidateTextureParameterName(OutData.OutputSelection.bMix, EditorSettings.MixTextureParameterName, TEXT("Mix")))
		{
			return false;
		}
		UMaterialInstanceConstant* TemplateMaterialInstance =
			EditorSettings.BillboardMaterialTemplate.LoadSynchronous();
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("Billboard Clouds Parent Material Instance is not selected in the current tool settings.");
			return false;
		}

		int32 AlphaAwareCroppedPlaneCount = 0;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			FAtlasBakeStats CropStats;
			if (!BakeBillboardAtlasGPU(
				StaticMesh,
				CoverData.SourceLODBounds,
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				CropOutputSelection,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropStats,
				OutError))
			{
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				CoverData.Settings.AlphaAwareTileCropGuardPixels,
				1,
				TileCrops);

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::FoliageBaker::PlaneCover::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					TileCrops,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError))
				{
					return false;
				}
			}
		}

		if (!BakeBillboardAtlasGPU(
			StaticMesh,
			CoverData.SourceLODBounds,
			CoverData.Triangles,
			MeshData.PlaneInfos,
			MeshData.Stats,
			CoverData.Settings,
			OutData.OutputSelection,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.AtlasStats,
			OutError))
		{
			return false;
		}
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? CoverData.Settings.AlphaAwareTileCropGuardPixels
			: 0;

		if (OutData.OutputSelection.bBaseColorOpacity)
		{
			OutData.AtlasTexture = CreateAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.AtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.AtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bNormalMask)
		{
			OutData.NormalAtlasTexture = CreateNormalAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.NormalAtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.NormalAtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bMix)
		{
			OutData.MixAtlasTexture = CreateMixAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.MixAtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.MixAtlasTexture)
			{
				return false;
			}
		}

		FFoliageBakerMaterialInstanceAssetParams MaterialParams;
		MaterialParams.OutputFolderName = EditorSettings.MaterialOutputFolderName;
		MaterialParams.OutputPackagePathOverride = OutputFolders.MaterialPackagePath;
		MaterialParams.AssetNamePrefix = EditorSettings.MaterialInstanceNamePrefix;
		MaterialParams.AssetNameSuffix = EditorSettings.MaterialInstanceNameSuffix;
		MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MaterialParams.BaseColorOpacityTextureParameterName = EditorSettings.BaseColorOpacityTextureParameterName;
		MaterialParams.NormalDepthTextureParameterName = EditorSettings.NormalDepthTextureParameterName;
		MaterialParams.MixTextureParameterName = EditorSettings.MixTextureParameterName;
		if (OutData.OutputSelection.bMaterialScalarAverages)
		{
			const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
				ParameterNames = {
					EditorSettings.LeafRoughnessParameterName,
					EditorSettings.LeafSpecularParameterName,
					EditorSettings.TrunkRoughnessParameterName,
					EditorSettings.TrunkSpecularParameterName,
				};
			if (!UE::FoliageBaker::MaterialResolver::ResolveTrunkLeafMaterialScalarParameters(
					OutData.AtlasStats.MaterialAverages,
					ParameterNames,
					MaterialParams.ScalarParameterValues,
					OutError))
			{
				return false;
			}
		}
		MaterialParams.MissingTemplateError = TEXT("Billboard Clouds Parent Material Instance is not selected in the current tool settings.");
		OutData.Material = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
			StaticMesh,
			AssetTransaction,
			MaterialParams,
			TemplateMaterialInstance,
			OutData.AtlasTexture,
			OutData.NormalAtlasTexture,
			OutData.MixAtlasTexture,
			OutError);
		return OutData.Material != nullptr;
	}

	bool CreateProxyMeshAssetBundle(
		UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = MeshOutputSelection.OutputMode;

		if (MeshOutputSelection.OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix = TEXT("_BillboardCloudProxy");
			MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
			MeshParams.DesiredUVChannelCount = 3;
			OutResult.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
				StaticMesh,
				AssetTransaction,
				MeshParams,
				MeshData.MeshDescription,
				TextureData.Material,
				OutError);
			if (!OutResult.ProxyMesh)
			{
				return false;
			}
		}
		else
		{
			int32 InstalledLODIndex = INDEX_NONE;
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection),
				MeshData.MeshDescription,
				TextureData.Material,
				InstalledLODIndex,
				OutError))
			{
				return false;
			}

			OutResult.ProxyMesh = &StaticMesh;
			OutResult.SourceMeshLODIndex = InstalledLODIndex;
		}

		OutResult.AtlasTexture = TextureData.AtlasTexture;
		OutResult.NormalAtlasTexture = TextureData.NormalAtlasTexture;
		OutResult.MixAtlasTexture = TextureData.MixAtlasTexture;
		OutResult.Material = TextureData.Material;
		return true;
	}

	FString BuildProxySuccessReport(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);

		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm"),
			*BuildBillboardCloudsOrTrunkSummary(StaticMesh, CoverData),
			CoverData.SourceLODIndex,
			CoverData.SourceLODBounds.SphereRadius);
		const FString BaseAtlasPath = TextureData.AtlasTexture ? TextureData.AtlasTexture->GetPathName() : TEXT("disabled");
		const FString NormalAtlasPath = TextureData.NormalAtlasTexture ? TextureData.NormalAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MixAtlasPath = TextureData.MixAtlasTexture ? TextureData.MixAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/Opacity=%s, Normal/Depth=%s, Mix=%s"),
			*EditorSettings.BaseColorOpacityTextureParameterName.ToString(),
			*EditorSettings.NormalDepthTextureParameterName.ToString(),
			*EditorSettings.MixTextureParameterName.ToString());
		const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
			MaterialScalarParameterNames = {
				EditorSettings.LeafRoughnessParameterName,
				EditorSettings.LeafSpecularParameterName,
				EditorSettings.TrunkRoughnessParameterName,
				EditorSettings.TrunkSpecularParameterName,
			};
		const FString MaterialScalarDetails =
			UE::FoliageBaker::MaterialResolver::BuildTrunkLeafMaterialAveragesReport(
				!EditorSettings.bBakeMixAtlas,
				TextureData.AtlasStats.MaterialAverages,
				MaterialScalarParameterNames);

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, rasterized refs=%d, crack-reduction refs=%d, masked refs=%d, shooting=%s, resolve=shared per-tile RDG masked depth; primary and crack-reduction geometry compete in the same depth target\n  base/color opacity atlas: %s, RGB=BaseColor, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  normal/depth atlas: %s, RGB=object/local-space normal, A=shared selected-source-LOD bounds linear depth (near 1, far 0, uncovered 1); WPO disabled during material baking\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks from the same GPU depth winner\n  material scalar averages: %s\n  trunk/leaf classification: ColorOpacity.A and UV2, trunk alpha=0.5 (128), leaf alpha=1 (255), UV2 trunk=(0,0), billboard/leaf=(1,0)\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (child of the Editor Preferences parent; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision generation off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals"),
			*TechniqueSummary,
			*AlphaPolicyDetails,
			*MeshOutputDetails,
			MeshData.Stats.PlaneCount,
			MeshData.Stats.QuadCount,
			MeshData.Stats.TriangleCount,
			TextureData.AtlasStats.Width,
			TextureData.AtlasStats.Height,
			TextureData.AtlasStats.TileResolution,
			TextureData.AtlasStats.PackedTileUtilizationPercent,
			TextureData.AtlasStats.FrontTileCount,
			TextureData.AtlasStats.BackTileCount,
			TextureData.AtlasStats.PaintedPixels,
			TextureData.AtlasStats.AlphaAwareCroppedPlanes,
			TextureData.AtlasStats.AlphaAwareTileCropGuardPixels,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.CrackReductionTriangleReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			GetTextureShootingMode(),
			*BaseAtlasPath,
			*NormalAtlasPath,
			*MixAtlasPath,
			*MaterialScalarDetails,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails);
	}

	FProxyAssetBuildResult BuildBillboardCloudProxyAsset(
		UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		FString Error;
		FProxyPlaneCoverBuildData CoverData;
		if (!BuildProxyPlaneCoverData(StaticMesh, EditorSettings, CoverData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyMeshBuildData MeshData;
		if (!BuildProxyMeshData(CoverData, MeshData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
			FFoliageBakerMeshOutputDialog::OpenAfterBake(StaticMesh, EditorSettings.SourceLODIndex);
		if (!MeshOutputSelection.IsSet())
		{
			return MakeProxyBuildCancelled(StaticMesh);
		}
		if (MeshOutputSelection->OutputMode != EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
				StaticMesh,
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection.GetValue()),
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FFoliageBakerGeneratedAssetOutputFolders OutputFolders;
		if (EditorSettings.bPlaceGeneratedAssetsNearReplacedLODAssets
			&& MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
		{
			OutputFolders = FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
				StaticMesh,
				MeshOutputSelection->ReplaceLODIndex);
		}

		FProxyTextureBuildData TextureData;
		FFoliageBakerAssetTransaction AssetTransaction;
		if (!BuildProxyTextureData(
				StaticMesh,
				EditorSettings,
				AssetTransaction,
				CoverData,
				MeshData,
				OutputFolders,
				TextureData,
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(
				StaticMesh,
				EditorSettings,
				AssetTransaction,
				MeshData,
				TextureData,
				MeshOutputSelection.GetValue(),
				Result,
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
		AssetTransaction.Commit();

		Result.bSucceeded = true;
		Result.Report = BuildProxySuccessReport(StaticMesh, EditorSettings, CoverData, MeshData, TextureData, Result);
		UE_LOG(LogFoliageBakerBillboardClouds, Display, TEXT("\n%s"), *Result.Report);
		return Result;
	}

	void AppendProxyCreatedAssets(const FProxyAssetBuildResult& BuildResult, TArray<UObject*>& OutCreatedAssets)
	{
		if (BuildResult.ProxyMesh)
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
		if (BuildResult.Material)
		{
			OutCreatedAssets.Add(BuildResult.Material);
		}
	}
}

bool FFoliageBakerBillboardCloudsBaker::HasAnyAtlasOutput(
	const UFoliageBakerBillboardCloudsSettings& Settings)
{
	return BuildAtlasOutputSelection(Settings).HasAnyOutput();
}

FFoliageBakerBillboardCloudsBakeResult FFoliageBakerBillboardCloudsBaker::Bake(
	UStaticMesh& StaticMesh,
	const UFoliageBakerBillboardCloudsSettings& Settings)
{
	const FProxyAssetBuildResult BuildResult =
		BuildBillboardCloudProxyAsset(StaticMesh, Settings);
	FFoliageBakerBillboardCloudsBakeResult Result;
	Result.bSucceeded = BuildResult.bSucceeded;
	Result.bCancelled = BuildResult.bCancelled;
	Result.Report = BuildResult.Report;
	if (BuildResult.bSucceeded)
	{
		AppendProxyCreatedAssets(BuildResult, Result.CreatedAssets);
	}
	return Result;
}
