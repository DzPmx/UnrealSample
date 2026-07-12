#include "FoliageBakerBillboardCloudsModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaterialBaker.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerKMeansPlaneCover.h"
#include "FoliageBakerPlaneCover.h"
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
#include "StaticMeshAttributes.h"
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
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(EditorSettings.AlphaAwareTileCropGuardPixels, 0, 16);
		return Settings;
	}

	UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings BuildKMeansSettings(
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings Settings;
		Settings.PlaneCount = FMath::Clamp(EditorSettings.KMeansPlaneCount, 1, 4096);
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
	using UE::FoliageBaker::MaterialResolver::SampleOpacityMaskValue;

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


	bool ComputeBarycentric2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B, const FVector2D& C, double& OutA, double& OutB, double& OutC)
	{
		const double Denominator = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		OutA = ((B.Y - C.Y) * (Point.X - C.X) + (C.X - B.X) * (Point.Y - C.Y)) / Denominator;
		OutB = ((C.Y - A.Y) * (Point.X - C.X) + (A.X - C.X) * (Point.Y - C.Y)) / Denominator;
		OutC = 1.0 - OutA - OutB;
		return OutA >= -1.0e-5 && OutB >= -1.0e-5 && OutC >= -1.0e-5;
	}

	bool ComputeBarycentric3D(const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle, const FVector& Point, double& OutA, double& OutB, double& OutC)
	{
		const FVector V0 = Triangle.Vertices[1] - Triangle.Vertices[0];
		const FVector V1 = Triangle.Vertices[2] - Triangle.Vertices[0];
		const FVector V2 = Point - Triangle.Vertices[0];
		const double D00 = FVector::DotProduct(V0, V0);
		const double D01 = FVector::DotProduct(V0, V1);
		const double D11 = FVector::DotProduct(V1, V1);
		const double D20 = FVector::DotProduct(V2, V0);
		const double D21 = FVector::DotProduct(V2, V1);
		const double Denominator = D00 * D11 - D01 * D01;
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		OutB = (D11 * D20 - D01 * D21) / Denominator;
		OutC = (D00 * D21 - D01 * D20) / Denominator;
		OutA = 1.0 - OutB - OutC;
		return true;
	}

	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	FColor EncodeObjectSpaceNormalToColor(const FVector& InNormal, const uint8 Alpha = 255)
	{
		FVector Normal = InNormal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}

		return FColor(
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.X * 0.5 + 0.5) * 255.0), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.Y * 0.5 + 0.5) * 255.0), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Normal.Z * 0.5 + 0.5) * 255.0), 0, 255)),
			Alpha);
	}

	FVector DecodeObjectSpaceNormalColor(const FColor& Color)
	{
		return FVector(
			static_cast<double>(Color.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Color.B) / 255.0 * 2.0 - 1.0).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	struct FNormalBakeBasisSample
	{
		FVector Normal = FVector::UpVector;
		FVector Tangent = FVector::ForwardVector;
		double CaptureDepth = TNumericLimits<double>::Max();
		float BinormalSign = 1.0f;
		float OutputNormalSign = 1.0f;
		bool bValid = false;
	};

	FVector DeriveTangentForNormal(const FVector& InNormal)
	{
		const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const FVector ReferenceAxis = FMath::Abs(Normal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
		FVector Tangent = FVector::CrossProduct(ReferenceAxis, Normal).GetSafeNormal();
		if (Tangent.IsNearlyZero())
		{
			Tangent = FVector::RightVector;
		}
		return Tangent;
	}

	FNormalBakeBasisSample MakeNormalBakeBasisSample(
		const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle,
		const double W0,
		const double W1,
		const double W2,
		const bool bFlipOutputNormalForTwoSidedBackFace)
	{
		FNormalBakeBasisSample Result;

		FVector Normal = Triangle.VertexNormals[0] * W0
			+ Triangle.VertexNormals[1] * W1
			+ Triangle.VertexNormals[2] * W2;
		if (!Normal.Normalize())
		{
			Normal = Triangle.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		}

		FVector Tangent = FVector::ForwardVector;
		float BinormalSign = 1.0f;
		if (Triangle.bHasTangents)
		{
			Tangent = Triangle.VertexTangents[0] * W0
				+ Triangle.VertexTangents[1] * W1
				+ Triangle.VertexTangents[2] * W2;
			BinormalSign = (Triangle.BinormalSigns[0] * W0
				+ Triangle.BinormalSigns[1] * W1
				+ Triangle.BinormalSigns[2] * W2) < 0.0 ? -1.0f : 1.0f;
		}
		else
		{
			Tangent = DeriveTangentForNormal(Normal);
		}

		Tangent = Tangent - Normal * FVector::DotProduct(Tangent, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = DeriveTangentForNormal(Normal);
		}

		Result.Normal = Normal;
		Result.Tangent = Tangent;
		Result.BinormalSign = BinormalSign;
		Result.OutputNormalSign = bFlipOutputNormalForTwoSidedBackFace ? -1.0f : 1.0f;
		Result.bValid = true;
		return Result;
	}

	FColor EncodeBakedTangentSpaceNormalToObjectSpaceColor(
		const FColor& RawBakedTangentSpaceNormal,
		const FNormalBakeBasisSample& Basis,
		const uint8 AlphaOverride)
	{
		if (!Basis.bValid)
		{
			return EncodeObjectSpaceNormalToColor(FVector::UpVector, AlphaOverride);
		}

		const FVector TangentSpaceNormal = DecodeObjectSpaceNormalColor(RawBakedTangentSpaceNormal);
		const FVector Normal = Basis.Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		FVector Tangent = Basis.Tangent - Normal * FVector::DotProduct(Basis.Tangent, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = DeriveTangentForNormal(Normal);
		}
		const FVector Binormal = FVector::CrossProduct(Normal, Tangent).GetSafeNormal() * Basis.BinormalSign;
		const FVector ObjectSpaceNormal = (Tangent * TangentSpaceNormal.X
			+ Binormal * TangentSpaceNormal.Y
			+ Normal * TangentSpaceNormal.Z).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Normal)
			* static_cast<double>(Basis.OutputNormalSign);
		return EncodeObjectSpaceNormalToColor(ObjectSpaceNormal, AlphaOverride);
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



































	bool BuildPerPlaneBakeMeshDescription(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FIntPoint& TileSize,
		const int32 MaterialIndexFilter,
		const int32 NumSourceUVChannels,
		const bool bReverseBakeWinding,
		FMeshDescription& OutMeshDescription,
		TArray<FVector2D>& OutCustomTileUVs,
		int32& OutMatchingTriangleCount)
	{
		OutMatchingTriangleCount = 0;
		OutCustomTileUVs.Reset();
		OutMeshDescription.Empty();
		FStaticMeshAttributes(OutMeshDescription).Register();



		const int32 DesiredUVChannels = FMath::Clamp(NumSourceUVChannels, 1, UE::FoliageBaker::PlaneCover::MaxMaterialBakeUVChannels);

		FStaticMeshAttributes Attributes(OutMeshDescription);
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		VertexInstanceUVs.SetNumChannels(DesiredUVChannels);

		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupMaterialSlotNames[PolygonGroupID] = FName(TEXT("BillboardBakeSlot"));

		const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
		const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);





		auto AppendTriangleGeometry = [&](
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Tri,
			const FVector Positions[3]) -> bool
		{
			if (MaterialIndexFilter != INDEX_NONE && Tri.MaterialIndex != MaterialIndexFilter)
			{
				return false;
			}
			if (Tri.Area <= 0.0)
			{
				return false;
			}
			if (FVector::CrossProduct(Positions[1] - Positions[0], Positions[2] - Positions[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return false;
			}

			double Weights[3][3] = {};
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (!ComputeBarycentric3D(Tri, Positions[Corner], Weights[Corner][0], Weights[Corner][1], Weights[Corner][2]))
				{
					return false;
				}
			}

			FVertexInstanceID VertexInstanceIDs[3];
			FVector2D CustomUVs[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const double W0 = Weights[Corner][0];
				const double W1 = Weights[Corner][1];
				const double W2 = Weights[Corner][2];
				const float W0f = static_cast<float>(W0);
				const float W1f = static_cast<float>(W1);
				const float W2f = static_cast<float>(W2);
				const FNormalBakeBasisSample Basis = MakeNormalBakeBasisSample(Tri, W0, W1, W2, false);

				const FVertexID VertexID = OutMeshDescription.CreateVertex();
				VertexPositions[VertexID] = FVector3f(Positions[Corner]);

				VertexInstanceIDs[Corner] = OutMeshDescription.CreateVertexInstance(VertexID);



				const FVector2f FallbackUV = Tri.bHasUVs
					? Tri.UVs[0] * W0f + Tri.UVs[1] * W1f + Tri.UVs[2] * W2f
					: FVector2f::ZeroVector;
				for (int32 UVChannel = 0; UVChannel < DesiredUVChannels; ++UVChannel)
				{
					const FVector2f SourceUV = (Tri.bHasUVs && UVChannel < Tri.NumUVChannels)
						? Tri.UVChannels[UVChannel][0] * W0f + Tri.UVChannels[UVChannel][1] * W1f + Tri.UVChannels[UVChannel][2] * W2f
						: FallbackUV;
					VertexInstanceUVs.Set(VertexInstanceIDs[Corner], UVChannel, SourceUV);
				}

				VertexInstanceNormals[VertexInstanceIDs[Corner]] = FVector3f(Basis.Normal);
				VertexInstanceTangents[VertexInstanceIDs[Corner]] = FVector3f(Basis.Tangent);
				VertexInstanceBinormalSigns[VertexInstanceIDs[Corner]] = Basis.BinormalSign;
				VertexInstanceColors[VertexInstanceIDs[Corner]] = Tri.bHasVertexColors
					? Tri.VertexColors[0] * W0f + Tri.VertexColors[1] * W1f + Tri.VertexColors[2] * W2f
					: FVector4f(1.0f, 1.0f, 1.0f, 1.0f);






				const FVector Projected = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
					Positions[Corner], PlaneInfo.Normal, PlaneInfo.Rho);
				const double UFrac = (FVector::DotProduct(Projected, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
				const double VFrac = (FVector::DotProduct(Projected, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
				CustomUVs[Corner] = FVector2D(UFrac, VFrac);
			}

			if (bReverseBakeWinding)
			{
				const FVertexInstanceID ReversedVertexInstanceIDs[3] =
				{
					VertexInstanceIDs[0],
					VertexInstanceIDs[2],
					VertexInstanceIDs[1]
				};
				OutCustomTileUVs.Add(CustomUVs[0]);
				OutCustomTileUVs.Add(CustomUVs[2]);
				OutCustomTileUVs.Add(CustomUVs[1]);
				OutMeshDescription.CreateTriangle(PolygonGroupID, ReversedVertexInstanceIDs);
			}
			else
			{
				OutCustomTileUVs.Add(CustomUVs[0]);
				OutCustomTileUVs.Add(CustomUVs[1]);
				OutCustomTileUVs.Add(CustomUVs[2]);
				OutMeshDescription.CreateTriangle(PolygonGroupID, VertexInstanceIDs);
			}
			return true;
		};

		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Tri = Triangles[TriangleIndex];
			const FVector Positions[3] = { Tri.Vertices[0], Tri.Vertices[1], Tri.Vertices[2] };
			if (AppendTriangleGeometry(Tri, Positions))
			{
				++OutMatchingTriangleCount;
			}
		}

		for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Tri = Triangles[Projection.TriangleIndex];
			for (int32 PolygonVertexIndex = 1; PolygonVertexIndex + 1 < Projection.ClippedPolygon.Num(); ++PolygonVertexIndex)
			{
				const FVector Positions[3] =
				{
					Projection.ClippedPolygon[0],
					Projection.ClippedPolygon[PolygonVertexIndex],
					Projection.ClippedPolygon[PolygonVertexIndex + 1]
				};
				if (AppendTriangleGeometry(Tri, Positions))
				{
					++OutMatchingTriangleCount;
				}
			}
		}

		return OutMatchingTriangleCount > 0;
	}

	bool BuildPerPlaneNormalBasisMap(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FIntPoint& TileSize,
		const int32 MaterialIndexFilter,
		const bool bFlipTwoSidedBackFaceOutputNormals,
		const FVector& CaptureRayDirection,
		const bool bReverseBakeWinding,
		TArray<FNormalBakeBasisSample>& OutBasisMap)
	{
		OutBasisMap.Reset();
		if (TileSize.X <= 0 || TileSize.Y <= 0)
		{
			return false;
		}

		OutBasisMap.Init(FNormalBakeBasisSample(), TileSize.X * TileSize.Y);

		const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
		const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);
		bool bWroteAnyPixel = false;

		auto RasterizeProjectedTriangle = [&](
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle,
			const FVector Positions[3])
		{
			if (MaterialIndexFilter != INDEX_NONE && Triangle.MaterialIndex != MaterialIndexFilter)
			{
				return;
			}
			if (Triangle.Area <= 0.0)
			{
				return;
			}
			if (FVector::CrossProduct(Positions[1] - Positions[0], Positions[2] - Positions[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return;
			}



			const bool bFlipOutputNormalForTwoSidedBackFace = bFlipTwoSidedBackFaceOutputNormals
				&& FVector::DotProduct(Triangle.Normal, CaptureRayDirection) < 0.0;

			FVector2D ProjectedPoints[3];
			double SourceWeightsAtVertex[3][3] = {};
			for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
			{
				if (!ComputeBarycentric3D(
					Triangle,
					Positions[VertexIndex],
					SourceWeightsAtVertex[VertexIndex][0],
					SourceWeightsAtVertex[VertexIndex][1],
					SourceWeightsAtVertex[VertexIndex][2]))
				{
					return;
				}

				const FVector ProjectedVertex = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
					Positions[VertexIndex],
					PlaneInfo.Normal,
					PlaneInfo.Rho);
				const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
				const double VFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
				ProjectedPoints[VertexIndex] = FVector2D(UFraction * TileSize.X, VFraction * TileSize.Y);
			}

			if (bReverseBakeWinding)
			{
				Swap(ProjectedPoints[1], ProjectedPoints[2]);
				for (int32 WeightIndex = 0; WeightIndex < 3; ++WeightIndex)
				{
					Swap(SourceWeightsAtVertex[1][WeightIndex], SourceWeightsAtVertex[2][WeightIndex]);
				}
			}

			const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
			const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
			const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);
			const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					double W0 = 0.0;
					double W1 = 0.0;
					double W2 = 0.0;
					if (!ComputeBarycentric2D(FVector2D(X + 0.5, Y + 0.5), ProjectedPoints[0], ProjectedPoints[1], ProjectedPoints[2], W0, W1, W2))
					{
						continue;
					}

					const double SourceW0 = SourceWeightsAtVertex[0][0] * W0
						+ SourceWeightsAtVertex[1][0] * W1
						+ SourceWeightsAtVertex[2][0] * W2;
					const double SourceW1 = SourceWeightsAtVertex[0][1] * W0
						+ SourceWeightsAtVertex[1][1] * W1
						+ SourceWeightsAtVertex[2][1] * W2;
					const double SourceW2 = SourceWeightsAtVertex[0][2] * W0
						+ SourceWeightsAtVertex[1][2] * W1
						+ SourceWeightsAtVertex[2][2] * W2;

					const int32 PixelIndex = Y * TileSize.X + X;
					if (!OutBasisMap.IsValidIndex(PixelIndex))
					{
						continue;
					}





					FNormalBakeBasisSample BasisSample = MakeNormalBakeBasisSample(
						Triangle,
						SourceW0,
						SourceW1,
						SourceW2,
						bFlipOutputNormalForTwoSidedBackFace);
					const FVector SourcePoint = Triangle.Vertices[0] * SourceW0
						+ Triangle.Vertices[1] * SourceW1
						+ Triangle.Vertices[2] * SourceW2;
					BasisSample.CaptureDepth = FVector::DotProduct(SourcePoint, CaptureRayDirection);
					OutBasisMap[PixelIndex] = BasisSample;
					bWroteAnyPixel = true;
				}
			}
		};

		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector Positions[3] = { Triangle.Vertices[0], Triangle.Vertices[1], Triangle.Vertices[2] };
			RasterizeProjectedTriangle(Triangle, Positions);
		}

		for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}

			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[Projection.TriangleIndex];
			for (int32 PolygonVertexIndex = 1; PolygonVertexIndex + 1 < Projection.ClippedPolygon.Num(); ++PolygonVertexIndex)
			{
				const FVector Positions[3] =
				{
					Projection.ClippedPolygon[0],
					Projection.ClippedPolygon[PolygonVertexIndex],
					Projection.ClippedPolygon[PolygonVertexIndex + 1]
				};
				RasterizeProjectedTriangle(Triangle, Positions);
			}
		}

		return bWroteAnyPixel;
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

	bool IsDepthResolvedPixelWinner(
		const TArray<FNormalBakeBasisSample>& BasisMap,
		const int32 BasisIndex,
		TArray<double>& InOutTileDepth,
		const bool bUpdateDepth)
	{
		if (BasisMap.IsEmpty() || InOutTileDepth.IsEmpty())
		{
			return true;
		}
		if (!BasisMap.IsValidIndex(BasisIndex)
			|| !BasisMap[BasisIndex].bValid
			|| !InOutTileDepth.IsValidIndex(BasisIndex))
		{
			return false;
		}

		const double Depth = BasisMap[BasisIndex].CaptureDepth;
		if (!FMath::IsFinite(Depth))
		{
			return false;
		}



		constexpr double DepthEpsilon = 1.0e-6;
		if (Depth <= InOutTileDepth[BasisIndex] + DepthEpsilon)
		{
			if (bUpdateDepth && Depth < InOutTileDepth[BasisIndex])
			{
				InOutTileDepth[BasisIndex] = Depth;
			}
			return true;
		}

		return false;
	}













	void BlitBakedBaseColorAndOpacityIntoAtlas(
		const TArray<FColor>& BakedBaseColor,
		const TArray<FColor>* BakedOpacityMask,
		const FIntPoint& BakedSize,
		const bool bBaseColorIsLinear,
		const FColor& BackgroundColor,
		const bool bMaterialHasOpacityMask,
		const float OpacityMaskClipValue,
		TArray<FColor>& AtlasPixels,
		const int32 AtlasWidth,
		const int32 AtlasHeight,
		const FIntPoint& TilePixelMin,
		const FIntPoint& TileSize,
		const TArray<FNormalBakeBasisSample>& BasisMap,
		TArray<double>& InOutTileDepth,
		TBitArray<>& OutMaterialCoverageMask,
		TBitArray<>& InOutCoverageMask,
		int32& InOutBakedClipZeroedPixels,
		int32& InOutPaintedPixels)
	{
		OutMaterialCoverageMask.Init(false, FMath::Max(0, TileSize.X * TileSize.Y));
		if (BakedBaseColor.IsEmpty() || BakedSize.X <= 0 || BakedSize.Y <= 0
			|| TileSize.X <= 0 || TileSize.Y <= 0)
		{
			return;
		}

		const int32 PadX = FMath::Max(0, (BakedSize.X - TileSize.X) / 2);
		const int32 PadY = FMath::Max(0, (BakedSize.Y - TileSize.Y) / 2);

		for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
		{
			const int32 AtlasY = TilePixelMin.Y + LocalY;
			const int32 SrcY = PadY + LocalY;
			if (AtlasY < 0 || AtlasY >= AtlasHeight || SrcY < 0 || SrcY >= BakedSize.Y)
			{
				continue;
			}
			for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
			{
				const int32 AtlasX = TilePixelMin.X + LocalX;
				const int32 SrcX = PadX + LocalX;
				if (AtlasX < 0 || AtlasX >= AtlasWidth || SrcX < 0 || SrcX >= BakedSize.X)
				{
					continue;
				}
				const int32 SrcIdx = SrcY * BakedSize.X + SrcX;
				const int32 DstIdx = AtlasY * AtlasWidth + AtlasX;
				if (!BakedBaseColor.IsValidIndex(SrcIdx))
				{
					continue;
				}

				const FColor RawBase = BakedBaseColor[SrcIdx];
				if (IsBakerBackgroundPixel(RawBase, BackgroundColor))
				{

					continue;
				}




				FColor Out;
				if (bBaseColorIsLinear)
				{
					const FLinearColor Linear(
						static_cast<float>(RawBase.R) / 255.0f,
						static_cast<float>(RawBase.G) / 255.0f,
						static_cast<float>(RawBase.B) / 255.0f,
						1.0f);
					Out = Linear.ToFColorSRGB();
				}
				else
				{
					Out = RawBase;
				}

				if (bMaterialHasOpacityMask && BakedOpacityMask && BakedOpacityMask->IsValidIndex(SrcIdx))
				{
					const FColor OpacitySample = (*BakedOpacityMask)[SrcIdx];
					if (IsBakerBackgroundPixel(OpacitySample, BackgroundColor))
					{


						Out.A = 0;
						++InOutBakedClipZeroedPixels;
					}
					else
					{

						Out.A = OpacitySample.R < UnitFloatToByte(OpacityMaskClipValue) ? 0 : OpacitySample.R;
						if (Out.A == 0)
						{
							++InOutBakedClipZeroedPixels;
						}
					}
				}
				else
				{
					Out.A = 255;
				}

				const int32 BasisIdx = LocalY * TileSize.X + LocalX;
				if (Out.A == 0)
				{
					if (BasisMap.IsEmpty()
						|| !InOutTileDepth.IsValidIndex(BasisIdx)
						|| !FMath::IsFinite(InOutTileDepth[BasisIdx]))
					{
						AtlasPixels[DstIdx] = Out;
						if (InOutCoverageMask.IsValidIndex(DstIdx))
						{
							InOutCoverageMask[DstIdx] = false;
						}
						++InOutPaintedPixels;
					}
					continue;
				}
				if (!IsDepthResolvedPixelWinner(BasisMap, BasisIdx, InOutTileDepth, true))
				{
					continue;
				}

				AtlasPixels[DstIdx] = Out;
				if (InOutCoverageMask.IsValidIndex(DstIdx))
				{
					InOutCoverageMask[DstIdx] = true;
				}
				if (OutMaterialCoverageMask.IsValidIndex(BasisIdx))
				{
					OutMaterialCoverageMask[BasisIdx] = true;
				}
				++InOutPaintedPixels;
			}
		}
	}

	bool BuildProjectedOpacityMaskFromMaterialBakeData(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
		const FIntPoint& TileSize,
		const int32 MaterialIndexFilter,
		const FMaterialBakeData& BakeData,
		const FColor& BackgroundColor,
		TArray<FColor>& OutOpacityData)
	{
		OutOpacityData.Reset();
		if (TileSize.X <= 0 || TileSize.Y <= 0
			|| !BakeData.bUseTextureAlphaAsOpacity
			|| !BakeData.bHasReadableOpacityMaskTexture
			|| !BakeData.OpacityMaskTexture.IsValid())
		{
			return false;
		}

		OutOpacityData.Init(BackgroundColor, TileSize.X * TileSize.Y);

		const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
		const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);
		const uint8 ClipByte = UnitFloatToByte(BakeData.OpacityMaskClipValue);
		int64 CoveredPixels = 0;
		int64 KeptPixels = 0;
		int64 RejectedPixels = 0;

		auto RasterizeProjectedTriangle = [&](
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle,
			const FVector Positions[3])
		{
			if (Triangle.MaterialIndex != MaterialIndexFilter || !Triangle.bHasUVs || Triangle.Area <= 0.0)
			{
				return;
			}
			if (FVector::CrossProduct(Positions[1] - Positions[0], Positions[2] - Positions[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				return;
			}

			FVector2D ProjectedPoints[3];
			FVector2f SourceUVs[3];
			for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
			{
				double W0 = 0.0;
				double W1 = 0.0;
				double W2 = 0.0;
				if (!ComputeBarycentric3D(Triangle, Positions[VertexIndex], W0, W1, W2))
				{
					return;
				}
				SourceUVs[VertexIndex] = Triangle.UVs[0] * static_cast<float>(W0)
					+ Triangle.UVs[1] * static_cast<float>(W1)
					+ Triangle.UVs[2] * static_cast<float>(W2);

				const FVector ProjectedVertex = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
					Positions[VertexIndex],
					PlaneInfo.Normal,
					PlaneInfo.Rho);
				const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
				const double VFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
				ProjectedPoints[VertexIndex] = FVector2D(UFraction * TileSize.X, VFraction * TileSize.Y);
			}

			const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
			const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), 0, TileSize.X - 1);
			const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);
			const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), 0, TileSize.Y - 1);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					double W0 = 0.0;
					double W1 = 0.0;
					double W2 = 0.0;
					if (!ComputeBarycentric2D(FVector2D(X + 0.5, Y + 0.5), ProjectedPoints[0], ProjectedPoints[1], ProjectedPoints[2], W0, W1, W2))
					{
						continue;
					}

					const FVector2f SourceUV = SourceUVs[0] * static_cast<float>(W0)
						+ SourceUVs[1] * static_cast<float>(W1)
						+ SourceUVs[2] * static_cast<float>(W2);
					const uint8 MaskByte = UnitFloatToByte(SampleOpacityMaskValue(BakeData.OpacityMaskTexture, SourceUV, BakeData.OpacityMaskChannel));
					const int32 PixelIndex = Y * TileSize.X + X;
					if (!OutOpacityData.IsValidIndex(PixelIndex))
					{
						continue;
					}

					OutOpacityData[PixelIndex] = FColor(MaskByte, MaskByte, MaskByte, 255);
					++CoveredPixels;
					if (MaskByte >= ClipByte)
					{
						++KeptPixels;
					}
					else
					{
						++RejectedPixels;
					}
				}
			}
		};

		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector Positions[3] = { Triangle.Vertices[0], Triangle.Vertices[1], Triangle.Vertices[2] };
			RasterizeProjectedTriangle(Triangle, Positions);
		}

		for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}

			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[Projection.TriangleIndex];
			for (int32 PolygonVertexIndex = 1; PolygonVertexIndex + 1 < Projection.ClippedPolygon.Num(); ++PolygonVertexIndex)
			{
				const FVector Positions[3] =
				{
					Projection.ClippedPolygon[0],
					Projection.ClippedPolygon[PolygonVertexIndex],
					Projection.ClippedPolygon[PolygonVertexIndex + 1]
				};
				RasterizeProjectedTriangle(Triangle, Positions);
			}
		}

		constexpr double MinimumUsefulOpacityMaskTransparentRatio = 0.001;
		return CoveredPixels > 0
			&& (KeptPixels > 0 || RejectedPixels > 0)
			&& BakeData.OpacityMaskTransparentRatio >= MinimumUsefulOpacityMaskTransparentRatio;
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

	double ComputeTriangleCaptureDepth(
		const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle,
		const FVector& CaptureRayDirection)
	{
		const FVector Center = (Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
		return FVector::DotProduct(Center, CaptureRayDirection);
	}

	double ComputeProjectionCaptureDepth(
		const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const FVector& CaptureRayDirection)
	{
		if (!Projection.ClippedPolygon.IsEmpty())
		{
			FVector Center = FVector::ZeroVector;
			for (const FVector& Point : Projection.ClippedPolygon)
			{
				Center += Point;
			}
			Center /= static_cast<double>(Projection.ClippedPolygon.Num());
			return FVector::DotProduct(Center, CaptureRayDirection);
		}

		return Triangles.IsValidIndex(Projection.TriangleIndex)
			? ComputeTriangleCaptureDepth(Triangles[Projection.TriangleIndex], CaptureRayDirection)
			: TNumericLimits<double>::Lowest();
	}

	void SortBakeFragmentsFarToNear(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const FVector& CaptureRayDirection,
		TArray<int32>& TriangleIndices,
		TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections)
	{



		TriangleIndices.Sort(
			[&Triangles, &CaptureRayDirection](const int32 A, const int32 B)
			{
				const double DepthA = Triangles.IsValidIndex(A)
					? ComputeTriangleCaptureDepth(Triangles[A], CaptureRayDirection)
					: TNumericLimits<double>::Lowest();
				const double DepthB = Triangles.IsValidIndex(B)
					? ComputeTriangleCaptureDepth(Triangles[B], CaptureRayDirection)
					: TNumericLimits<double>::Lowest();
				if (!FMath::IsNearlyEqual(DepthA, DepthB, 1.0e-6))
				{
					return DepthA > DepthB;
				}
				return A < B;
			});

		CrackReductionProjections.Sort(
			[&Triangles, &CaptureRayDirection](const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& A, const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& B)
			{
				const double DepthA = ComputeProjectionCaptureDepth(A, Triangles, CaptureRayDirection);
				const double DepthB = ComputeProjectionCaptureDepth(B, Triangles, CaptureRayDirection);
				if (!FMath::IsNearlyEqual(DepthA, DepthB, 1.0e-6))
				{
					return DepthA > DepthB;
				}
				return A.TriangleIndex < B.TriangleIndex;
			});
	}

	void BakeBillboardAtlasGPU(
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
		FAtlasBakeStats& OutStats)
	{
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;
		OutStats.TilePaddingPixels = ProxyStats.AtlasTilePaddingPixels;

		OutPixels.Init(FColor(0, 0, 0, 0), OutStats.Width * OutStats.Height);
		if (OutputSelection.bNormalMask)
		{
			OutNormalPixels.Init(EncodeObjectSpaceNormalToColor(FVector::UpVector, 255), OutStats.Width * OutStats.Height);
		}
		else
		{
			OutNormalPixels.Reset();
		}
		if (OutputSelection.bMix)
		{
			OutMixPixels.Init(FColor(255, 128, 0, 0), OutStats.Width * OutStats.Height);
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
			const int64 PaddedWidth = static_cast<int64>(TileSize.X + Padding * 2);
			const int64 PaddedHeight = static_cast<int64>(TileSize.Y + Padding * 2);
			PackedPaddedTilePixels += PaddedWidth * PaddedHeight;
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
		const int64 AtlasPixelCount = static_cast<int64>(OutStats.Width) * static_cast<int64>(OutStats.Height);
		OutStats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, OutPixels.Num());
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, OutPixels.Num());
		const FVector SharedDepthCenter = SourceLODBounds.Origin;
		const double SharedDepthRadius = FMath::Max(
			static_cast<double>(SourceLODBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);

		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		const int32 NumSourceUVChannels = GetSourceMeshMaxUVChannelCount(Triangles);
		const TArray<FMaterialBakeData> MaterialBakeData = UE::FoliageBaker::MaterialResolver::ResolveMaterialBakeData(
			SourceStaticMesh,
			SourceLODIndex,
			SourceLODBounds,
			Triangles,
			OutputSelection,
			Settings.SourceMaterialBakeResolution,
			false,
			OutStats);

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bReverseBakeWinding)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
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
				return;
			}

			SortBakeFragmentsFarToNear(
				Triangles,
				CaptureRayDirection,
				PrimaryTriangleIndices,
				CrackReductionProjectionsToBake);

			TArray<double> TileDepth;
			TileDepth.Init(TNumericLimits<double>::Max(), TileSize.X * TileSize.Y);


			const TArray<int32> MaterialIndicesUsed = CollectReferencedMaterialIndices(Triangles, PrimaryTriangleIndices, CrackReductionProjectionsToBake);


			const FIntPoint BakeRTSize = TileSize;

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
				const bool bMaterialHasOpacityMask = (SourceBlendMode == BLEND_Masked)
					|| MaterialInterface->IsPropertyActive(MP_OpacityMask);
				const float SourceOpacityMaskClipValue = MaterialInterface->GetOpacityMaskClipValue();
				const bool bSourceMaterialTwoSided = MaterialInterface->IsTwoSided();
				const bool bWantsBaseColor = OutputSelection.bBaseColorOpacity;
				const bool bWantsOpacity = OutputSelection.bBaseColorOpacity && bMaterialHasOpacityMask;
				const bool bWantsNormal = OutputSelection.bNormalMask;
				const bool bWantsAO = OutputSelection.bMix && MaterialInterface->IsPropertyActive(MP_AmbientOcclusion);
				const bool bWantsRoughness = OutputSelection.bMix && MaterialInterface->IsPropertyActive(MP_Roughness);
				const bool bWantsMetallic = OutputSelection.bMix && MaterialInterface->IsPropertyActive(MP_Metallic);
				const bool bWantsEmission = OutputSelection.bMix && MaterialInterface->IsPropertyActive(MP_EmissiveColor);
				const UMaterial* SourceMaterial = MaterialInterface->GetMaterial();
				const bool bSourceMaterialTangentSpaceNormal = !SourceMaterial || SourceMaterial->bTangentSpaceNormal;
				const bool bFlipTwoSidedBackFaceOutputNormals = bSourceMaterialTwoSided && bSourceMaterialTangentSpaceNormal;

				FMeshDescription PerPlaneMesh;
				TArray<FVector2D> CustomTileUVs;
				int32 MatchingTriangleCount = 0;
				if (!BuildPerPlaneBakeMeshDescription(
						Triangles, PrimaryTriangleIndices, CrackReductionProjectionsToBake, PlaneInfo, TileSize,
						MaterialIndex, NumSourceUVChannels, bReverseBakeWinding,
						PerPlaneMesh, CustomTileUVs, MatchingTriangleCount)
					|| MatchingTriangleCount == 0)
				{
					continue;
				}

				OutStats.SourceTexturedTriangles += MatchingTriangleCount;
				OutStats.RasterizedTriangleReferences += MatchingTriangleCount;

				TArray<FNormalBakeBasisSample> NormalBasisMap;
				const bool bHasNormalBasisMap = BuildPerPlaneNormalBasisMap(
					Triangles,
					PrimaryTriangleIndices,
					CrackReductionProjectionsToBake,
					PlaneInfo,
					TileSize,
					MaterialIndex,
					bFlipTwoSidedBackFaceOutputNormals,
					CaptureRayDirection,
					bReverseBakeWinding,
					NormalBasisMap);
				if (!bHasNormalBasisMap)
				{
					NormalBasisMap.Reset();
				}







				const FColor BakeBackground = FColor::Magenta;

				FMaterialData MaterialSettings;
				MaterialSettings.Material = MaterialInterface;




				MaterialSettings.BlendMode = BLEND_Opaque;
				MaterialSettings.bPerformBorderSmear = false;
				MaterialSettings.bPerformShrinking = false;



				MaterialSettings.bTangentSpaceNormal = true;
				MaterialSettings.BackgroundColor = BakeBackground;

				if (bWantsBaseColor)      { MaterialSettings.PropertySizes.Add(MP_BaseColor,        BakeRTSize); }
				if (bWantsOpacity)        { MaterialSettings.PropertySizes.Add(MP_OpacityMask,      BakeRTSize); }
				if (bWantsNormal)         { MaterialSettings.PropertySizes.Add(MP_Normal,           BakeRTSize); }
				if (bWantsAO)             { MaterialSettings.PropertySizes.Add(MP_AmbientOcclusion, BakeRTSize); }
				if (bWantsRoughness)      { MaterialSettings.PropertySizes.Add(MP_Roughness,        BakeRTSize); }
				if (bWantsMetallic)       { MaterialSettings.PropertySizes.Add(MP_Metallic,         BakeRTSize); }
				if (bWantsEmission)       { MaterialSettings.PropertySizes.Add(MP_EmissiveColor,    BakeRTSize); }
				if (MaterialSettings.PropertySizes.IsEmpty())
				{
					continue;
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

				TArray<FMaterialData*> MaterialSettingPtrs;
				MaterialSettingPtrs.Add(&MaterialSettings);
				TArray<FMeshData*> MeshSettingPtrs;
				MeshSettingPtrs.Add(&MeshSettings);

				TArray<FBakeOutput> BakeOutputs;
				if (!FFoliageBakerMaterialBaker::BakeMaterials(MaterialSettingPtrs, MeshSettingPtrs, BakeOutputs))
				{
					continue;
				}
				const FBakeOutput& BakeOutput = BakeOutputs[0];

				const TArray<FColor>* BaseColorData = BakeOutput.PropertyData.Find(MP_BaseColor);
				const TArray<FColor>* OpacityData = BakeOutput.PropertyData.Find(MP_OpacityMask);
				const TArray<FColor>* NormalData = BakeOutput.PropertyData.Find(MP_Normal);
				const TArray<FColor>* AOData = BakeOutput.PropertyData.Find(MP_AmbientOcclusion);
				const TArray<FColor>* RoughData = BakeOutput.PropertyData.Find(MP_Roughness);
				const TArray<FColor>* MetallicData = BakeOutput.PropertyData.Find(MP_Metallic);
				const TArray<FColor>* EmissionData = BakeOutput.PropertyData.Find(MP_EmissiveColor);


				auto IsOpacityBakeUsableForBaseCoverage = [&](
					const TArray<FColor>* CandidateOpacityData) -> bool
				{
					if (!BaseColorData || BaseColorData->IsEmpty()
						|| !CandidateOpacityData || CandidateOpacityData->IsEmpty()
						|| CandidateOpacityData->Num() != BaseColorData->Num())
					{
						return false;
					}

					int64 BaseCoveredPixels = 0;
					int64 OpacityCoveredPixels = 0;
					for (int32 SampleIndex = 0; SampleIndex < BaseColorData->Num(); ++SampleIndex)
					{
						if (IsBakerBackgroundPixel((*BaseColorData)[SampleIndex], BakeBackground))
						{
							continue;
						}
						++BaseCoveredPixels;
						const FColor& OpacitySample = (*CandidateOpacityData)[SampleIndex];
						if (!IsBakerBackgroundPixel(OpacitySample, BakeBackground))
						{
							++OpacityCoveredPixels;
						}
					}
					return BaseCoveredPixels > 0 && OpacityCoveredPixels > 0;
				};
				const bool bOpacityMaskBakeSucceeded = bWantsOpacity && IsOpacityBakeUsableForBaseCoverage(OpacityData);
				const FMaterialBakeData* FallbackOpacityBakeData = MaterialBakeData.IsValidIndex(MaterialIndex)
					? &MaterialBakeData[MaterialIndex]
					: nullptr;
				TArray<FColor> ProjectedOpacityData;
				const bool bProjectedOpacityMaskSucceeded = bWantsOpacity
					&& !bOpacityMaskBakeSucceeded
					&& FallbackOpacityBakeData
					&& BuildProjectedOpacityMaskFromMaterialBakeData(
						Triangles,
						PrimaryTriangleIndices,
						CrackReductionProjectionsToBake,
						PlaneInfo,
						TileSize,
						MaterialIndex,
						*FallbackOpacityBakeData,
						BakeBackground,
						ProjectedOpacityData);
				const TArray<FColor>* EffectiveOpacityData = bOpacityMaskBakeSucceeded
					? OpacityData
					: (bProjectedOpacityMaskSucceeded ? &ProjectedOpacityData : nullptr);
				const bool bEffectiveOpacityMaskSucceeded = bOpacityMaskBakeSucceeded || bProjectedOpacityMaskSucceeded;





				auto AccumulateBakeChannel = [&BakeBackground](FAtlasBakeStats::FBakeChannelAgg& Agg, const TArray<FColor>* Data)
				{
					if (!Data || Data->IsEmpty())
					{
						return;
					}
					Agg.bAny = true;
					++Agg.BakeCount;
					for (const FColor& C : *Data)
					{
						++Agg.TotalPixels;
						if (IsBakerBackgroundPixel(C, BakeBackground))
						{
							++Agg.BackgroundPixels;
							continue;
						}
						Agg.SumR += C.R;
						if (C.R < Agg.MinR) Agg.MinR = C.R;
						if (C.R > Agg.MaxR) Agg.MaxR = C.R;
						if (C.R == 0 && C.G == 0 && C.B == 0)                { ++Agg.ZeroRgbPixels; }
						else if (C.R == 255 && C.G == 255 && C.B == 255)     { ++Agg.FullWhiteRgbPixels; }
						else                                                  { ++Agg.OtherRgbPixels; }
					}
				};
				FAtlasBakeStats::FBakeMaterialAgg& MaterialAgg = OutStats.GpuBakeDiagnostics.FindOrAdd(MaterialInterface->GetName());
				MaterialAgg.SourceBlendMode = static_cast<int32>(SourceBlendMode);
				MaterialAgg.WantsOpacity = bWantsOpacity ? 1 : 0;
				MaterialAgg.WantsBaseColor = bWantsBaseColor ? 1 : 0;
				MaterialAgg.WantsNormal = bWantsNormal ? 1 : 0;
				AccumulateBakeChannel(MaterialAgg.BaseColor, BaseColorData);
				AccumulateBakeChannel(MaterialAgg.Opacity, EffectiveOpacityData ? EffectiveOpacityData : OpacityData);
				AccumulateBakeChannel(MaterialAgg.Normal, NormalData);

				const FIntPoint* BakeSizePtr = BakeOutput.PropertySizes.Find(MP_BaseColor);
				if (!BakeSizePtr && NormalData)
				{
					BakeSizePtr = BakeOutput.PropertySizes.Find(MP_Normal);
				}
				if (!BakeSizePtr && AOData)     { BakeSizePtr = BakeOutput.PropertySizes.Find(MP_AmbientOcclusion); }
				if (!BakeSizePtr)               { continue; }
				const FIntPoint BakeSize = *BakeSizePtr;

				const bool bBaseColorLinear = BakeOutput.PropertyIsLinearColor.FindRef(MP_BaseColor);
				const bool bBaseColorWritesDepth = BaseColorData && !BaseColorData->IsEmpty();


				TBitArray<> MaterialCoverageMask;
				if (BaseColorData && !BaseColorData->IsEmpty())
				{
					int32 PaintedThisTile = 0;
					int32 BakedClipZeroedThisTile = 0;
					BlitBakedBaseColorAndOpacityIntoAtlas(
						*BaseColorData,
						bWantsOpacity ? EffectiveOpacityData : nullptr,
						BakeSize,
						bBaseColorLinear,
						BakeBackground,
						bWantsOpacity && bEffectiveOpacityMaskSucceeded,
						SourceOpacityMaskClipValue,
						OutPixels,
						OutStats.Width, OutStats.Height,
						TilePixelMin, TileSize,
						NormalBasisMap,
						TileDepth,
						MaterialCoverageMask,
						AtlasCoverage,
						BakedClipZeroedThisTile,
						PaintedThisTile);
					OutStats.PaintedPixels += PaintedThisTile;
					OutStats.BakedOpacityClipZeroedPixels += BakedClipZeroedThisTile;
					if (bWantsOpacity)
					{
						OutStats.TextureAlphaOpacityReferences += MatchingTriangleCount;
						if (bEffectiveOpacityMaskSucceeded)
						{
							OutStats.GpuOpacityExportReferences += MatchingTriangleCount;
						}
						else
						{
							OutStats.GpuOpacityExportFailedReferences += MatchingTriangleCount;
						}
						if (SourceBlendMode == BLEND_Masked)
						{
							OutStats.MaskedMaterialBakeReferences += MatchingTriangleCount;
						}
					}
					else
					{
						OutStats.ForcedOpaqueAlphaReferences += MatchingTriangleCount;
					}
				}


				if (bWantsNormal && bHasNormalBasisMap && NormalData && !NormalData->IsEmpty())
				{
					for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
					{
						const int32 AtlasY = TilePixelMin.Y + LocalY;
						const int32 SrcY = FMath::Max(0, (BakeSize.Y - TileSize.Y) / 2) + LocalY;
						if (AtlasY < 0 || AtlasY >= OutStats.Height || SrcY < 0 || SrcY >= BakeSize.Y)
						{
							continue;
						}
						for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
						{
							const int32 AtlasX = TilePixelMin.X + LocalX;
							const int32 SrcX = FMath::Max(0, (BakeSize.X - TileSize.X) / 2) + LocalX;
							if (AtlasX < 0 || AtlasX >= OutStats.Width || SrcX < 0 || SrcX >= BakeSize.X)
							{
								continue;
							}
							const int32 SrcIdx = SrcY * BakeSize.X + SrcX;
							const int32 DstIdx = AtlasY * OutStats.Width + AtlasX;
							if (!NormalData->IsValidIndex(SrcIdx))
							{
								continue;
							}
							const FColor RawNormal = (*NormalData)[SrcIdx];

							if (IsBakerBackgroundPixel(RawNormal, BakeBackground))
							{
								continue;
							}
							const int32 BasisIdx = LocalY * TileSize.X + LocalX;


							if (bBaseColorWritesDepth
								&& (!MaterialCoverageMask.IsValidIndex(BasisIdx) || !MaterialCoverageMask[BasisIdx]))
							{
								continue;
							}
							if (!NormalBasisMap.IsValidIndex(BasisIdx) || !NormalBasisMap[BasisIdx].bValid)
							{
								continue;
							}
							if (!IsDepthResolvedPixelWinner(NormalBasisMap, BasisIdx, TileDepth, !bBaseColorWritesDepth))
							{
								continue;
							}
							const uint8 CoverageAlpha = (bBaseColorWritesDepth && OutPixels.IsValidIndex(DstIdx))
								? OutPixels[DstIdx].A
								: 255;
							if (CoverageAlpha == 0)
							{
								continue;
							}
							const double SignedDepth = NormalBasisMap[BasisIdx].CaptureDepth
								- FVector::DotProduct(SharedDepthCenter, CaptureRayDirection);
							const double LinearDepth = FMath::Clamp(
								(SignedDepth + SharedDepthRadius) / (2.0 * SharedDepthRadius),
								0.0,
								1.0);
							const uint8 EncodedDepth = static_cast<uint8>(FMath::RoundToInt(LinearDepth * 255.0));
							OutNormalPixels[DstIdx] = EncodeBakedTangentSpaceNormalToObjectSpaceColor(
								RawNormal,
								NormalBasisMap[BasisIdx],
								EncodedDepth);
							if (NormalCoverage.IsValidIndex(DstIdx))
							{
								NormalCoverage[DstIdx] = true;
							}
						}
					}
				}


				if (OutputSelection.bMix)
				{
					const bool bAnyMix = (AOData && !AOData->IsEmpty())
						|| (RoughData && !RoughData->IsEmpty())
						|| (MetallicData && !MetallicData->IsEmpty())
						|| (EmissionData && !EmissionData->IsEmpty());
					if (bAnyMix)
					{
						OutStats.SourceMixTextureReferences += MatchingTriangleCount;
						for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
						{
							const int32 AtlasY = TilePixelMin.Y + LocalY;
							const int32 SrcY = FMath::Max(0, (BakeSize.Y - TileSize.Y) / 2) + LocalY;
							if (AtlasY < 0 || AtlasY >= OutStats.Height || SrcY < 0 || SrcY >= BakeSize.Y)
							{
								continue;
							}
							for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
							{
								const int32 AtlasX = TilePixelMin.X + LocalX;
								const int32 SrcX = FMath::Max(0, (BakeSize.X - TileSize.X) / 2) + LocalX;
								if (AtlasX < 0 || AtlasX >= OutStats.Width || SrcX < 0 || SrcX >= BakeSize.X)
								{
									continue;
								}
								const int32 SrcIdx = SrcY * BakeSize.X + SrcX;
								const int32 DstIdx = AtlasY * OutStats.Width + AtlasX;
								const int32 BasisIdx = LocalY * TileSize.X + LocalX;
								if (bBaseColorWritesDepth
									&& (!MaterialCoverageMask.IsValidIndex(BasisIdx) || !MaterialCoverageMask[BasisIdx]))
								{
									continue;
								}
								if (!IsDepthResolvedPixelWinner(NormalBasisMap, BasisIdx, TileDepth, !bBaseColorWritesDepth))
								{
									continue;
								}

								const bool bAOBg = !AOData || !AOData->IsValidIndex(SrcIdx)
									|| IsBakerBackgroundPixel((*AOData)[SrcIdx], BakeBackground);
								const bool bRoughBg = !RoughData || !RoughData->IsValidIndex(SrcIdx)
									|| IsBakerBackgroundPixel((*RoughData)[SrcIdx], BakeBackground);
								const bool bMetalBg = !MetallicData || !MetallicData->IsValidIndex(SrcIdx)
									|| IsBakerBackgroundPixel((*MetallicData)[SrcIdx], BakeBackground);
								const bool bEmBg = !EmissionData || !EmissionData->IsValidIndex(SrcIdx)
									|| IsBakerBackgroundPixel((*EmissionData)[SrcIdx], BakeBackground);
								if (bAOBg && bRoughBg && bMetalBg && bEmBg)
								{
									continue;
								}
								auto SampleR = [SrcIdx, &BakeBackground](const TArray<FColor>* Data, uint8 Default) -> uint8
								{
									if (Data && Data->IsValidIndex(SrcIdx))
									{
										const FColor& C = (*Data)[SrcIdx];
										if (!IsBakerBackgroundPixel(C, BakeBackground))
										{
											return C.R;
										}
									}
									return Default;
								};
								auto SampleLuminance = [SrcIdx, &BakeBackground](const TArray<FColor>* Data, uint8 Default) -> uint8
								{
									if (Data && Data->IsValidIndex(SrcIdx))
									{
										const FColor& C = (*Data)[SrcIdx];
										if (!IsBakerBackgroundPixel(C, BakeBackground))
										{
											return static_cast<uint8>(FMath::Max3(C.R, C.G, C.B));
										}
									}
									return Default;
								};
								FColor& Mix = OutMixPixels[DstIdx];
								Mix.R = SampleR(AOData, 255);
								Mix.G = SampleR(RoughData, 128);
								Mix.B = SampleR(MetallicData, 0);
								Mix.A = SampleLuminance(EmissionData, 0);
							}
						}
					}
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			if (FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				continue;
			}

			BakePlaneAndSide(
				PlaneInfo,
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				-PlaneInfo.Normal,
				false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal,
					true);
			}
		}



		UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);
		if (OutputSelection.bNormalMask)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutNormalPixels, OutStats.Width, OutStats.Height, PlaneInfos, &NormalCoverage, false);
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
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(OutMixPixels, OutStats.Width, OutStats.Height, PlaneInfos, &AtlasCoverage, true);
		}
	}

	UTexture2D* CreateBillboardTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FString& OutputFolderName,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
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
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			0.0f,
			TEXT("No mix atlas pixels were generated."),
			OutError);
	}

	const TCHAR* GetMeshOutputModeText(const EBillboardCloudsMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EBillboardCloudsMeshOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
		case EBillboardCloudsMeshOutputMode::ReplaceSourceMeshLOD:
			return TEXT("replaced source mesh LOD");
		case EBillboardCloudsMeshOutputMode::SeparateMeshAsset:
		default:
			return TEXT("separate mesh asset");
		}
	}

	EFoliageBakerMeshAssetOutputMode ToAssetOutputMode(const EBillboardCloudsMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EBillboardCloudsMeshOutputMode::AddToSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD;
		case EBillboardCloudsMeshOutputMode::ReplaceSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD;
		case EBillboardCloudsMeshOutputMode::SeparateMeshAsset:
		default:
			return EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		}
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(const UFoliageBakerBillboardCloudsSettings& Settings)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = ToAssetOutputMode(Settings.MeshOutputMode);
		Params.RequestedReplaceLODIndex = Settings.ReplaceSourceLODIndex;
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
		FString Report;
		UStaticMesh* ProxyMesh = nullptr;
		EBillboardCloudsMeshOutputMode MeshOutputMode = EBillboardCloudsMeshOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	struct FProxyBatchBuildResult
	{
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
			BakeBillboardAtlasGPU(
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
				CropStats);

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

		BakeBillboardAtlasGPU(
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
			OutData.AtlasStats);
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
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = EditorSettings.MeshOutputMode;

		if (EditorSettings.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset)
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
				BuildSourceLODAssetParams(EditorSettings),
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
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/Opacity=%s, Normal/Depth=%s, Mix=%s"),
			*EditorSettings.BaseColorOpacityTextureParameterName.ToString(),
			*EditorSettings.NormalDepthTextureParameterName.ToString(),
			*EditorSettings.MixTextureParameterName.ToString());

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, readable material textures=%d, mix-texture materials=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, crack-reduction refs=%d, alpha refs=%d, masked refs=%d, gpu opacity mask refs=%d, gpu opacity mask failed refs=%d, gpu opacity clip zeroed pixels=%d, mix refs texture=%d, forced opaque=%d, shooting=%s, resolve=GPU material bake, side-aware per-plane material raster, far-to-near painter depth order\n  base/color opacity atlas: %s\n  normal/depth atlas: %s, RGB=object/local-space normal, A=shared selected-source-LOD bounds linear depth (near 0, far 1, uncovered 1); WPO disabled during material baking\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks, GPU-baked from material outputs\n  trunk/leaf mask: UV2 classification, trunk=(0,0), billboard/leaf=(1,0), trunk-white mask = 1 - UV2.x\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (copied from settings template; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision generation off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals%s"),
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
			TextureData.AtlasStats.ReadableMaterialTextures,
			TextureData.AtlasStats.SourceMixTextureMaterials,
			TextureData.AtlasStats.TextureAlphaOpacityMaterials,
			TextureData.AtlasStats.SourceTexturedTriangles,
			TextureData.AtlasStats.FallbackTriangles,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.CrackReductionTriangleReferences,
			TextureData.AtlasStats.TextureAlphaOpacityReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			TextureData.AtlasStats.GpuOpacityExportReferences,
			TextureData.AtlasStats.GpuOpacityExportFailedReferences,
			TextureData.AtlasStats.BakedOpacityClipZeroedPixels,
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
		if (EditorSettings.MeshOutputMode != EBillboardCloudsMeshOutputMode::SeparateMeshAsset
			&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
				StaticMesh,
				BuildSourceLODAssetParams(EditorSettings),
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
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

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(StaticMesh, EditorSettings, AssetTransaction, MeshData, TextureData, Result, Error))
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
