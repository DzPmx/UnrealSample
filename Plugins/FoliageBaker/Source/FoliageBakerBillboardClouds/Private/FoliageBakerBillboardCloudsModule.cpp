#include "FoliageBakerBillboardCloudsModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerKMeansPlaneCover.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Docking/TabManager.h"
#include "IDetailCustomization.h"
#include "IDetailsView.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "MeshDescription.h"
#include "MaterialBakingStructures.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "StaticMeshResources.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerBillboardCloudsModule"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerBillboardClouds, Log, All);

namespace
{
	using UE::FoliageBaker::ProjectedMaterialBake::EncodeObjectSpaceNormalToColor;

	class FFoliageBakerCategoryOrderCustomization final : public IDetailCustomization
	{
	public:
		static TSharedRef<IDetailCustomization> MakeInstance()
		{
			return MakeShared<FFoliageBakerCategoryOrderCustomization>();
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			DetailBuilder.EditCategory(TEXT("Mesh")).SetSortOrder(0);
			DetailBuilder.EditCategory(TEXT("Feature")).SetSortOrder(1);
			DetailBuilder.EditCategory(TEXT("Asset")).SetSortOrder(2);
			DetailBuilder.EditCategory(TEXT("Material")).SetSortOrder(3);
		}
	};

	const FName BillboardCloudsToolTabName(TEXT("BillboardCloudsTools"));

	bool ComputeSourceTriangleBounds(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		FBoxSphereBounds& OutBounds)
	{
		FBox Bounds(ForceInit);
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Bounds += Vertex;
			}
		}
		if (!Bounds.IsValid)
		{
			OutBounds = FBoxSphereBounds(ForceInitToZero);
			return false;
		}

		OutBounds = FBoxSphereBounds(Bounds);
		return true;
	}

	TArray<UStaticMesh*> GetSelectedStaticMeshes()
	{
		TArray<FAssetData> SelectedAssets;
		AssetSelectionUtils::GetSelectedAssets(SelectedAssets);

		TArray<UStaticMesh*> StaticMeshes;
		for (const FAssetData& AssetData : SelectedAssets)
		{
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(AssetData.GetAsset()))
			{
				StaticMeshes.Add(StaticMesh);
			}
		}

		return StaticMeshes;
	}

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
		Settings.SourceMaterialBakeResolution = FMath::Clamp(EditorSettings.SourceMaterialBakeResolution, 256, 4096);
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
		case EBillboardCloudsTrunkCardAtlasScale::OnePointFiveX:
			Settings.TrunkCardAtlasScale = 1.5;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::TwoX:
			Settings.TrunkCardAtlasScale = 2.0;
			break;
		default:
			Settings.TrunkCardAtlasScale = 2.0;
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
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches = bEnableTrunkCards
			? UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(StaticMesh, RawKeywords)
			: UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult();
		Split.bEnabled = bEnableTrunkCards && MaterialMatches.bEnabled;
		Split.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;
		Split.BillboardTriangles.Reserve(SourceTriangles.Num());
		Split.BillboardToSourceTriangleIndices.Reserve(SourceTriangles.Num());

		for (int32 SourceTriangleIndex = 0; SourceTriangleIndex < SourceTriangles.Num(); ++SourceTriangleIndex)
		{
			UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = SourceTriangles[SourceTriangleIndex];
			const bool bUseTrunkCards = Split.bEnabled && MaterialMatches.IsMatch(Triangle.MaterialIndex);
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

		const int32 TrunkPlaneCount = FMath::Clamp(RequestedTrunkPlaneCount, 2, 4);
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

		const TCHAR* LayoutName = TEXT("off");
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

		return FString::Printf(
			TEXT("\n  trunk cards: enabled, matched materials=%d, trunk triangles=%d, billboard input triangles=%d, vertical planes=%d (%s), atlas scale=%.1fx, origin-centered, shooting=horizontal ortho trunk-only"),
			Split.MatchedMaterialCount,
			Split.TrunkTriangleIndices.Num(),
			Split.BillboardTriangles.Num(),
			TrunkPlaneCount,
			LayoutName,
			FMath::Max(1.0, TrunkCardAtlasScale));
	}

	using EBillboardOpacityMaskChannel = UE::FoliageBaker::MaterialResolver::EOpacityMaskChannel;
	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;
	using FMaterialBakeData = UE::FoliageBaker::MaterialResolver::FMaterialBakeData;
	using FMaterialScalarBakeData = UE::FoliageBaker::MaterialResolver::FMaterialScalarBakeData;

	struct FAtlasBakeStats : UE::FoliageBaker::MaterialResolver::FMaterialResolveStats
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 TileResolution = 0;
		int32 TilePaddingPixels = 0;
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 SourceTexturedTriangles = 0;
		int32 FallbackTriangles = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 CrackReductionTriangleReferences = 0;
		int32 SourceMixTextureReferences = 0;
		int32 TextureAlphaOpacityReferences = 0;
		int32 ForcedOpaqueAlphaReferences = 0;
		int32 GpuOpacityExportReferences = 0;
		int32 GpuOpacityExportFailedReferences = 0;
		int32 BakedOpacityClipZeroedPixels = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;

		struct FBakeChannelAgg
		{
			int64 TotalPixels = 0;
			int64 BackgroundPixels = 0;
			int64 ZeroRgbPixels = 0;
			int64 FullWhiteRgbPixels = 0;
			int64 OtherRgbPixels = 0;
			int64 SumR = 0;
			uint8 MinR = 255;
			uint8 MaxR = 0;
			int32 BakeCount = 0;
			bool bAny = false;
		};

		struct FBakeMaterialAgg
		{
			int32 SourceBlendMode = -1;
			int32 WantsOpacity = 0;
			int32 WantsBaseColor = 0;
			int32 WantsNormal = 0;
			FBakeChannelAgg BaseColor;
			FBakeChannelAgg Opacity;
			FBakeChannelAgg Normal;
		};

		TMap<FString, FBakeMaterialAgg> GpuBakeDiagnostics;
	};


	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	bool ComputeBarycentric2D(
		const FVector2D& Point,
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		double& OutA,
		double& OutB,
		double& OutC)
	{
		const double Denominator = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		OutA = ((B.Y - C.Y) * (Point.X - C.X) + (C.X - B.X) * (Point.Y - C.Y)) / Denominator;
		OutB = ((C.Y - A.Y) * (Point.X - C.X) + (A.X - C.X) * (Point.Y - C.Y)) / Denominator;
		OutC = 1.0 - OutA - OutB;
		return true;
	}

	float SampleMaterialScalar(const FMaterialScalarBakeData& BakeData, const FVector2f& UV)
	{
		if (!BakeData.bHasReadableTexture || !BakeData.Texture.IsValid())
		{
			return FMath::Clamp(BakeData.Constant, 0.0f, 1.0f);
		}

		const FLinearColor Sample = BakeData.Texture.Sample(UV);
		if (BakeData.bUseLuminance)
		{
			return FMath::Clamp(FMath::Max3(Sample.R, Sample.G, Sample.B), 0.0f, 1.0f);
		}
		switch (BakeData.Channel)
		{
		case EBillboardOpacityMaskChannel::Green: return FMath::Clamp(Sample.G, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Blue: return FMath::Clamp(Sample.B, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Alpha: return FMath::Clamp(Sample.A, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Red:
		default: return FMath::Clamp(Sample.R, 0.0f, 1.0f);
		}
	}

	bool IsPointInsidePlaneProxyEnvelope(const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo, const FVector& Point, const double Tolerance)
	{
		const double U = FVector::DotProduct(Point, PlaneInfo.AxisU);
		const double V = FVector::DotProduct(Point, PlaneInfo.AxisV);
		const double SignedDistance = FVector::DotProduct(PlaneInfo.Normal, Point) - PlaneInfo.Rho;
		return U >= PlaneInfo.EnvelopeMinU - Tolerance
			&& U <= PlaneInfo.EnvelopeMaxU + Tolerance
			&& V >= PlaneInfo.EnvelopeMinV - Tolerance
			&& V <= PlaneInfo.EnvelopeMaxV + Tolerance
			&& SignedDistance >= PlaneInfo.EnvelopeMinSignedDistance - Tolerance
			&& SignedDistance <= PlaneInfo.EnvelopeMaxSignedDistance + Tolerance;
	}

	double ComputePlaneProxyEnvelopeBoundaryDistance(const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo, const FVector& Point)
	{
		const double U = FVector::DotProduct(Point, PlaneInfo.AxisU);
		const double V = FVector::DotProduct(Point, PlaneInfo.AxisV);
		const double SignedDistance = FVector::DotProduct(PlaneInfo.Normal, Point) - PlaneInfo.Rho;
		return FMath::Min3(
			FMath::Min(U - PlaneInfo.EnvelopeMinU, PlaneInfo.EnvelopeMaxU - U),
			FMath::Min(V - PlaneInfo.EnvelopeMinV, PlaneInfo.EnvelopeMaxV - V),
			FMath::Min(SignedDistance - PlaneInfo.EnvelopeMinSignedDistance, PlaneInfo.EnvelopeMaxSignedDistance - SignedDistance));
	}

	bool IsPointNearPlaneProxyEnvelopeBoundary(
		const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FVector& Point,
		const double BoundaryWidth,
		const double Tolerance)
	{
		const double BoundaryDistance = ComputePlaneProxyEnvelopeBoundaryDistance(PlaneInfo, Point);
		return BoundaryDistance >= -Tolerance
			&& BoundaryDistance <= FMath::Max(BoundaryWidth, Tolerance);
	}



































	int32 GetSourceMeshMaxUVChannelCount(const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles)
	{
		int32 ChannelCount = 1;
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
		{
			ChannelCount = FMath::Max(ChannelCount, Triangle.NumUVChannels);
		}
		return FMath::Clamp(ChannelCount, 1, UE::FoliageBaker::PlaneCover::MaxMaterialBakeUVChannels);
	}

	FORCEINLINE bool IsBakerBackgroundPixel(const FColor& Sample, const FColor& Background)
	{
		return Sample.R == Background.R
			&& Sample.G == Background.G
			&& Sample.B == Background.B
			&& Sample.A == Background.A;
	}

	TArray<int32> CollectReferencedMaterialIndices(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections)
	{
		TSet<int32> Set;
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (Triangles.IsValidIndex(TriangleIndex))
			{
				Set.Add(FMath::Max(0, Triangles[TriangleIndex].MaterialIndex));
			}
		}
		for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (Triangles.IsValidIndex(Projection.TriangleIndex))
			{
				Set.Add(FMath::Max(0, Triangles[Projection.TriangleIndex].MaterialIndex));
			}
		}
		TArray<int32> Result = Set.Array();
		Result.Sort();
		return Result;
	}


	bool BakeBillboardAtlasGPU(
		const UStaticMesh& SourceStaticMesh,
		const int32 SourceLODIndex,
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
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;
		OutStats.TilePaddingPixels = ProxyStats.AtlasTilePaddingPixels;

		const int32 AtlasPixelCount = FMath::Max(0, OutStats.Width * OutStats.Height);
		OutPixels.Init(FColor(0, 0, 0, 0), AtlasPixelCount);
		if (OutputSelection.bNormalMask)
		{
			OutNormalPixels.Init(EncodeObjectSpaceNormalToColor(FVector::UpVector, 255), AtlasPixelCount);
		}
		else
		{
			OutNormalPixels.Reset();
		}
		if (OutputSelection.bMix)
		{
			OutMixPixels.Init(FColor(255, 128, 0, 0), AtlasPixelCount);
		}
		else
		{
			OutMixPixels.Reset();
		}

		int64 PackedPaddedTilePixels = 0;
		auto AccumulateAtlasTileStats = [&](const FIntPoint& TileSize, const int32 Padding, const bool bBackFace)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			PackedPaddedTilePixels += static_cast<int64>(TileSize.X + Padding * 2)
				* static_cast<int64>(TileSize.Y + Padding * 2);
			if (bBackFace)
			{
				++OutStats.BackTileCount;
			}
			else
			{
				++OutStats.FrontTileCount;
			}
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateAtlasTileStats(PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels, false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateAtlasTileStats(PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels, true);
			}
		}
		OutStats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, AtlasPixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, AtlasPixelCount);
		const FVector SharedDepthCenter = SourceLODBounds.Origin;
		const double SharedDepthRadius = FMath::Max(
			static_cast<double>(SourceLODBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		const int32 NumSourceUVChannels = GetSourceMeshMaxUVChannelCount(Triangles);

		TArray<FMaterialBakeData> MaterialBakeData;
		if (OutputSelection.bMix)
		{
			FAtlasOutputSelection SourcePropertySelection = OutputSelection;
			SourcePropertySelection.bBaseColorOpacity = false;
			SourcePropertySelection.bNormalMask = false;
			MaterialBakeData = UE::FoliageBaker::MaterialResolver::ResolveMaterialBakeData(
				SourceStaticMesh,
				SourceLODIndex,
				SourceLODBounds,
				Triangles,
				SourcePropertySelection,
				Settings.SourceMaterialBakeResolution,
				false,
				OutStats);
		}
		OutStats.MaterialAlphaPolicyDetails =
			TEXT(" source masked-shader BaseColor/final coverage/source-triangle-id/object-normal share one projected painter input; no color, alpha, normal, or depth fallback");

		auto AccumulateBakeChannel = [](FAtlasBakeStats::FBakeChannelAgg& Agg, const TArray<FColor>* Data, const FColor& Background)
		{
			if (!Data || Data->IsEmpty())
			{
				return;
			}
			Agg.bAny = true;
			++Agg.BakeCount;
			for (const FColor& Color : *Data)
			{
				++Agg.TotalPixels;
				if (IsBakerBackgroundPixel(Color, Background))
				{
					++Agg.BackgroundPixels;
					continue;
				}
				Agg.SumR += Color.R;
				Agg.MinR = FMath::Min(Agg.MinR, Color.R);
				Agg.MaxR = FMath::Max(Agg.MaxR, Color.R);
				if (Color.R == 0 && Color.G == 0 && Color.B == 0)
				{
					++Agg.ZeroRgbPixels;
				}
				else if (Color.R == 255 && Color.G == 255 && Color.B == 255)
				{
					++Agg.FullWhiteRgbPixels;
				}
				else
				{
					++Agg.OtherRgbPixels;
				}
			}
		};

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide) -> bool
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return true;
			}

			TArray<int32> PrimaryTriangleIndices;
			PrimaryTriangleIndices.Reserve(PlaneInfo.TriangleIndices.Num());
			TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection> CrackReductionProjectionsToBake;
			CrackReductionProjectionsToBake.Reserve(PlaneInfo.CrackReductionProjections.Num());
			TBitArray<> QueuedTriangles;
			QueuedTriangles.Init(false, Triangles.Num());
			for (const int32 TriangleIndex : PlaneInfo.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
				{
					QueuedTriangles[TriangleIndex] = true;
					PrimaryTriangleIndices.Add(TriangleIndex);
				}
			}
			if (!PlaneInfo.bIsTrunkCard)
			{
				for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : PlaneInfo.CrackReductionProjections)
				{
					const int32 TriangleIndex = Projection.TriangleIndex;
					if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
					{
						QueuedTriangles[TriangleIndex] = true;
						CrackReductionProjectionsToBake.Add(Projection);
						++OutStats.CrackReductionTriangleReferences;
					}
				}
			}
			if (PrimaryTriangleIndices.IsEmpty() && CrackReductionProjectionsToBake.IsEmpty())
			{
				return true;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			TArray<double> TileDepth;
			TileDepth.Init(TNumericLimits<double>::Max(), TilePixelCount);
			const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
			const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);
			const bool bFlipTextureV = Settings.AtlasVConvention
				== UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
			const TArray<int32> MaterialIndicesUsed = CollectReferencedMaterialIndices(
				Triangles,
				PrimaryTriangleIndices,
				CrackReductionProjectionsToBake);
			const FColor BakeBackground = FColor::Magenta;

			for (const int32 MaterialIndex : MaterialIndicesUsed)
			{
				UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (!MaterialInterface)
				{
					MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
				}
				const EBlendMode SourceBlendMode = MaterialInterface->GetBlendMode();
				const bool bMaterialHasOpacityMask = SourceBlendMode == BLEND_Masked;

				UE::FoliageBaker::ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
				ProjectedBakeParams.TileSize = TileSize;
				ProjectedBakeParams.CaptureRayDirection = CaptureRayDirection;
				ProjectedBakeParams.AtlasVConvention = Settings.AtlasVConvention;
				ProjectedBakeParams.MaterialIndexFilter = MaterialIndex;
				ProjectedBakeParams.NumSourceUVChannels = NumSourceUVChannels;
				ProjectedBakeParams.bBackSide = bBackSide;
				ProjectedBakeParams.bBuildNormalBasisMap = false;

				FMeshDescription PerPlaneMesh;
				TArray<FVector2D> CustomTileUVs;
				TArray<UE::FoliageBaker::ProjectedMaterialBake::FNormalBasisSample> UnusedNormalBasisMap;
				TArray<int32> RasterSourceTriangleIndices;
				int32 MatchingTriangleCount = 0;
				FString ProjectedInputError;
				const bool bBuiltPlaneSideBakeInputs =
					UE::FoliageBaker::ProjectedMaterialBake::BuildPlaneSideBakeInputs(
						Triangles,
						PrimaryTriangleIndices,
						CrackReductionProjectionsToBake,
						PlaneInfo,
						ProjectedBakeParams,
						PerPlaneMesh,
						CustomTileUVs,
						UnusedNormalBasisMap,
						MatchingTriangleCount,
						&ProjectedInputError,
						&RasterSourceTriangleIndices);
				if (MatchingTriangleCount == 0)
				{
					continue;
				}
				if (!bBuiltPlaneSideBakeInputs)
				{
					OutError = FString::Printf(
						TEXT("BillboardClouds projected material input failed for plane %d (%s), material %d: %s"),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						*ProjectedInputError);
					return false;
				}

				OutStats.RasterizedTriangleReferences += MatchingTriangleCount;
				if (OutputSelection.bBaseColorOpacity)
				{
					OutStats.SourceTexturedTriangles += MatchingTriangleCount;
				}
				if (bMaterialHasOpacityMask)
				{
					OutStats.TextureAlphaOpacityReferences += MatchingTriangleCount;
					OutStats.GpuOpacityExportReferences += MatchingTriangleCount;
					OutStats.MaskedMaterialBakeReferences += MatchingTriangleCount;
				}
				else
				{
					OutStats.ForcedOpaqueAlphaReferences += MatchingTriangleCount;
				}
				if (OutputSelection.bMix && MaterialBakeData.IsValidIndex(MaterialIndex))
				{
					const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
					if (BakeData.AmbientOcclusion.bHasReadableTexture
						|| BakeData.Roughness.bHasReadableTexture
						|| BakeData.Metallic.bHasReadableTexture
						|| BakeData.Emission.bHasReadableTexture)
					{
						OutStats.SourceMixTextureReferences += MatchingTriangleCount;
					}
				}

				FMeshData MeshSettings;
				MeshSettings.MeshDescription = &PerPlaneMesh;
				MeshSettings.Mesh = &SourceStaticMesh;
				MeshSettings.MaterialIndices.Add(0);
				MeshSettings.TextureCoordinateBox = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				MeshSettings.TextureCoordinateIndex = 0;
				MeshSettings.LightMapIndex = 0;
				MeshSettings.PrimitiveData = FPrimitiveData(SourceLODBounds);
				MeshSettings.CustomTextureCoordinates = MoveTemp(CustomTileUVs);

				TArray<FColor> MaskedBaseColorData;
				if (OutputSelection.bBaseColorOpacity)
				{
					FString MaskedBaseColorError;
					if (!FFoliageBakerMaskedMaterialBaker::BakeBaseColor(
							*MaterialInterface,
							MeshSettings,
							TileSize,
							BakeBackground,
							MaskedBaseColorData,
							&MaskedBaseColorError))
					{
						OutError = FString::Printf(
							TEXT("BillboardClouds masked BaseColor bake failed for plane %d (%s), material %d: %s"),
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"),
							MaterialIndex,
							*MaskedBaseColorError);
						return false;
					}
				}

				TArray<FColor> FinalCoverageData;
				FString FinalCoverageError;
				if (!FFoliageBakerMaskedMaterialBaker::BakeFinalCoverage(
						*MaterialInterface,
						MeshSettings,
						TileSize,
						BakeBackground,
						FinalCoverageData,
						&FinalCoverageError))
				{
					OutStats.GpuOpacityExportFailedReferences += MatchingTriangleCount;
					OutError = FString::Printf(
						TEXT("BillboardClouds FinalCoverage bake failed for plane %d (%s), material %d: %s"),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						*FinalCoverageError);
					return false;
				}

				TArray<FColor> SourceTriangleIdData;
				FString SourceTriangleIdError;
				if (!FFoliageBakerMaskedMaterialBaker::BakeSourceTriangleId(
						*MaterialInterface,
						MeshSettings,
						RasterSourceTriangleIndices,
						TileSize,
						SourceTriangleIdData,
						&SourceTriangleIdError))
				{
					OutError = FString::Printf(
						TEXT("BillboardClouds source-triangle-id bake failed for plane %d (%s), material %d: %s"),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						*SourceTriangleIdError);
					return false;
				}

				TArray<FColor> MaskedObjectNormalData;
				if (OutputSelection.bNormalMask)
				{
					FString MaskedNormalError;
					if (!FFoliageBakerMaskedMaterialBaker::BakeObjectSpaceNormal(
							*MaterialInterface,
							MeshSettings,
							TileSize,
							BakeBackground,
							MaskedObjectNormalData,
							&MaskedNormalError))
					{
						OutError = FString::Printf(
							TEXT("BillboardClouds masked object-normal bake failed for plane %d (%s), material %d: %s"),
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"),
							MaterialIndex,
							*MaskedNormalError);
						return false;
					}
				}

				if ((OutputSelection.bBaseColorOpacity && MaskedBaseColorData.Num() != TilePixelCount)
					|| FinalCoverageData.Num() != TilePixelCount
					|| SourceTriangleIdData.Num() != TilePixelCount
					|| (OutputSelection.bNormalMask && MaskedObjectNormalData.Num() != TilePixelCount))
				{
					OutError = FString::Printf(
						TEXT("BillboardClouds masked outputs returned invalid sizes for plane %d (%s), material %d: base=%d, coverage=%d, id=%d, normal=%d, expected=%d."),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						MaterialIndex,
						MaskedBaseColorData.Num(),
						FinalCoverageData.Num(),
						SourceTriangleIdData.Num(),
						MaskedObjectNormalData.Num(),
						TilePixelCount);
					return false;
				}

				FAtlasBakeStats::FBakeMaterialAgg& MaterialAgg =
					OutStats.GpuBakeDiagnostics.FindOrAdd(MaterialInterface->GetName());
				MaterialAgg.SourceBlendMode = static_cast<int32>(SourceBlendMode);
				MaterialAgg.WantsOpacity = bMaterialHasOpacityMask ? 1 : 0;
				MaterialAgg.WantsBaseColor = OutputSelection.bBaseColorOpacity ? 1 : 0;
				MaterialAgg.WantsNormal = OutputSelection.bNormalMask ? 1 : 0;
				AccumulateBakeChannel(MaterialAgg.BaseColor, OutputSelection.bBaseColorOpacity ? &MaskedBaseColorData : nullptr, BakeBackground);
				AccumulateBakeChannel(MaterialAgg.Opacity, &FinalCoverageData, BakeBackground);
				AccumulateBakeChannel(MaterialAgg.Normal, OutputSelection.bNormalMask ? &MaskedObjectNormalData : nullptr, BakeBackground);

				for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
				{
					const int32 AtlasY = TilePixelMin.Y + LocalY;
					if (AtlasY < 0 || AtlasY >= OutStats.Height)
					{
						continue;
					}
					for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
					{
						const int32 AtlasX = TilePixelMin.X + LocalX;
						if (AtlasX < 0 || AtlasX >= OutStats.Width)
						{
							continue;
						}
						const int32 TilePixelIndex = LocalY * TileSize.X + LocalX;
						const bool bHasFinalCoverage =
							FinalCoverageData[TilePixelIndex] != BakeBackground;
						const int32 SourceTriangleIndex =
							FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
								SourceTriangleIdData[TilePixelIndex]);
						const bool bHasSourceTriangleId = SourceTriangleIndex != INDEX_NONE;
						const bool bHasObjectNormal = !OutputSelection.bNormalMask
							|| MaskedObjectNormalData[TilePixelIndex] != BakeBackground;
						if (bHasFinalCoverage != bHasSourceTriangleId
							|| (bHasFinalCoverage && !bHasObjectNormal))
						{
							OutError = FString::Printf(
								TEXT("BillboardClouds masked passes disagree at pixel (%d,%d) for plane %d (%s), material %d: coverage=%d, id=%d, normal=%d."),
								LocalX,
								LocalY,
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"),
								MaterialIndex,
								bHasFinalCoverage ? 1 : 0,
								bHasSourceTriangleId ? 1 : 0,
								bHasObjectNormal ? 1 : 0);
							return false;
						}
						if (!bHasFinalCoverage)
						{
							continue;
						}
						if (!Triangles.IsValidIndex(SourceTriangleIndex)
							|| Triangles[SourceTriangleIndex].MaterialIndex != MaterialIndex)
						{
							OutError = FString::Printf(
								TEXT("BillboardClouds source-triangle-id decoded invalid triangle %d for plane %d (%s), material %d."),
								SourceTriangleIndex,
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"),
								MaterialIndex);
							return false;
						}

						const UE::FoliageBaker::PlaneCover::FSourceTriangle& SourceTriangle =
							Triangles[SourceTriangleIndex];
						FVector2D ProjectedPoints[3];
						for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
						{
							const FVector ProjectedVertex = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
								SourceTriangle.Vertices[VertexIndex],
								PlaneInfo.Normal,
								PlaneInfo.Rho);
							const double UFraction =
								(FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
							const double PlaneVFraction =
								(FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
							const double TileVFraction = bFlipTextureV ? 1.0 - PlaneVFraction : PlaneVFraction;
							ProjectedPoints[VertexIndex] = FVector2D(
								UFraction * TileSize.X,
								TileVFraction * TileSize.Y);
						}

						double W0 = 0.0;
						double W1 = 0.0;
						double W2 = 0.0;
						if (!ComputeBarycentric2D(
								FVector2D(LocalX + 0.5, LocalY + 0.5),
								ProjectedPoints[0],
								ProjectedPoints[1],
								ProjectedPoints[2],
								W0,
								W1,
								W2))
						{
							OutError = FString::Printf(
								TEXT("BillboardClouds could not reconstruct source triangle %d at covered pixel (%d,%d) for plane %d (%s), material %d."),
								SourceTriangleIndex,
								LocalX,
								LocalY,
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"),
								MaterialIndex);
							return false;
						}

						const FVector SourcePoint = SourceTriangle.Vertices[0] * W0
							+ SourceTriangle.Vertices[1] * W1
							+ SourceTriangle.Vertices[2] * W2;
						const double CaptureDepth = FVector::DotProduct(SourcePoint, CaptureRayDirection);
						if (!FMath::IsFinite(CaptureDepth)
							|| CaptureDepth > TileDepth[TilePixelIndex] + 1.0e-6)
						{
							continue;
						}
						TileDepth[TilePixelIndex] = CaptureDepth;
						const int32 AtlasPixelIndex = AtlasY * OutStats.Width + AtlasX;
						const uint8 CoverageAlpha = bMaterialHasOpacityMask
							? FinalCoverageData[TilePixelIndex].R
							: 255;

						if (OutputSelection.bBaseColorOpacity)
						{
							FColor Color = MaskedBaseColorData[TilePixelIndex];
							Color.A = CoverageAlpha;
							OutPixels[AtlasPixelIndex] = Color;
						}
						if (AtlasCoverage.IsValidIndex(AtlasPixelIndex))
						{
							AtlasCoverage[AtlasPixelIndex] = true;
						}

						if (OutputSelection.bNormalMask)
						{
							const double SignedDepth = CaptureDepth
								- FVector::DotProduct(SharedDepthCenter, CaptureRayDirection);
							const double LinearDepth = FMath::Clamp(
								(SignedDepth + SharedDepthRadius) / (2.0 * SharedDepthRadius),
								0.0,
								1.0);
							FColor ObjectNormal = MaskedObjectNormalData[TilePixelIndex];
							ObjectNormal.A = UnitFloatToByte(static_cast<float>(LinearDepth));
							OutNormalPixels[AtlasPixelIndex] = ObjectNormal;
							if (NormalCoverage.IsValidIndex(AtlasPixelIndex))
							{
								NormalCoverage[AtlasPixelIndex] = true;
							}
						}

						if (OutputSelection.bMix && MaterialBakeData.IsValidIndex(MaterialIndex))
						{
							const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
							const FVector2f SourceUV = SourceTriangle.bHasUVs
								? SourceTriangle.UVs[0] * static_cast<float>(W0)
									+ SourceTriangle.UVs[1] * static_cast<float>(W1)
									+ SourceTriangle.UVs[2] * static_cast<float>(W2)
								: FVector2f::ZeroVector;
							OutMixPixels[AtlasPixelIndex] = FColor(
								UnitFloatToByte(SampleMaterialScalar(BakeData.AmbientOcclusion, SourceUV)),
								UnitFloatToByte(SampleMaterialScalar(BakeData.Roughness, SourceUV)),
								UnitFloatToByte(SampleMaterialScalar(BakeData.Metallic, SourceUV)),
								UnitFloatToByte(SampleMaterialScalar(BakeData.Emission, SourceUV)));
						}
						++OutStats.PaintedPixels;
					}
				}
			}
			return true;
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			if (FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				continue;
			}
			if (!BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.AtlasPixelMin,
					PlaneInfo.AtlasTileSize,
					-PlaneInfo.Normal,
					false))
			{
				return false;
			}
			if (PlaneInfo.bHasBackFaceAtlas
				&& !BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal,
					true))
			{
				return false;
			}
		}

		UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
			OutPixels,
			OutStats.Width,
			OutStats.Height,
			PlaneInfos);
		if (OutputSelection.bNormalMask)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				OutNormalPixels,
				OutStats.Width,
				OutStats.Height,
				PlaneInfos,
				&NormalCoverage,
				false);
			for (int32 PixelIndex = 0; PixelIndex < OutNormalPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage.IsValidIndex(PixelIndex) || !NormalCoverage[PixelIndex])
				{
					OutNormalPixels[PixelIndex].A = 255;
				}
			}
			UE::FoliageBaker::Atlas::NormalizeEncodedObjectSpaceNormals(OutNormalPixels);
		}
		if (OutputSelection.bMix)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				OutMixPixels,
				OutStats.Width,
				OutStats.Height,
				PlaneInfos,
				&AtlasCoverage,
				true);
		}
		return true;
	}
	UTexture2D* CreateBillboardTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FString& OutputFolderName,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const FColor MipBackgroundColor,
		const TextureCompressionSettings CompressionSettings,
		const TextureGroup LODGroup,
		const bool bSRGB,
		const float AlphaCoverageThreshold,
		const FString& EmptyPixelsError,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = OutputFolderName;
		Params.AssetNamePrefix = AssetNamePrefix;
		Params.AssetNameSuffix = AssetNameSuffix;
		Params.Width = AtlasStats.Width;
		Params.Height = AtlasStats.Height;
		Params.CompressionSettings = CompressionSettings;
		Params.LODGroup = LODGroup;
		Params.bSRGB = bSRGB;
		Params.AlphaCoverageThreshold = AlphaCoverageThreshold;
		Params.MipBackgroundColor = MipBackgroundColor;
		Params.bNormalizeMipNormals = LODGroup == TEXTUREGROUP_WorldNormalMap;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			Params.MipTileRects.Add(FIntRect(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasPixelMin + PlaneInfo.AtlasTileSize));
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				Params.MipTileRects.Add(FIntRect(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasPixelMin + PlaneInfo.BackAtlasTileSize));
			}
		}
		Params.EmptyPixelsError = EmptyPixelsError;
		return FFoliageBakerAssetBuilder::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Params,
			Pixels,
			OutError);
	}

	UTexture2D* CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const float AlphaCoverageThreshold,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.BaseColorOpacityTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(0, 0, 0, 0),
			TC_BC7,
			TEXTUREGROUP_World,
			true,
			AlphaCoverageThreshold,
			TEXT("No atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.NormalTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(128, 128, 255, 255),
			TC_BC7,
			TEXTUREGROUP_WorldNormalMap,
			false,
			0.0f,
			TEXT("No normal atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.MixTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(255, 128, 0, 0),
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			0.0f,
			TEXT("No mix atlas pixels were generated."),
			OutError);
	}

	const TCHAR* GetMeshOutputModeText(const EFoliageBakerMeshAssetOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
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
		Params.SourceLODIndex = Settings.SourceLODIndex;
		Params.DesiredUVChannelCount = 3;

		Params.RebuildLODMetadataKey = FName(TEXT("FoliageBaker.BillboardCloudsLOD"));
		return Params;
	}

	struct FProxyPlaneCoverBuildData
	{
		int32 SourceLODIndex = INDEX_NONE;
		FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings KMeansSettings;
		FTrunkCardTriangleSplit TrunkSplit;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverResult BillboardResult;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
		bool bHasBillboardResult = false;
		int32 TrunkPlaneCount = 0;
	};

	struct FProxyMeshBuildData
	{
		FMeshDescription MeshDescription;
		UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats Stats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> PlaneInfos;
	};

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

	struct FProxyBatchBuildResult
	{
		bool bCancelled = false;
		FString Report;
		TArray<UObject*> CreatedAssets;
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
		return OutputSelection;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		OutData.SourceLODIndex = EditorSettings.SourceLODIndex;
		if (OutData.SourceLODIndex < 0 || OutData.SourceLODIndex >= MAX_STATIC_MESH_LODS)
		{
			OutError = FString::Printf(
				TEXT("Source LOD index %d is outside the supported range 0-%d."),
				OutData.SourceLODIndex,
				MAX_STATIC_MESH_LODS - 1);
			return false;
		}
		if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			OutData.SourceLODIndex,
			OutData.Triangles,
			OutError))
		{
			return false;
		}
		if (!ComputeSourceTriangleBounds(OutData.Triangles, OutData.SourceLODBounds))
		{
			OutError = FString::Printf(TEXT("Source LOD %d has no valid bounds."), OutData.SourceLODIndex);
			return false;
		}

		OutData.Settings = BuildSettingsForMesh(OutData.SourceLODBounds, EditorSettings);
		OutData.KMeansSettings = BuildKMeansSettings(EditorSettings);
		const int32 RequestedTrunkPlaneCount = FMath::Clamp(EditorSettings.TrunkCardPlaneCount, 2, 4);
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
		UMaterialInstanceConstant* TemplateMaterialInstance = EditorSettings.BillboardMaterialTemplate.LoadSynchronous();
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("Billboard material template instance is not set. Configure Billboard Material Template in the Billboard Clouds tool panel.");
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
				CoverData.SourceLODIndex,
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
			CoverData.SourceLODIndex,
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
			const float AlphaCoverageThreshold = TemplateMaterialInstance->GetBlendMode() == BLEND_Masked
				? TemplateMaterialInstance->GetOpacityMaskClipValue()
				: 0.0f;
			OutData.AtlasTexture = CreateAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutData.AtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				AlphaCoverageThreshold,
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
		MaterialParams.AssetNamePrefix = EditorSettings.MaterialInstanceNamePrefix;
		MaterialParams.AssetNameSuffix = EditorSettings.MaterialInstanceNameSuffix;
		MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MaterialParams.BaseColorOpacityTextureParameterName = EditorSettings.BaseColorOpacityTextureParameterName;
		MaterialParams.NormalDepthTextureParameterName = EditorSettings.NormalDepthTextureParameterName;
		MaterialParams.MixTextureParameterName = EditorSettings.MixTextureParameterName;
		MaterialParams.MissingTemplateError = TEXT("Billboard material template instance is not set. Configure Billboard Material Template in the Billboard Clouds tool panel.");
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

		FString GpuBakeDetails;
		for (const TPair<FString, FAtlasBakeStats::FBakeMaterialAgg>& It : TextureData.AtlasStats.GpuBakeDiagnostics)
		{
			auto FormatChannel = [](const TCHAR* Name, const FAtlasBakeStats::FBakeChannelAgg& Agg) -> FString
			{
				if (!Agg.bAny)
				{
					return FString::Printf(TEXT("\n      %s: <not requested>"), Name);
				}
				const int64 NonBg = Agg.TotalPixels - Agg.BackgroundPixels;
				const double MeanR = NonBg > 0 ? static_cast<double>(Agg.SumR) / static_cast<double>(NonBg) : 0.0;
				return FString::Printf(
					TEXT("\n      %s: bakes=%d total=%lld bg=%lld nonbg=%lld zero=%lld full=%lld other=%lld R:[%u..%u] avg=%.1f"),
					Name, Agg.BakeCount, Agg.TotalPixels, Agg.BackgroundPixels, NonBg,
					Agg.ZeroRgbPixels, Agg.FullWhiteRgbPixels, Agg.OtherRgbPixels,
					NonBg > 0 ? Agg.MinR : 0, NonBg > 0 ? Agg.MaxR : 0, MeanR);
			};
			const FAtlasBakeStats::FBakeMaterialAgg& Agg = It.Value;
			GpuBakeDetails += FString::Printf(
				TEXT("\n    material=%s blend=%d wantsBaseColor=%d wantsOpacity=%d wantsNormal=%d%s%s%s"),
				*It.Key, Agg.SourceBlendMode, Agg.WantsBaseColor, Agg.WantsOpacity, Agg.WantsNormal,
				*FormatChannel(TEXT("BaseColor"), Agg.BaseColor),
				*FormatChannel(TEXT("Opacity"),   Agg.Opacity),
				*FormatChannel(TEXT("Normal"),    Agg.Normal));
		}
		const FString GpuBakeDiagnosticsBlock = GpuBakeDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  gpu bake diagnostics:%s"), *GpuBakeDetails);
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

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, crack-reduction refs=%d, alpha refs=%d, masked refs=%d, final coverage refs=%d, final coverage failed refs=%d, mix refs texture=%d, forced opaque=%d, shooting=%s, resolve=GPU material bake, source masked-shader final coverage, side-aware per-plane material raster, far-to-near painter depth order\n  base/color opacity atlas: %s\n  normal/depth atlas: %s, RGB=object/local-space normal, A=shared selected-source-LOD bounds linear depth (near 0, far 1, uncovered 1); WPO disabled during material baking\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks, GPU-baked from material outputs\n  trunk/leaf mask: UV2 classification, trunk=(0,0), billboard/leaf=(1,0), trunk-white mask = 1 - UV2.x\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (copied from settings template; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision generation off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals%s"),
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
			TextureData.AtlasStats.SourceTexturedTriangles,
			TextureData.AtlasStats.FallbackTriangles,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.CrackReductionTriangleReferences,
			TextureData.AtlasStats.TextureAlphaOpacityReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			TextureData.AtlasStats.GpuOpacityExportReferences,
			TextureData.AtlasStats.GpuOpacityExportFailedReferences,
			TextureData.AtlasStats.SourceMixTextureReferences,
			TextureData.AtlasStats.ForcedOpaqueAlphaReferences,
			GetTextureShootingMode(),
			*BaseAtlasPath,
			*NormalAtlasPath,
			*MixAtlasPath,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails,
			*GpuBakeDiagnosticsBlock);
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

		FProxyTextureBuildData TextureData;
		FFoliageBakerAssetTransaction AssetTransaction;
		if (!BuildProxyTextureData(StaticMesh, EditorSettings, AssetTransaction, CoverData, MeshData, TextureData, Error))
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

	FProxyBatchBuildResult BuildBillboardCloudProxyAssetsForSelection(
		const TArray<UStaticMesh*>& StaticMeshes,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		FProxyBatchBuildResult BatchResult;

		FScopedSlowTask SlowTask(StaticMeshes.Num(), LOCTEXT("CreatePlaneProxyMeshesSlowTask", "Creating Billboard Clouds plane proxy meshes..."));
		SlowTask.MakeDialog();

		for (UStaticMesh* StaticMesh : StaticMeshes)
		{
			if (!StaticMesh)
			{
				continue;
			}

			SlowTask.EnterProgressFrame(1.0f, FText::FromString(StaticMesh->GetName()));

			const FProxyAssetBuildResult BuildResult = BuildBillboardCloudProxyAsset(*StaticMesh, EditorSettings);
			BatchResult.Report += BuildResult.Report + TEXT("\n\n");
			if (BuildResult.bSucceeded)
			{
				AppendProxyCreatedAssets(BuildResult, BatchResult.CreatedAssets);
			}
			if (BuildResult.bCancelled)
			{
				BatchResult.bCancelled = true;
				break;
			}
		}

		return BatchResult;
	}

	void SyncCreatedProxyAssetsToContentBrowser(const TArray<UObject*>& CreatedAssets)
	{
		if (!CreatedAssets.IsEmpty() && GEditor)
		{
			GEditor->SyncBrowserToObjects(CreatedAssets);
		}
	}

	void ShowNoStaticMeshSelectionMessage()
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("NoStaticMeshSelectionForProxy", "Add one or more Static Mesh assets to the Billboard Clouds tool panel before clicking Bake.")
		);
	}

	void ShowProxyBuildReport(const FString& Report)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
	}

	void RunCreatePlaneProxyMeshes(
		const TArray<UStaticMesh*>& StaticMeshes,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		const FProxyBatchBuildResult BatchResult = BuildBillboardCloudProxyAssetsForSelection(StaticMeshes, EditorSettings);
		SyncCreatedProxyAssetsToContentBrowser(BatchResult.CreatedAssets);
		ShowProxyBuildReport(BatchResult.Report);
	}
}

void FFoliageBakerBillboardCloudsModule::StartupModule()
{
	EnsureToolSettings();
}

void FFoliageBakerBillboardCloudsModule::ShutdownModule()
{
	SettingsDetailsView.Reset();
	ToolSettings.Reset();
}

void FFoliageBakerBillboardCloudsModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("FoliageBaker");
	Section.Label = LOCTEXT("FoliageBakerSection", "Foliage Baker");
	Section.AddMenuEntry(
		"BillboardCloudsTools",
		LOCTEXT("CreatePlaneProxyMeshesLabel", "Billboard Clouds"),
		LOCTEXT("CreatePlaneProxyMeshesTooltip", "Open the Billboard Clouds tool panel to configure and bake Static Mesh proxy geometry."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
		FToolMenuExecuteAction::CreateRaw(this, &FFoliageBakerBillboardCloudsModule::ExecuteCreatePlaneProxyMeshes)
	);
}

void FFoliageBakerBillboardCloudsModule::EnsureToolSettings()
{
	if (!ToolSettings.IsValid())
	{
		ToolSettings.Reset(NewObject<UFoliageBakerBillboardCloudsSettings>(GetTransientPackage(), NAME_None, RF_Transactional));
	}
}

void FFoliageBakerBillboardCloudsModule::AddContentBrowserSelectionToTool()
{
	EnsureToolSettings();
	const TArray<UStaticMesh*> SelectedStaticMeshes = GetSelectedStaticMeshes();
	if (SelectedStaticMeshes.IsEmpty())
	{
		return;
	}

	ToolSettings->Modify();
	for (UStaticMesh* StaticMesh : SelectedStaticMeshes)
	{
		ToolSettings->SourceStaticMeshes.AddUnique(StaticMesh);
	}
	ToolSettings->PostEditChange();
	if (SettingsDetailsView.IsValid())
	{
		SettingsDetailsView->ForceRefresh();
	}
}

TSharedRef<SWidget> FFoliageBakerBillboardCloudsModule::CreateFeaturePanel()
{
	EnsureToolSettings();

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bLockable = false;
	DetailsViewArgs.bShowOptions = false;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
	DetailsViewArgs.bUpdatesFromSelection = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	SettingsDetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	SettingsDetailsView->RegisterInstancedCustomPropertyLayout(
		ToolSettings->GetClass(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFoliageBakerCategoryOrderCustomization::MakeInstance));
	SettingsDetailsView->SetObject(ToolSettings.Get());

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 8.0f, 8.0f, 4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Raw(this, &FFoliageBakerBillboardCloudsModule::GetSourceMeshCountText)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("UnifiedAddSelectedMeshesButton", "Add Content Browser Selection"))
					.ToolTipText(LOCTEXT("UnifiedAddSelectedMeshesTooltip", "Add selected Static Mesh assets without removing meshes already queued."))
					.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleAddSelectedMeshes)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("UnifiedClearMeshesButton", "Clear"))
					.ToolTipText(LOCTEXT("UnifiedClearMeshesTooltip", "Remove all queued Static Mesh assets."))
					.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleClearMeshes)
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8.0f, 4.0f)
		[
			SettingsDetailsView.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UnifiedBakeRequirementsHint", "Queue at least one Static Mesh, then configure the shared bake settings below."))
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.MinDesiredWidth(200.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("UnifiedBakeBillboardCloudsButton", "Bake BillboardClouds"))
					.ToolTipText(LOCTEXT("UnifiedBakeBillboardCloudsTooltip", "Bake BillboardClouds assets for every queued Static Mesh."))
					.IsEnabled_Raw(this, &FFoliageBakerBillboardCloudsModule::CanBake)
					.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleBake)
				]
			]
		];
}

TSharedRef<SDockTab> FFoliageBakerBillboardCloudsModule::SpawnBillboardCloudsToolTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	EnsureToolSettings();

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bLockable = false;
	DetailsViewArgs.bShowOptions = false;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
	DetailsViewArgs.bUpdatesFromSelection = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	SettingsDetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	SettingsDetailsView->SetObject(ToolSettings.Get());

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 8.0f, 8.0f, 4.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(8.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Raw(this, &FFoliageBakerBillboardCloudsModule::GetSourceMeshCountText)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AddSelectedMeshesButton", "Add Content Browser Selection"))
						.ToolTipText(LOCTEXT("AddSelectedMeshesTooltip", "Add selected Static Mesh assets without removing meshes already queued."))
						.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleAddSelectedMeshes)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearMeshesButton", "Clear"))
						.ToolTipText(LOCTEXT("ClearMeshesTooltip", "Remove all queued Static Mesh assets."))
						.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleClearMeshes)
					]
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(8.0f, 4.0f)
			[
				SettingsDetailsView.ToSharedRef()
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSeparator)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f)
			.HAlign(HAlign_Right)
			[
				SNew(SBox)
				.MinDesiredWidth(120.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("BakeBillboardCloudsButton", "Bake"))
					.ToolTipText(LOCTEXT("BakeBillboardCloudsTooltip", "Bake Billboard Clouds assets for every queued Static Mesh."))
					.IsEnabled_Raw(this, &FFoliageBakerBillboardCloudsModule::CanBake)
					.OnClicked_Raw(this, &FFoliageBakerBillboardCloudsModule::HandleBake)
				]
			]
		];
}

FReply FFoliageBakerBillboardCloudsModule::HandleAddSelectedMeshes()
{
	AddContentBrowserSelectionToTool();
	return FReply::Handled();
}

FReply FFoliageBakerBillboardCloudsModule::HandleClearMeshes()
{
	EnsureToolSettings();
	ToolSettings->Modify();
	ToolSettings->SourceStaticMeshes.Reset();
	ToolSettings->PostEditChange();
	if (SettingsDetailsView.IsValid())
	{
		SettingsDetailsView->ForceRefresh();
	}
	return FReply::Handled();
}

bool FFoliageBakerBillboardCloudsModule::CanBake() const
{
	if (!ToolSettings.IsValid())
	{
		return false;
	}
	return ToolSettings->SourceStaticMeshes.ContainsByPredicate([](const TObjectPtr<UStaticMesh>& StaticMesh)
	{
		return StaticMesh != nullptr;
	});
}

FText FFoliageBakerBillboardCloudsModule::GetSourceMeshCountText() const
{
	int32 ValidMeshCount = 0;
	if (ToolSettings.IsValid())
	{
		for (const UStaticMesh* StaticMesh : ToolSettings->SourceStaticMeshes)
		{
			ValidMeshCount += StaticMesh != nullptr ? 1 : 0;
		}
	}
	return FText::Format(LOCTEXT("QueuedStaticMeshCount", "Static Meshes queued: {0}"), FText::AsNumber(ValidMeshCount));
}

FReply FFoliageBakerBillboardCloudsModule::HandleBake()
{
	EnsureToolSettings();
	TArray<UStaticMesh*> StaticMeshes;
	for (UStaticMesh* StaticMesh : ToolSettings->SourceStaticMeshes)
	{
		if (StaticMesh)
		{
			StaticMeshes.AddUnique(StaticMesh);
		}
	}

	if (StaticMeshes.IsEmpty())
	{
		ShowNoStaticMeshSelectionMessage();
		return FReply::Handled();
	}

	RunCreatePlaneProxyMeshes(StaticMeshes, *ToolSettings);
	return FReply::Handled();
}

void FFoliageBakerBillboardCloudsModule::ExecuteCreatePlaneProxyMeshes(const FToolMenuContext& MenuContext)
{
	(void)MenuContext;
	AddContentBrowserSelectionToTool();
	FGlobalTabmanager::Get()->TryInvokeTab(BillboardCloudsToolTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFoliageBakerBillboardCloudsModule, FoliageBakerBillboardClouds)
