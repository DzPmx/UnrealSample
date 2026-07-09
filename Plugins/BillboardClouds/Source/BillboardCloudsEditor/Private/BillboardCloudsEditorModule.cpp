#include "BillboardCloudsEditorModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "AssetToolsModule.h"
#include "BillboardCloudsEditorSettings.h"
#include "BillboardCloudsPlaneCover.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "IMaterialBakingModule.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "MaterialShared.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "MeshDescription.h"
#include "MaterialBakingStructures.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FBillboardCloudsEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogBillboardCloudsEditor, Log, All);

namespace
{
	const FName BillboardProxyMaterialSlotName(TEXT("BillboardProxy"));

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

	UE::BillboardClouds::FPlaneCoverSettings BuildSettingsForMesh(const UStaticMesh& StaticMesh, const UBillboardCloudsEditorSettings& EditorSettings)
	{
		UE::BillboardClouds::FPlaneCoverSettings Settings;
		switch (EditorSettings.Technique)
		{
		case EBillboardCloudsTechnique::KMeansClustering:
			Settings.Technique = UE::BillboardClouds::EPlaneCoverTechnique::KMeansClustering;
			break;
		case EBillboardCloudsTechnique::GodOfWarCards:
			Settings.Technique = UE::BillboardClouds::EPlaneCoverTechnique::GodOfWarCards;
			break;
		case EBillboardCloudsTechnique::PlaneSpaceGreedy:
		default:
			Settings.Technique = UE::BillboardClouds::EPlaneCoverTechnique::PlaneSpaceGreedy;
			break;
		}
		Settings.NormalThetaSteps = FMath::Max(4, EditorSettings.NormalThetaSteps);
		Settings.NormalPhiSteps = FMath::Max(3, EditorSettings.NormalPhiSteps);
		Settings.RhoBinCount = FMath::Max(8, EditorSettings.RhoBinCount);
		Settings.AdaptiveRefinementDepth = FMath::Clamp(EditorSettings.AdaptiveRefinementDepth, 0, 16);
		Settings.KMeansPlaneCount = FMath::Clamp(EditorSettings.KMeansPlaneCount, 1, 4096);
		Settings.KMeansMaxIterations = FMath::Clamp(EditorSettings.KMeansMaxIterations, 1, 512);
		switch (EditorSettings.KMeansCrackReductionMode)
		{
		case EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip:
			Settings.KMeansCrackReductionMode = UE::BillboardClouds::EKMeansCrackReductionMode::ScaledEnvelopeClip;
			break;
		case EBillboardCloudsCrackReductionMode::Off:
		default:
			Settings.KMeansCrackReductionMode = UE::BillboardClouds::EKMeansCrackReductionMode::Off;
			break;
		}
		Settings.KMeansCrackReductionProjectionScale = FMath::Clamp(EditorSettings.KMeansCrackReductionProjectionScale, 0.0, 1.0);
		Settings.GodOfWarGeodesicSubdivisions = FMath::Clamp(EditorSettings.GodOfWarGeodesicSubdivisions, 0, 5);
		Settings.GodOfWarCandidateSpacingMultiplier = FMath::Clamp(EditorSettings.GodOfWarCandidateSpacingMultiplier, 0.1, 8.0);
		Settings.TextureTilePaddingPixels = FMath::Clamp(EditorSettings.TextureTilePaddingPixels, 0, 128);
		Settings.TextureAtlasResolution = FMath::Clamp(EditorSettings.TextureAtlasResolution, 256, 8192);
		Settings.SourceMaterialBakeResolution = FMath::Clamp(EditorSettings.SourceMaterialBakeResolution, 256, 8192);
		switch (EditorSettings.DoubleSidedBakeMode)
		{
		case EBillboardCloudsDoubleSidedBakeMode::TrunkCardsOnly:
			Settings.DoubleSidedBakeMode = UE::BillboardClouds::EDoubleSidedBakeMode::TrunkCardsOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::BillboardPlanesOnly:
			Settings.DoubleSidedBakeMode = UE::BillboardClouds::EDoubleSidedBakeMode::BillboardPlanesOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::AllPlanes:
			Settings.DoubleSidedBakeMode = UE::BillboardClouds::EDoubleSidedBakeMode::AllPlanes;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::Off:
		default:
			Settings.DoubleSidedBakeMode = UE::BillboardClouds::EDoubleSidedBakeMode::Off;
			break;
		}
		Settings.TextureCompactnessWeight = FMath::Clamp(EditorSettings.TextureCompactnessWeight, 0.0, 8.0);
		Settings.ErrorTolerance = FMath::Max(
			FMath::Max(0.0, EditorSettings.MinimumErrorCm),
			StaticMesh.GetBounds().SphereRadius * FMath::Max(0.0, EditorSettings.RelativeError));
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

	struct FTrunkCardTriangleSplit
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<UE::BillboardClouds::FSourceTriangle> BillboardTriangles;
		TArray<int32> BillboardToSourceTriangleIndices;
		TArray<int32> TrunkTriangleIndices;
	};

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

	bool DoesMaterialOrParentNameMatchKeywords(const UMaterialInterface* MaterialInterface, const TArray<FString>& Keywords)
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

	FTrunkCardTriangleSplit SplitTrianglesForTrunkCards(
		const UStaticMesh& StaticMesh,
		TArray<UE::BillboardClouds::FSourceTriangle>& SourceTriangles,
		const bool bEnableTrunkCards,
		const TArray<FString>& RawKeywords)
	{
		FTrunkCardTriangleSplit Split;
		const TArray<FString> Keywords = BuildNormalizedKeywords(RawKeywords);
		Split.bEnabled = bEnableTrunkCards && !Keywords.IsEmpty();
		Split.BillboardTriangles.Reserve(SourceTriangles.Num());
		Split.BillboardToSourceTriangleIndices.Reserve(SourceTriangles.Num());

		TArray<uint8> bMaterialUsesTrunkCards;
		const TArray<FStaticMaterial>& SourceMaterials = StaticMesh.GetStaticMaterials();
		bMaterialUsesTrunkCards.SetNumZeroed(FMath::Max(1, SourceMaterials.Num()));
		if (Split.bEnabled)
		{
			for (int32 MaterialIndex = 0; MaterialIndex < bMaterialUsesTrunkCards.Num(); ++MaterialIndex)
			{
				const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (DoesMaterialOrParentNameMatchKeywords(MaterialInterface, Keywords))
				{
					bMaterialUsesTrunkCards[MaterialIndex] = 1;
					++Split.MatchedMaterialCount;
				}
			}
		}

		for (int32 SourceTriangleIndex = 0; SourceTriangleIndex < SourceTriangles.Num(); ++SourceTriangleIndex)
		{
			UE::BillboardClouds::FSourceTriangle& Triangle = SourceTriangles[SourceTriangleIndex];
			const bool bUseTrunkCards = Split.bEnabled
				&& bMaterialUsesTrunkCards.IsValidIndex(Triangle.MaterialIndex)
				&& bMaterialUsesTrunkCards[Triangle.MaterialIndex] != 0;
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

	UE::BillboardClouds::FPlaneCoverResult RemapPlaneCoverResultToSourceTriangles(
		const UE::BillboardClouds::FPlaneCoverResult& BillboardResult,
		const TArray<int32>& BillboardToSourceTriangleIndices,
		const int32 SourceTriangleCount)
	{
		UE::BillboardClouds::FPlaneCoverResult RemappedResult = BillboardResult;
		RemappedResult.SourceTriangleCount = SourceTriangleCount;
		for (UE::BillboardClouds::FPlaneCoverPlane& Plane : RemappedResult.Planes)
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
		const TArray<UE::BillboardClouds::FSourceTriangle>& SourceTriangles,
		const TArray<int32>& TrunkTriangleIndices,
		const int32 RequestedTrunkPlaneCount,
		UE::BillboardClouds::FPlaneCoverResult& InOutResult)
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

			const UE::BillboardClouds::FSourceTriangle& Triangle = SourceTriangles[TriangleIndex];
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

			UE::BillboardClouds::FPlaneCoverPlane& Plane = InOutResult.Planes.AddDefaulted_GetRef();
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

	struct FTexturePixels
	{
		TArray64<uint8> Bytes;
		int32 Width = 0;
		int32 Height = 0;
		ETextureSourceFormat Format = TSF_Invalid;
		bool bLinearColor = false;

		bool IsValid() const
		{
			return Width > 0 && Height > 0 && !Bytes.IsEmpty() && (Format == TSF_BGRA8 || Format == TSF_G8);
		}

		FColor SampleRawColor(const FVector2f& UV) const
		{
			if (!IsValid())
			{
				return FColor::White;
			}

			const double U = static_cast<double>(UV.X) - FMath::FloorToDouble(static_cast<double>(UV.X));
			const double V = static_cast<double>(UV.Y) - FMath::FloorToDouble(static_cast<double>(UV.Y));
			const int32 X = FMath::Clamp(FMath::FloorToInt(U * static_cast<double>(Width)), 0, Width - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt(V * static_cast<double>(Height)), 0, Height - 1);
			const int64 PixelIndex = static_cast<int64>(Y) * Width + X;

			if (Format == TSF_G8)
			{
				const uint8 Gray = Bytes[PixelIndex];
				return FColor(Gray, Gray, Gray, 255);
			}

			const int64 ByteIndex = PixelIndex * 4;
			const uint8 B = Bytes[ByteIndex + 0];
			const uint8 G = Bytes[ByteIndex + 1];
			const uint8 R = Bytes[ByteIndex + 2];
			const uint8 A = Bytes[ByteIndex + 3];
			return FColor(R, G, B, A);
		}

		FLinearColor Sample(const FVector2f& UV) const
		{
			const FColor RawColor = SampleRawColor(UV);
			return bLinearColor ? RawColor.ReinterpretAsLinear() : FLinearColor(RawColor);
		}

		void SetFromColors(const TArray<FColor>& Colors, const FIntPoint& Size, const bool bInLinearColor)
		{
			Width = Size.X;
			Height = Size.Y;
			Format = TSF_BGRA8;
			bLinearColor = bInLinearColor;
			Bytes.Reset();

			const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
			if (PixelCount <= 0 || Colors.Num() < PixelCount)
			{
				Width = 0;
				Height = 0;
				Format = TSF_Invalid;
				return;
			}

			Bytes.SetNumUninitialized(static_cast<int32>(PixelCount * 4));
			for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				const FColor& Color = Colors[PixelIndex];
				const int64 ByteIndex = PixelIndex * 4;
				Bytes[ByteIndex + 0] = Color.B;
				Bytes[ByteIndex + 1] = Color.G;
				Bytes[ByteIndex + 2] = Color.R;
				Bytes[ByteIndex + 3] = Color.A;
			}
		}

		bool AlphaLooksLikeCutoutMask() const
		{
			if (!IsValid() || Format != TSF_BGRA8)
			{
				return false;
			}

			int64 NonOpaquePixels = 0;
			const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
			for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				const uint8 Alpha = Bytes[PixelIndex * 4 + 3];
				NonOpaquePixels += Alpha < 250 ? 1 : 0;
			}

			const double NonOpaqueRatio = PixelCount > 0 ? static_cast<double>(NonOpaquePixels) / static_cast<double>(PixelCount) : 0.0;
			return NonOpaqueRatio >= 0.001;
		}
	};

	struct FAtlasOutputSelection
	{
		bool bBaseColorOpacity = true;
		bool bNormalMask = true;
		bool bMix = false;

		bool HasAnyOutput() const
		{
			return bBaseColorOpacity || bNormalMask || bMix;
		}
	};

	enum class EBillboardOpacityMaskChannel : uint8
	{
		Red,
		Green,
		Blue,
		Alpha
	};

	const TCHAR* GetOpacityMaskChannelName(const EBillboardOpacityMaskChannel Channel)
	{
		switch (Channel)
		{
		case EBillboardOpacityMaskChannel::Red:
			return TEXT("R");
		case EBillboardOpacityMaskChannel::Green:
			return TEXT("G");
		case EBillboardOpacityMaskChannel::Blue:
			return TEXT("B");
		case EBillboardOpacityMaskChannel::Alpha:
			return TEXT("A");
		default:
			return TEXT("?");
		}
	}

	const TCHAR* GetBlendModeName(EBlendMode BlendMode);

	struct FMaterialScalarBakeData
	{
		float Constant = 0.0f;
		FTexturePixels Texture;
		bool bHasReadableTexture = false;
		bool bUseLuminance = false;
		EBillboardOpacityMaskChannel Channel = EBillboardOpacityMaskChannel::Red;
		FString Source;
	};

	float SampleOpacityMaskValue(const FTexturePixels& Texture, const FVector2f& UV, const EBillboardOpacityMaskChannel Channel)
	{
		const FLinearColor Sample = Texture.Sample(UV);
		if (Texture.Format == TSF_G8)
		{
			return Sample.R;
		}

		switch (Channel)
		{
		case EBillboardOpacityMaskChannel::Red:
			return Sample.R;
		case EBillboardOpacityMaskChannel::Green:
			return Sample.G;
		case EBillboardOpacityMaskChannel::Blue:
			return Sample.B;
		case EBillboardOpacityMaskChannel::Alpha:
			return Sample.A;
		default:
			return Sample.A;
		}
	}

	struct FMaterialBakeData
	{
		FLinearColor BaseColor = FLinearColor::White;
		FTexturePixels BaseColorTexture;
		FTexturePixels OpacityMaskTexture;
		FMaterialScalarBakeData AmbientOcclusion;
		FMaterialScalarBakeData Roughness;
		FMaterialScalarBakeData Metallic;
		FMaterialScalarBakeData Emission;
		bool bHasReadableBaseColorTexture = false;
		bool bHasReadableOpacityMaskTexture = false;
		bool bUseTextureAlphaAsOpacity = false;
		bool bTwoSided = false;
		EBillboardOpacityMaskChannel OpacityMaskChannel = EBillboardOpacityMaskChannel::Alpha;
		float OpacityMaskClipValue = 0.33333334f;
		double OpacityMaskTransparentRatio = 0.0;
		FString OpacityMaskSource;
	};

	struct FAtlasBakeStats
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
		int32 ReadableMaterialTextures = 0;
		int32 SourceMixTextureMaterials = 0;
		int32 SourceMixTextureReferences = 0;
		int32 TextureAlphaOpacityMaterials = 0;
		int32 TextureAlphaOpacityReferences = 0;
		int32 ForcedOpaqueAlphaReferences = 0;
		int32 GpuOpacityExportReferences = 0;
		int32 GpuOpacityExportFailedReferences = 0;
		int32 BakedOpacityClipZeroedPixels = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
		FString MaterialAlphaPolicyDetails;

		// GPU-bake diagnostic aggregates: per (material, property) sum of pixel counts
		// across all tile bakes for that material. Emitted verbatim into the report so
		// it survives Output Log filtering and is trivially inspectable per-run.
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

	bool TextureNameLooksLikeBaseColor(const FString& Name)
	{
		const FString LowerName = Name.ToLower();
		if (LowerName.Contains(TEXT("normal")) || LowerName.Contains(TEXT("_n")) || LowerName.Contains(TEXT("rough")) || LowerName.Contains(TEXT("metal")) || LowerName.Contains(TEXT("ao")) || LowerName.Contains(TEXT("orm")) || LowerName.Contains(TEXT("mask")))
		{
			return false;
		}

		return LowerName.Contains(TEXT("basecolor"))
			|| LowerName.Contains(TEXT("base_color"))
			|| LowerName.Contains(TEXT("albedo"))
			|| LowerName.Contains(TEXT("diffuse"))
			|| LowerName.Contains(TEXT("color"))
			|| LowerName.Contains(TEXT("_d"));
	}

	bool TextureNameLooksLikeOpacityMask(const FString& Name)
	{
		const FString LowerName = Name.ToLower();
		if (LowerName.Contains(TEXT("normal")) || LowerName.Contains(TEXT("_n")) || LowerName.Contains(TEXT("rough")) || LowerName.Contains(TEXT("metal")) || LowerName.Contains(TEXT("orm")) || LowerName.Contains(TEXT("ao")))
		{
			return false;
		}

		return LowerName.Contains(TEXT("opacity"))
			|| LowerName.Contains(TEXT("alpha"))
			|| LowerName.Contains(TEXT("cutout"))
			|| LowerName.Contains(TEXT("cut_out"))
			|| LowerName.Contains(TEXT("transparency"))
			|| LowerName.Contains(TEXT("opacitymask"))
			|| LowerName.Contains(TEXT("opacity_mask"))
			|| LowerName.Contains(TEXT("_mask"))
			|| LowerName.Contains(TEXT("mask_"));
	}

	EBillboardOpacityMaskChannel GetMaskChannelFromInput(const FExpressionInput& Input)
	{
		if (Input.MaskA != 0 || Input.OutputIndex == 4)
		{
			return EBillboardOpacityMaskChannel::Alpha;
		}
		if (Input.MaskG != 0 || Input.OutputIndex == 2)
		{
			return EBillboardOpacityMaskChannel::Green;
		}
		if (Input.MaskB != 0 || Input.OutputIndex == 3)
		{
			return EBillboardOpacityMaskChannel::Blue;
		}

		return EBillboardOpacityMaskChannel::Red;
	}

	bool DoesInputUseExplicitChannel(const FExpressionInput& Input)
	{
		return Input.MaskR != 0 || Input.MaskG != 0 || Input.MaskB != 0 || Input.MaskA != 0 || Input.OutputIndex > 0;
	}

	bool TryLoadTexturePixels(const UTexture2D* Texture, FTexturePixels& OutPixels)
	{
		OutPixels = FTexturePixels();
		if (!Texture || !Texture->Source.IsValid())
		{
			return false;
		}

		const ETextureSourceFormat Format = Texture->Source.GetFormat();
		if (Format != TSF_BGRA8 && Format != TSF_G8)
		{
			return false;
		}

		TArray64<uint8> RawBytes;
		if (!const_cast<UTexture2D*>(Texture)->Source.GetMipData(RawBytes, 0) || RawBytes.IsEmpty())
		{
			return false;
		}

		OutPixels.Bytes = MoveTemp(RawBytes);
		OutPixels.Width = Texture->Source.GetSizeX();
		OutPixels.Height = Texture->Source.GetSizeY();
		OutPixels.Format = Format;
		OutPixels.bLinearColor = !Texture->SRGB;
		return OutPixels.IsValid();
	}

	UTexture2D* FindBestBaseColorTexture(UMaterialInterface* MaterialInterface)
	{
		if (!MaterialInterface)
		{
			return nullptr;
		}

		TArray<FMaterialParameterInfo> TextureParameterInfos;
		TArray<FGuid> TextureParameterIds;
		MaterialInterface->GetAllTextureParameterInfo(TextureParameterInfos, TextureParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : TextureParameterInfos)
		{
			if (!TextureNameLooksLikeBaseColor(ParameterInfo.Name.ToString()))
			{
				continue;
			}

			UTexture* Texture = nullptr;
			if (MaterialInterface->GetTextureParameterValue(FHashedMaterialParameterInfo(ParameterInfo), Texture))
			{
				if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
				{
					return Texture2D;
				}
			}
		}

		TArray<UTexture*> UsedTextures;
		MaterialInterface->GetUsedTextures(UsedTextures);
		for (UTexture* Texture : UsedTextures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && Texture2D->CompressionSettings != TC_Normalmap && TextureNameLooksLikeBaseColor(Texture2D->GetName()))
			{
				return Texture2D;
			}
		}

		for (UTexture* Texture : UsedTextures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && Texture2D->CompressionSettings != TC_Normalmap)
			{
				return Texture2D;
			}
		}

		return nullptr;
	}

	UTexture2D* FindMaterialPropertyTextureFromParameter(
		UMaterialInterface* MaterialInterface,
		const FName ParameterName,
		const TCHAR* PropertyLabel,
		FString& OutSource)
	{
		if (!MaterialInterface || ParameterName.IsNone())
		{
			return nullptr;
		}

		UTexture* Texture = nullptr;
		if (MaterialInterface->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), Texture))
		{
			if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				OutSource = FString::Printf(TEXT("%s parameter %s -> %s"), PropertyLabel, *ParameterName.ToString(), *Texture2D->GetName());
				return Texture2D;
			}
		}

		return nullptr;
	}

	UTexture2D* FindMaterialPropertyTexture(
		UMaterialInterface* MaterialInterface,
		const EMaterialProperty Property,
		const TCHAR* PropertyLabel,
		EBillboardOpacityMaskChannel& OutChannel,
		bool& bOutUseLuminance,
		FString& OutSource)
	{
		OutChannel = EBillboardOpacityMaskChannel::Red;
		bOutUseLuminance = false;
		OutSource.Reset();
		if (!MaterialInterface)
		{
			return nullptr;
		}

		if (UMaterial* Material = MaterialInterface->GetMaterial())
		{
			if (FExpressionInput* PropertyInput = Material->GetExpressionInputForProperty(Property))
			{
				const FExpressionInput TracedInput = PropertyInput->GetTracedInput();
				if (TracedInput.Expression)
				{
					OutChannel = GetMaskChannelFromInput(TracedInput);
					bOutUseLuminance = Property == MP_EmissiveColor && !DoesInputUseExplicitChannel(TracedInput);

					if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = FindMaterialPropertyTextureFromParameter(MaterialInterface, TextureParameter->ParameterName, PropertyLabel, OutSource))
						{
							OutSource += bOutUseLuminance
								? TEXT(" luminance")
								: FString::Printf(TEXT(" channel %s"), GetOpacityMaskChannelName(OutChannel));
							return Texture2D;
						}
					}

					if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = Cast<UTexture2D>(TextureSample->Texture))
						{
							OutSource = bOutUseLuminance
								? FString::Printf(TEXT("%s input texture %s luminance"), PropertyLabel, *Texture2D->GetName())
								: FString::Printf(
									TEXT("%s input texture %s channel %s"),
									PropertyLabel,
									*Texture2D->GetName(),
									GetOpacityMaskChannelName(OutChannel));
							return Texture2D;
						}
					}
				}
			}
		}

		return nullptr;
	}

	UTexture2D* FindOpacityMaskTextureFromParameter(
		UMaterialInterface* MaterialInterface,
		const FName ParameterName,
		FString& OutSource)
	{
		if (!MaterialInterface || ParameterName.IsNone())
		{
			return nullptr;
		}

		UTexture* Texture = nullptr;
		if (MaterialInterface->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), Texture))
		{
			if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				OutSource = FString::Printf(TEXT("opacity parameter %s -> %s"), *ParameterName.ToString(), *Texture2D->GetName());
				return Texture2D;
			}
		}

		return nullptr;
	}

	UTexture2D* FindOpacityMaskTexture(
		UMaterialInterface* MaterialInterface,
		EBillboardOpacityMaskChannel& OutChannel,
		FString& OutSource)
	{
		OutChannel = EBillboardOpacityMaskChannel::Alpha;
		OutSource.Reset();
		if (!MaterialInterface)
		{
			return nullptr;
		}

		if (UMaterial* Material = MaterialInterface->GetMaterial())
		{
			if (FExpressionInput* OpacityMaskInput = Material->GetExpressionInputForProperty(MP_OpacityMask))
			{
				const FExpressionInput TracedInput = OpacityMaskInput->GetTracedInput();
				if (TracedInput.Expression)
				{
					OutChannel = GetMaskChannelFromInput(TracedInput);

					if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = FindOpacityMaskTextureFromParameter(MaterialInterface, TextureParameter->ParameterName, OutSource))
						{
							OutSource += FString::Printf(TEXT(" channel %s"), GetOpacityMaskChannelName(OutChannel));
							return Texture2D;
						}
					}

					if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = Cast<UTexture2D>(TextureSample->Texture))
						{
							OutSource = FString::Printf(
								TEXT("opacity input texture %s channel %s"),
								*Texture2D->GetName(),
								GetOpacityMaskChannelName(OutChannel));
							return Texture2D;
						}
					}
				}
			}
		}

		TArray<FMaterialParameterInfo> TextureParameterInfos;
		TArray<FGuid> TextureParameterIds;
		MaterialInterface->GetAllTextureParameterInfo(TextureParameterInfos, TextureParameterIds);
		for (const FMaterialParameterInfo& ParameterInfo : TextureParameterInfos)
		{
			if (!TextureNameLooksLikeOpacityMask(ParameterInfo.Name.ToString()))
			{
				continue;
			}

			if (UTexture2D* Texture2D = FindOpacityMaskTextureFromParameter(MaterialInterface, ParameterInfo.Name, OutSource))
			{
				OutChannel = Texture2D->Source.GetFormat() == TSF_G8 ? EBillboardOpacityMaskChannel::Red : EBillboardOpacityMaskChannel::Alpha;
				OutSource += FString::Printf(TEXT(" inferred channel %s"), GetOpacityMaskChannelName(OutChannel));
				return Texture2D;
			}
		}

		TArray<UTexture*> UsedTextures;
		MaterialInterface->GetUsedTextures(UsedTextures);
		for (UTexture* Texture : UsedTextures)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D && Texture2D->CompressionSettings != TC_Normalmap && TextureNameLooksLikeOpacityMask(Texture2D->GetName()))
			{
				OutChannel = Texture2D->Source.GetFormat() == TSF_G8 ? EBillboardOpacityMaskChannel::Red : EBillboardOpacityMaskChannel::Alpha;
				OutSource = FString::Printf(
					TEXT("opacity texture name %s inferred channel %s"),
					*Texture2D->GetName(),
					GetOpacityMaskChannelName(OutChannel));
				return Texture2D;
			}
		}

		return nullptr;
	}

	double EstimateOpacityMaskTransparentRatio(
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const int32 MaterialIndex,
		const FMaterialBakeData& BakeData)
	{
		if (!BakeData.bHasReadableOpacityMaskTexture)
		{
			return 0.0;
		}

		double TotalWeight = 0.0;
		double TransparentWeight = 0.0;
		for (const UE::BillboardClouds::FSourceTriangle& Triangle : Triangles)
		{
			if (Triangle.MaterialIndex != MaterialIndex || !Triangle.bHasUVs || Triangle.Area <= 0.0)
			{
				continue;
			}

			const FVector2f SampleUVs[] =
			{
				Triangle.UVs[0],
				Triangle.UVs[1],
				Triangle.UVs[2],
				(Triangle.UVs[0] + Triangle.UVs[1] + Triangle.UVs[2]) / 3.0f
			};

			const double SampleWeight = Triangle.Area / UE_ARRAY_COUNT(SampleUVs);
			for (const FVector2f& UV : SampleUVs)
			{
				TotalWeight += SampleWeight;
				const float MaskValue = SampleOpacityMaskValue(BakeData.OpacityMaskTexture, UV, BakeData.OpacityMaskChannel);
				TransparentWeight += MaskValue < BakeData.OpacityMaskClipValue ? SampleWeight : 0.0;
			}
		}

		return TotalWeight > 0.0 ? TransparentWeight / TotalWeight : 0.0;
	}

	FLinearColor ResolveMaterialBaseColor(UMaterialInterface* MaterialInterface)
	{
		if (!MaterialInterface)
		{
			return FLinearColor::White;
		}

		static const FName ParameterNames[] =
		{
			TEXT("BaseColor"),
			TEXT("Base Color"),
			TEXT("Base_Color"),
			TEXT("Albedo"),
			TEXT("Diffuse"),
			TEXT("Color"),
			TEXT("Tint"),
		};

		for (const FName ParameterName : ParameterNames)
		{
			FLinearColor Color;
			if (MaterialInterface->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), Color))
			{
				return Color;
			}
		}

		return FLinearColor::White;
	}

	float ResolveMaterialScalarParameter(UMaterialInterface* MaterialInterface, const TConstArrayView<FName> ParameterNames, const float DefaultValue)
	{
		if (!MaterialInterface)
		{
			return DefaultValue;
		}

		for (const FName ParameterName : ParameterNames)
		{
			float Value = 0.0f;
			if (MaterialInterface->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), Value))
			{
				return FMath::Clamp(Value, 0.0f, 1.0f);
			}
		}

		return DefaultValue;
	}

	float ResolveMaterialVectorParameterMaxChannel(UMaterialInterface* MaterialInterface, const TConstArrayView<FName> ParameterNames, const float DefaultValue)
	{
		if (!MaterialInterface)
		{
			return DefaultValue;
		}

		for (const FName ParameterName : ParameterNames)
		{
			FLinearColor Value;
			if (MaterialInterface->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), Value))
			{
				return FMath::Clamp(FMath::Max3(Value.R, Value.G, Value.B), 0.0f, 1.0f);
			}
		}

		return DefaultValue;
	}

	FMaterialScalarBakeData BuildMaterialScalarBakeData(
		UMaterialInterface* MaterialInterface,
		const EMaterialProperty Property,
		const TCHAR* PropertyLabel,
		const TConstArrayView<FName> ScalarParameterNames,
		const float DefaultValue,
		FAtlasBakeStats& InOutStats,
		const bool bUseVectorMaxParameterFallback = false)
	{
		FMaterialScalarBakeData BakeData;
		BakeData.Constant = bUseVectorMaxParameterFallback
			? ResolveMaterialVectorParameterMaxChannel(MaterialInterface, ScalarParameterNames, DefaultValue)
			: ResolveMaterialScalarParameter(MaterialInterface, ScalarParameterNames, DefaultValue);

		EBillboardOpacityMaskChannel Channel = EBillboardOpacityMaskChannel::Red;
		bool bUseLuminance = false;
		FString Source;
		if (UTexture2D* Texture = FindMaterialPropertyTexture(MaterialInterface, Property, PropertyLabel, Channel, bUseLuminance, Source))
		{
			BakeData.bHasReadableTexture = TryLoadTexturePixels(Texture, BakeData.Texture);
			if (BakeData.bHasReadableTexture)
			{
				BakeData.Channel = Channel;
				BakeData.bUseLuminance = bUseLuminance;
				BakeData.Source = Source;
				++InOutStats.ReadableMaterialTextures;
			}
		}

		return BakeData;
	}

	bool CopyBakedPropertyToTexture(
		const FBakeOutput& BakeOutput,
		const EMaterialProperty Property,
		FTexturePixels& OutTexture)
	{
		const TArray<FColor>* PropertyData = BakeOutput.PropertyData.Find(Property);
		const FIntPoint* PropertySize = BakeOutput.PropertySizes.Find(Property);
		if (!PropertyData || !PropertySize || PropertyData->IsEmpty() || PropertySize->X <= 0 || PropertySize->Y <= 0)
		{
			return false;
		}

		const bool bLinearColor = BakeOutput.PropertyIsLinearColor.FindRef(Property);
		OutTexture.SetFromColors(*PropertyData, *PropertySize, bLinearColor);
		return OutTexture.IsValid();
	}

	bool ShouldBakeEvaluatedMaterialProperty(
		UMaterialInterface* MaterialInterface,
		const EMaterialProperty Property,
		const bool bMaterialCanUseOpacity)
	{
		if (!MaterialInterface)
		{
			return false;
		}

		if (Property == MP_BaseColor)
		{
			return true;
		}
		if (Property == MP_OpacityMask)
		{
			return bMaterialCanUseOpacity && MaterialInterface->IsPropertyActive(Property);
		}
		return MaterialInterface->IsPropertyActive(Property);
	}

	bool ApplyEvaluatedMaterialOutputs(
		const UStaticMesh& SourceStaticMesh,
		const int32 MaterialIndex,
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		UMaterialInterface* MaterialInterface,
		const FAtlasOutputSelection& OutputSelection,
		const UE::BillboardClouds::FPlaneCoverSettings& Settings,
		FMaterialBakeData& InOutBakeData)
	{
		if (!MaterialInterface)
		{
			return false;
		}

		const FMeshDescription* SourceMeshDescription = SourceStaticMesh.GetMeshDescription(0);
		if (!SourceMeshDescription)
		{
			return false;
		}

		const EBlendMode BlendMode = MaterialInterface->GetBlendMode();
		const bool bMaterialCanUseOpacity = BlendMode != BLEND_Opaque;
		const FIntPoint BakeSize(
			FMath::Clamp(Settings.SourceMaterialBakeResolution, 256, 8192),
			FMath::Clamp(Settings.SourceMaterialBakeResolution, 256, 8192));

		FMaterialData MaterialSettings;
		MaterialSettings.Material = MaterialInterface;
		// Bake raw material property values; alpha clipping is applied later when rasterizing the billboard atlas.
		MaterialSettings.BlendMode = BLEND_Opaque;
		MaterialSettings.bPerformBorderSmear = true;
		MaterialSettings.bPerformShrinking = false;
		MaterialSettings.bTangentSpaceNormal = true;
		MaterialSettings.BackgroundColor = FColor::Transparent;

		auto AddProperty = [&](const EMaterialProperty Property)
		{
			if (ShouldBakeEvaluatedMaterialProperty(MaterialInterface, Property, bMaterialCanUseOpacity))
			{
				MaterialSettings.PropertySizes.Add(Property, BakeSize);
			}
		};

		if (OutputSelection.bBaseColorOpacity)
		{
			AddProperty(MP_BaseColor);
			AddProperty(MP_OpacityMask);
		}
		if (OutputSelection.bMix)
		{
			AddProperty(MP_AmbientOcclusion);
			AddProperty(MP_Roughness);
			AddProperty(MP_Metallic);
			AddProperty(MP_EmissiveColor);
		}

		if (MaterialSettings.PropertySizes.IsEmpty())
		{
			return false;
		}

		FMeshData MeshSettings;
		MeshSettings.MeshDescription = SourceMeshDescription;
		MeshSettings.Mesh = &SourceStaticMesh;
		MeshSettings.MaterialIndices.Add(MaterialIndex);
		MeshSettings.TextureCoordinateBox = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
		MeshSettings.TextureCoordinateIndex = 0;
		MeshSettings.LightMapIndex = SourceStaticMesh.GetLightMapCoordinateIndex();
		MeshSettings.PrimitiveData = FPrimitiveData(&SourceStaticMesh);

		TArray<FMaterialData*> MaterialSettingPtrs;
		MaterialSettingPtrs.Add(&MaterialSettings);
		TArray<FMeshData*> MeshSettingPtrs;
		MeshSettingPtrs.Add(&MeshSettings);

		TArray<FBakeOutput> BakeOutputs;
		IMaterialBakingModule& MaterialBakingModule = FModuleManager::Get().LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
		MaterialBakingModule.SetLinearBake(true);
		MaterialBakingModule.BakeMaterials(MaterialSettingPtrs, MeshSettingPtrs, BakeOutputs);
		if (BakeOutputs.IsEmpty())
		{
			return false;
		}

		bool bAppliedAnyProperty = false;
		const FBakeOutput& BakeOutput = BakeOutputs[0];
		if (CopyBakedPropertyToTexture(BakeOutput, MP_BaseColor, InOutBakeData.BaseColorTexture))
		{
			InOutBakeData.BaseColor = FLinearColor::White;
			InOutBakeData.bHasReadableBaseColorTexture = true;
			bAppliedAnyProperty = true;
		}
		FTexturePixels EvaluatedOpacityMaskTexture;
		if (CopyBakedPropertyToTexture(BakeOutput, MP_OpacityMask, EvaluatedOpacityMaskTexture))
		{
			FMaterialBakeData EvaluatedOpacityMaskData;
			EvaluatedOpacityMaskData.OpacityMaskTexture = EvaluatedOpacityMaskTexture;
			EvaluatedOpacityMaskData.bHasReadableOpacityMaskTexture = true;
			EvaluatedOpacityMaskData.OpacityMaskChannel = EBillboardOpacityMaskChannel::Red;
			EvaluatedOpacityMaskData.OpacityMaskClipValue = InOutBakeData.OpacityMaskClipValue;
			const double EvaluatedTransparentRatio = EstimateOpacityMaskTransparentRatio(Triangles, MaterialIndex, EvaluatedOpacityMaskData);
			const double ExistingTransparentRatio = InOutBakeData.bHasReadableOpacityMaskTexture
				? EstimateOpacityMaskTransparentRatio(Triangles, MaterialIndex, InOutBakeData)
				: 0.0;
			constexpr double MinimumUsefulOpacityMaskTransparentRatio = 0.001;
			constexpr double MinimumEvaluatedToExistingOpacityRatio = 0.25;
			const bool bEvaluatedPreservesExistingCutout =
				ExistingTransparentRatio < MinimumUsefulOpacityMaskTransparentRatio
				|| EvaluatedTransparentRatio >= ExistingTransparentRatio * MinimumEvaluatedToExistingOpacityRatio;
			if (EvaluatedTransparentRatio >= MinimumUsefulOpacityMaskTransparentRatio && bEvaluatedPreservesExistingCutout)
			{
				InOutBakeData.OpacityMaskTexture = MoveTemp(EvaluatedOpacityMaskTexture);
				InOutBakeData.bHasReadableOpacityMaskTexture = true;
				InOutBakeData.bUseTextureAlphaAsOpacity = true;
				InOutBakeData.OpacityMaskChannel = EBillboardOpacityMaskChannel::Red;
				InOutBakeData.OpacityMaskSource = FString::Printf(TEXT("evaluated material OpacityMask output %dx%d"), BakeSize.X, BakeSize.Y);
				bAppliedAnyProperty = true;
			}
			else if (InOutBakeData.OpacityMaskSource.IsEmpty())
			{
				InOutBakeData.OpacityMaskSource = FString::Printf(
					TEXT("evaluated material OpacityMask output rejected, transparent %.2f%%, existing %.2f%%"),
					EvaluatedTransparentRatio * 100.0,
					ExistingTransparentRatio * 100.0);
			}
			else
			{
				InOutBakeData.OpacityMaskSource += FString::Printf(
					TEXT("; evaluated material OpacityMask output rejected, transparent %.2f%%, existing %.2f%%"),
					EvaluatedTransparentRatio * 100.0,
					ExistingTransparentRatio * 100.0);
			}
		}
		if (CopyBakedPropertyToTexture(BakeOutput, MP_AmbientOcclusion, InOutBakeData.AmbientOcclusion.Texture))
		{
			InOutBakeData.AmbientOcclusion.bHasReadableTexture = true;
			InOutBakeData.AmbientOcclusion.Channel = EBillboardOpacityMaskChannel::Red;
			InOutBakeData.AmbientOcclusion.Source = FString::Printf(TEXT("evaluated material AmbientOcclusion output %dx%d"), BakeSize.X, BakeSize.Y);
			bAppliedAnyProperty = true;
		}
		if (CopyBakedPropertyToTexture(BakeOutput, MP_Roughness, InOutBakeData.Roughness.Texture))
		{
			InOutBakeData.Roughness.bHasReadableTexture = true;
			InOutBakeData.Roughness.Channel = EBillboardOpacityMaskChannel::Red;
			InOutBakeData.Roughness.Source = FString::Printf(TEXT("evaluated material Roughness output %dx%d"), BakeSize.X, BakeSize.Y);
			bAppliedAnyProperty = true;
		}
		if (CopyBakedPropertyToTexture(BakeOutput, MP_Metallic, InOutBakeData.Metallic.Texture))
		{
			InOutBakeData.Metallic.bHasReadableTexture = true;
			InOutBakeData.Metallic.Channel = EBillboardOpacityMaskChannel::Red;
			InOutBakeData.Metallic.Source = FString::Printf(TEXT("evaluated material Metallic output %dx%d"), BakeSize.X, BakeSize.Y);
			bAppliedAnyProperty = true;
		}
		if (CopyBakedPropertyToTexture(BakeOutput, MP_EmissiveColor, InOutBakeData.Emission.Texture))
		{
			InOutBakeData.Emission.bHasReadableTexture = true;
			InOutBakeData.Emission.Channel = EBillboardOpacityMaskChannel::Red;
			InOutBakeData.Emission.bUseLuminance = true;
			InOutBakeData.Emission.Source = FString::Printf(TEXT("evaluated material Emissive output %dx%d"), BakeSize.X, BakeSize.Y);
			bAppliedAnyProperty = true;
		}

		return bAppliedAnyProperty;
	}

	TArray<FMaterialBakeData> BuildMaterialBakeData(
		const UStaticMesh& SourceStaticMesh,
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const FAtlasOutputSelection& OutputSelection,
		const UE::BillboardClouds::FPlaneCoverSettings& Settings,
		FAtlasBakeStats& InOutStats)
	{
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		TArray<FMaterialBakeData> MaterialBakeData;
		MaterialBakeData.SetNum(FMath::Max(1, SourceMaterials.Num()));

		for (int32 MaterialIndex = 0; MaterialIndex < MaterialBakeData.Num(); ++MaterialIndex)
		{
			UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
				? SourceMaterials[MaterialIndex].MaterialInterface
				: nullptr;

			FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
			BakeData.BaseColor = ResolveMaterialBaseColor(MaterialInterface);
			static const FName AmbientOcclusionParameterNames[] = { TEXT("AmbientOcclusion"), TEXT("Ambient Occlusion"), TEXT("AO"), TEXT("Occlusion") };
			static const FName RoughnessParameterNames[] = { TEXT("Roughness") };
			static const FName MetallicParameterNames[] = { TEXT("Metallic"), TEXT("Metalness"), TEXT("Metal") };
			static const FName EmissionParameterNames[] = { TEXT("Emission"), TEXT("Emissive"), TEXT("EmissiveIntensity"), TEXT("Emissive Strength") };
			BakeData.AmbientOcclusion = BuildMaterialScalarBakeData(
				MaterialInterface,
				MP_AmbientOcclusion,
				TEXT("ambient occlusion"),
				TConstArrayView<FName>(AmbientOcclusionParameterNames, UE_ARRAY_COUNT(AmbientOcclusionParameterNames)),
				1.0f,
				InOutStats);
			BakeData.Roughness = BuildMaterialScalarBakeData(
				MaterialInterface,
				MP_Roughness,
				TEXT("roughness"),
				TConstArrayView<FName>(RoughnessParameterNames, UE_ARRAY_COUNT(RoughnessParameterNames)),
				0.5f,
				InOutStats);
			BakeData.Metallic = BuildMaterialScalarBakeData(
				MaterialInterface,
				MP_Metallic,
				TEXT("metallic"),
				TConstArrayView<FName>(MetallicParameterNames, UE_ARRAY_COUNT(MetallicParameterNames)),
				0.0f,
				InOutStats);
			BakeData.Emission = BuildMaterialScalarBakeData(
				MaterialInterface,
				MP_EmissiveColor,
				TEXT("emission"),
				TConstArrayView<FName>(EmissionParameterNames, UE_ARRAY_COUNT(EmissionParameterNames)),
				0.0f,
				InOutStats,
				true);
			if (BakeData.AmbientOcclusion.bHasReadableTexture || BakeData.Roughness.bHasReadableTexture || BakeData.Metallic.bHasReadableTexture || BakeData.Emission.bHasReadableTexture)
			{
				++InOutStats.SourceMixTextureMaterials;
			}
			const EBlendMode BlendMode = MaterialInterface ? MaterialInterface->GetBlendMode() : BLEND_Opaque;
			const bool bMaterialCanUseOpacity = BlendMode != BLEND_Opaque;
			BakeData.bTwoSided = MaterialInterface ? MaterialInterface->IsTwoSided() : false;
			BakeData.OpacityMaskClipValue = MaterialInterface ? MaterialInterface->GetOpacityMaskClipValue() : 0.33333334f;
			UTexture2D* LoadedBaseColorTexture = nullptr;
			if (UTexture2D* BaseColorTexture = FindBestBaseColorTexture(MaterialInterface))
			{
				BakeData.bHasReadableBaseColorTexture = TryLoadTexturePixels(BaseColorTexture, BakeData.BaseColorTexture);
				if (BakeData.bHasReadableBaseColorTexture)
				{
					LoadedBaseColorTexture = BaseColorTexture;
					++InOutStats.ReadableMaterialTextures;
				}
			}

			if (bMaterialCanUseOpacity)
			{
				EBillboardOpacityMaskChannel OpacityMaskChannel = EBillboardOpacityMaskChannel::Alpha;
				FString OpacityMaskSource;
				if (UTexture2D* OpacityMaskTexture = FindOpacityMaskTexture(MaterialInterface, OpacityMaskChannel, OpacityMaskSource))
				{
					BakeData.bHasReadableOpacityMaskTexture = TryLoadTexturePixels(OpacityMaskTexture, BakeData.OpacityMaskTexture);
					if (BakeData.bHasReadableOpacityMaskTexture)
					{
						BakeData.bUseTextureAlphaAsOpacity = true;
						BakeData.OpacityMaskChannel = OpacityMaskChannel;
						BakeData.OpacityMaskSource = OpacityMaskSource;
						if (OpacityMaskTexture != LoadedBaseColorTexture)
						{
							++InOutStats.ReadableMaterialTextures;
						}
					}
				}
			}

			if (bMaterialCanUseOpacity
				&& !BakeData.bUseTextureAlphaAsOpacity
				&& BakeData.bHasReadableBaseColorTexture
				&& BakeData.BaseColorTexture.AlphaLooksLikeCutoutMask())
			{
				BakeData.OpacityMaskTexture = BakeData.BaseColorTexture;
				BakeData.bHasReadableOpacityMaskTexture = true;
				BakeData.bUseTextureAlphaAsOpacity = true;
				BakeData.OpacityMaskChannel = EBillboardOpacityMaskChannel::Alpha;
				BakeData.OpacityMaskSource = TEXT("base color alpha cutout fallback");
			}

			const bool bHadMixBeforeEvaluatedBake = BakeData.AmbientOcclusion.bHasReadableTexture
				|| BakeData.Roughness.bHasReadableTexture
				|| BakeData.Metallic.bHasReadableTexture
				|| BakeData.Emission.bHasReadableTexture;
			ApplyEvaluatedMaterialOutputs(SourceStaticMesh, MaterialIndex, Triangles, MaterialInterface, OutputSelection, Settings, BakeData);
			if (!bHadMixBeforeEvaluatedBake
				&& (BakeData.AmbientOcclusion.bHasReadableTexture
					|| BakeData.Roughness.bHasReadableTexture
					|| BakeData.Metallic.bHasReadableTexture
					|| BakeData.Emission.bHasReadableTexture))
			{
				++InOutStats.SourceMixTextureMaterials;
			}

			if (BakeData.bUseTextureAlphaAsOpacity)
			{
				BakeData.OpacityMaskTransparentRatio = EstimateOpacityMaskTransparentRatio(Triangles, MaterialIndex, BakeData);
				++InOutStats.TextureAlphaOpacityMaterials;
				BakeData.OpacityMaskSource += FString::Printf(
					TEXT(", transparent %.2f%%; cutout"),
					BakeData.OpacityMaskTransparentRatio * 100.0);
			}

			if (BakeData.OpacityMaskSource.IsEmpty())
			{
				BakeData.OpacityMaskSource = bMaterialCanUseOpacity
					? TEXT("no explicit opacity mask texture resolved; forced opaque")
					: TEXT("opaque blend; forced opaque");
			}

			const FString SlotName = SourceMaterials.IsValidIndex(MaterialIndex)
				? SourceMaterials[MaterialIndex].MaterialSlotName.ToString()
				: TEXT("None");
			const FString MaterialName = MaterialInterface ? MaterialInterface->GetName() : TEXT("None");
			const FString NormalSource = TEXT("GPU-baked MP_Normal converted through matched source render-data TBN basis for all cards; two-sided tangent-space backfaces flip after decode");
			InOutStats.MaterialAlphaPolicyDetails += FString::Printf(
				TEXT("\n    material=%d, slot=%s, asset=%s, blend=%s, two-sided=%s, alpha=%s, normal=%s"),
				MaterialIndex,
				*SlotName,
				*MaterialName,
				GetBlendModeName(BlendMode),
				BakeData.bTwoSided ? TEXT("yes") : TEXT("no"),
				*BakeData.OpacityMaskSource,
				*NormalSource);
		}

		return MaterialBakeData;
	}

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

	bool ComputeBarycentric3D(const UE::BillboardClouds::FSourceTriangle& Triangle, const FVector& Point, double& OutA, double& OutB, double& OutC)
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
		const UE::BillboardClouds::FSourceTriangle& Triangle,
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

	void NormalizeEncodedNormalAtlas(TArray<FColor>& Pixels)
	{
		for (FColor& Pixel : Pixels)
		{
			const uint8 Alpha = Pixel.A;
			Pixel = EncodeObjectSpaceNormalToColor(DecodeObjectSpaceNormalColor(Pixel), Alpha);
		}
	}

	const TCHAR* GetBlendModeName(const EBlendMode BlendMode)
	{
		switch (BlendMode)
		{
		case BLEND_Opaque:
			return TEXT("Opaque");
		case BLEND_Masked:
			return TEXT("Masked");
		case BLEND_Translucent:
			return TEXT("Translucent");
		case BLEND_Additive:
			return TEXT("Additive");
		case BLEND_Modulate:
			return TEXT("Modulate");
		case BLEND_AlphaComposite:
			return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout:
			return TEXT("AlphaHoldout");
		default:
			return TEXT("Other");
		}
	}

	void DilateTransparentRgbInsideAtlasTiles(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos,
		const TBitArray<>* CoverageMask = nullptr,
		const bool bDilateAlpha = false)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
		{
			return;
		}

		TBitArray<> HasColor;
		HasColor.Init(false, Pixels.Num());
		const bool bUseCoverageMask = CoverageMask && CoverageMask->Num() == Pixels.Num();

		struct FAtlasTileInfo
		{
			FIntPoint PixelMin = FIntPoint::ZeroValue;
			FIntPoint TileSize = FIntPoint::ZeroValue;
			int32 Padding = 0;
		};

		TArray<FAtlasTileInfo> AtlasTiles;
		AtlasTiles.Reserve(PlaneInfos.Num() * 2);

		TArray<int32> TileDilationIterations;
		TileDilationIterations.Reserve(PlaneInfos.Num() * 2);

		int32 MaxDilationIterations = 0;
		auto AddAtlasTileForDilation = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize, const int32 Padding)
		{
			const int32 MinX = FMath::Clamp(PixelMin.X, 0, Width - 1);
			const int32 MinY = FMath::Clamp(PixelMin.Y, 0, Height - 1);
			const int32 MaxX = FMath::Clamp(PixelMin.X + TileSize.X - 1, 0, Width - 1);
			const int32 MaxY = FMath::Clamp(PixelMin.Y + TileSize.Y - 1, 0, Height - 1);
			if (TileSize.X <= 0 || TileSize.Y <= 0 || MinX > MaxX || MinY > MaxY)
			{
				return;
			}

			const int32 DilationIterations = FMath::Clamp(FMath::Max(4, Padding), 1, 32);
			AtlasTiles.Add({ PixelMin, TileSize, Padding });
			TileDilationIterations.Add(DilationIterations);
			MaxDilationIterations = FMath::Max(MaxDilationIterations, DilationIterations);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 PixelIndex = Y * Width + X;
					if (bUseCoverageMask ? (*CoverageMask)[PixelIndex] : Pixels[PixelIndex].A > 0)
					{
						HasColor[PixelIndex] = true;
					}
				}
			}
		};

		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AddAtlasTileForDilation(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AddAtlasTileForDilation(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			}
		}

		if (MaxDilationIterations <= 0)
		{
			return;
		}

		static constexpr int32 NeighborOffsets[8][2] =
		{
			{ -1, -1 }, { 0, -1 }, { 1, -1 },
			{ -1,  0 },            { 1,  0 },
			{ -1,  1 }, { 0,  1 }, { 1,  1 }
		};

		TArray<FColor> Scratch;
		for (int32 Iteration = 0; Iteration < MaxDilationIterations; ++Iteration)
		{
			Scratch = Pixels;
			TBitArray<> NextHasColor = HasColor;
			bool bChanged = false;

			for (int32 TileIndex = 0; TileIndex < AtlasTiles.Num(); ++TileIndex)
			{
				if (!TileDilationIterations.IsValidIndex(TileIndex) || Iteration >= TileDilationIterations[TileIndex])
				{
					continue;
				}

				const FAtlasTileInfo& AtlasTile = AtlasTiles[TileIndex];
				const FIntPoint TileSize = AtlasTile.TileSize;
				const int32 MinX = FMath::Clamp(AtlasTile.PixelMin.X, 0, Width - 1);
				const int32 MinY = FMath::Clamp(AtlasTile.PixelMin.Y, 0, Height - 1);
				const int32 MaxX = FMath::Clamp(AtlasTile.PixelMin.X + TileSize.X - 1, 0, Width - 1);
				const int32 MaxY = FMath::Clamp(AtlasTile.PixelMin.Y + TileSize.Y - 1, 0, Height - 1);
				if (TileSize.X <= 0 || TileSize.Y <= 0 || MinX > MaxX || MinY > MaxY)
				{
					continue;
				}

				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						const int32 PixelIndex = Y * Width + X;
						if (HasColor[PixelIndex])
						{
							continue;
						}

						int32 AccumR = 0;
						int32 AccumG = 0;
						int32 AccumB = 0;
						int32 AccumA = 0;
						int32 SourceCount = 0;
						for (const auto& Offset : NeighborOffsets)
						{
							const int32 NeighborX = X + Offset[0];
							const int32 NeighborY = Y + Offset[1];
							if (NeighborX < MinX || NeighborX > MaxX || NeighborY < MinY || NeighborY > MaxY)
							{
								continue;
							}

							const int32 NeighborIndex = NeighborY * Width + NeighborX;
							if (!HasColor[NeighborIndex])
							{
								continue;
							}

							const FColor& NeighborColor = Pixels[NeighborIndex];
							AccumR += NeighborColor.R;
							AccumG += NeighborColor.G;
							AccumB += NeighborColor.B;
							AccumA += NeighborColor.A;
							++SourceCount;
						}

						if (SourceCount > 0)
						{
							FColor& DilatedColor = Scratch[PixelIndex];
							DilatedColor.R = static_cast<uint8>(FMath::Clamp((AccumR + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.G = static_cast<uint8>(FMath::Clamp((AccumG + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.B = static_cast<uint8>(FMath::Clamp((AccumB + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.A = bDilateAlpha
								? static_cast<uint8>(FMath::Clamp((AccumA + SourceCount / 2) / SourceCount, 0, 255))
								: 0;
							NextHasColor[PixelIndex] = true;
							bChanged = true;
						}
					}
				}
			}

			if (!bChanged)
			{
				break;
			}

			Pixels = MoveTemp(Scratch);
			HasColor = MoveTemp(NextHasColor);
		}
	}

	void CopyAtlasTileBorderIntoPadding(
		TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos,
		const bool bClearPaddingAlpha = true)
	{
		auto CopyTileBorderIntoPadding = [&Pixels, Width, Height, bClearPaddingAlpha](const FIntPoint& PixelMin, const FIntPoint& TileSize, const int32 Padding)
		{
			if (Padding <= 0 || TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}

			const int32 InteriorMinX = PixelMin.X;
			const int32 InteriorMinY = PixelMin.Y;
			const int32 InteriorMaxX = InteriorMinX + TileSize.X - 1;
			const int32 InteriorMaxY = InteriorMinY + TileSize.Y - 1;
			const int32 PaddedMinX = FMath::Max(0, InteriorMinX - Padding);
			const int32 PaddedMinY = FMath::Max(0, InteriorMinY - Padding);
			const int32 PaddedMaxX = FMath::Min(Width - 1, InteriorMaxX + Padding);
			const int32 PaddedMaxY = FMath::Min(Height - 1, InteriorMaxY + Padding);

			for (int32 Y = PaddedMinY; Y <= PaddedMaxY; ++Y)
			{
				for (int32 X = PaddedMinX; X <= PaddedMaxX; ++X)
				{
					if (X >= InteriorMinX && X <= InteriorMaxX && Y >= InteriorMinY && Y <= InteriorMaxY)
					{
						continue;
					}

					const int32 SourceX = FMath::Clamp(X, InteriorMinX, InteriorMaxX);
					const int32 SourceY = FMath::Clamp(Y, InteriorMinY, InteriorMaxY);
					FColor PaddingColor = Pixels[SourceY * Width + SourceX];
					if (bClearPaddingAlpha)
					{
						PaddingColor.A = 0;
					}
					Pixels[Y * Width + X] = PaddingColor;
				}
			}
		};

		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			CopyTileBorderIntoPadding(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				CopyTileBorderIntoPadding(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			}
		}
	}

	bool IsPointInsidePlaneProxyEnvelope(const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo, const FVector& Point, const double Tolerance)
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

	double ComputePlaneProxyEnvelopeBoundaryDistance(const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo, const FVector& Point)
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
		const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo,
		const FVector& Point,
		const double BoundaryWidth,
		const double Tolerance)
	{
		const double BoundaryDistance = ComputePlaneProxyEnvelopeBoundaryDistance(PlaneInfo, Point);
		return BoundaryDistance >= -Tolerance
			&& BoundaryDistance <= FMath::Max(BoundaryWidth, Tolerance);
	}

	bool AccumulateAlphaBoundsForTile(
		const TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const FIntPoint& PixelMin,
		const FIntPoint& TileSize,
		const int32 GuardPixels,
		double& InOutMinUFraction,
		double& InOutMaxUFraction,
		double& InOutMinVFraction,
		double& InOutMaxVFraction)
	{
		if (Width <= 0 || Height <= 0 || TileSize.X <= 0 || TileSize.Y <= 0 || Pixels.Num() < Width * Height)
		{
			return false;
		}

		int32 MinLocalX = TNumericLimits<int32>::Max();
		int32 MaxLocalX = -TNumericLimits<int32>::Max();
		int32 MinLocalY = TNumericLimits<int32>::Max();
		int32 MaxLocalY = -TNumericLimits<int32>::Max();

		for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
		{
			const int32 Y = PixelMin.Y + LocalY;
			if (Y < 0 || Y >= Height)
			{
				continue;
			}

			for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
			{
				const int32 X = PixelMin.X + LocalX;
				if (X < 0 || X >= Width)
				{
					continue;
				}

				if (Pixels[Y * Width + X].A == 0)
				{
					continue;
				}

				MinLocalX = FMath::Min(MinLocalX, LocalX);
				MaxLocalX = FMath::Max(MaxLocalX, LocalX);
				MinLocalY = FMath::Min(MinLocalY, LocalY);
				MaxLocalY = FMath::Max(MaxLocalY, LocalY);
			}
		}

		if (MaxLocalX < MinLocalX || MaxLocalY < MinLocalY)
		{
			return false;
		}

		const int32 ExpandedMinX = FMath::Clamp(MinLocalX - GuardPixels, 0, TileSize.X - 1);
		const int32 ExpandedMaxX = FMath::Clamp(MaxLocalX + GuardPixels, 0, TileSize.X - 1);
		const int32 ExpandedMinY = FMath::Clamp(MinLocalY - GuardPixels, 0, TileSize.Y - 1);
		const int32 ExpandedMaxY = FMath::Clamp(MaxLocalY + GuardPixels, 0, TileSize.Y - 1);

		InOutMinUFraction = FMath::Min(InOutMinUFraction, static_cast<double>(ExpandedMinX) / static_cast<double>(TileSize.X));
		InOutMaxUFraction = FMath::Max(InOutMaxUFraction, static_cast<double>(ExpandedMaxX + 1) / static_cast<double>(TileSize.X));
		InOutMinVFraction = FMath::Min(InOutMinVFraction, static_cast<double>(ExpandedMinY) / static_cast<double>(TileSize.Y));
		InOutMaxVFraction = FMath::Max(InOutMaxVFraction, static_cast<double>(ExpandedMaxY + 1) / static_cast<double>(TileSize.Y));
		return true;
	}

	int32 BuildAlphaAwareTileCrops(
		const TArray<FColor>& Pixels,
		const int32 Width,
		const int32 Height,
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos,
		const int32 GuardPixels,
		TArray<UE::BillboardClouds::FPlaneProxyTileCrop>& OutTileCrops)
	{
		OutTileCrops.Reset();
		OutTileCrops.SetNum(PlaneInfos.Num());

		int32 CroppedPlaneCount = 0;
		constexpr double CropEpsilon = 1.0e-5;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			double MinUFraction = 1.0;
			double MaxUFraction = 0.0;
			double MinVFraction = 1.0;
			double MaxVFraction = 0.0;

			bool bHasCoverage = AccumulateAlphaBoundsForTile(
				Pixels,
				Width,
				Height,
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				GuardPixels,
				MinUFraction,
				MaxUFraction,
				MinVFraction,
				MaxVFraction);

			if (PlaneInfo.bHasBackFaceAtlas)
			{
				bHasCoverage |= AccumulateAlphaBoundsForTile(
					Pixels,
					Width,
					Height,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					GuardPixels,
					MinUFraction,
					MaxUFraction,
					MinVFraction,
					MaxVFraction);
			}

			if (!bHasCoverage)
			{
				continue;
			}

			MinUFraction = FMath::Clamp(MinUFraction, 0.0, 1.0);
			MaxUFraction = FMath::Clamp(MaxUFraction, 0.0, 1.0);
			MinVFraction = FMath::Clamp(MinVFraction, 0.0, 1.0);
			MaxVFraction = FMath::Clamp(MaxVFraction, 0.0, 1.0);
			const bool bCropsTile =
				MinUFraction > CropEpsilon
				|| MaxUFraction < 1.0 - CropEpsilon
				|| MinVFraction > CropEpsilon
				|| MaxVFraction < 1.0 - CropEpsilon;
			if (!bCropsTile || MaxUFraction <= MinUFraction || MaxVFraction <= MinVFraction)
			{
				continue;
			}

			UE::BillboardClouds::FPlaneProxyTileCrop& Crop = OutTileCrops[PlaneIndex];
			Crop.bEnabled = true;
			Crop.MinUFraction = MinUFraction;
			Crop.MaxUFraction = MaxUFraction;
			Crop.MinVFraction = MinVFraction;
			Crop.MaxVFraction = MaxVFraction;
			++CroppedPlaneCount;
		}

		return CroppedPlaneCount;
	}

	// ----------------------------------------------------------------------------
	// GPU material baking path (route A).
	//
	// For each proxy plane we build a temporary FMeshDescription that preserves the
	// source VertexInstanceUVs for material sampling. Atlas-tile positions are passed
	// separately through FMeshData::CustomTextureCoordinates, so the material shader
	// still sees the source TexCoord streams while the baker rasterizes into the tile.
	// We then call IMaterialBakingModule::BakeMaterials which runs the material's real
	// pixel shader on the GPU into a render target sized to the tile. The result is
	// blitted into the shared atlas at the plane's tile position.
	//
	// This path supports real material features such as tangent-space normal maps,
	// WPO, Custom nodes, Layered Materials, and parameter-driven material graphs.
	// Source vertex colors are not preserved yet; they are filled with white below.
	// ----------------------------------------------------------------------------

	// Build a per-plane MeshDescription that will drive the material baker.
	// Design points (important, non-obvious):
	//   * We do NOT overwrite the mesh's VertexInstanceUVs with tile-UV. The
	//     material's own pixel shader samples TexCoord[N] from these; overwriting
	//     UV0 with tile-UV was the root cause of the "leaves gone / opacity mask
	//     all black" bug from the first attempt.
	//   * Instead, atlas-tile UV positions are handed to the baker through
	//     FMeshData::CustomTextureCoordinates, which the material renderer uses
	//     ONLY for vertex XY position (see MaterialRenderItem.cpp line 273). This
	//     is exactly the same mechanism MeshMergeUtilities uses to bake atlases.
	//   * The atlas UVs are relative to TextureCoordinateBox = [0,1], so we just
	//     compute per-vertex tile UV = (tile_pixel_uv) and let the baker sort out
	//     the transform to pixel positions.
	//   * The baker's RT is sized to the tile interior. Atlas padding is filled
	//     afterwards by atlas-side dilation and tile-border copy passes.
	//
	// Also fills VertexColors from an all-white default (source vertex colors
	// aren't captured by FSourceTriangle today; if a project needs them, extend
	// FSourceTriangle and copy them through here).
	bool BuildPerPlaneBakeMeshDescription(
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::BillboardClouds::FCrackReductionProjection>& CrackReductionProjections,
		const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo,
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

		// MaterialRenderItem forwards UV0..UV5 to the material shader and reserves
		// UV6/UV7 for source position storage, so only preserve the usable channels.
		const int32 DesiredUVChannels = FMath::Clamp(NumSourceUVChannels, 1, UE::BillboardClouds::MaxMaterialBakeUVChannels);

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

		// Geometry fills the entire RT [0,1]^2 (matches TileSize exactly). Padding
		// around the tile in the ATLAS image (not in this per-tile RT) is handled
		// afterwards by DilateTransparentRgbInsideAtlasTiles / CopyAtlasTileBorderIntoPadding
		// in the caller. Keeping the RT sized to the interior avoids wasting
		// (padding*2)^2 pixels per bake and simplifies index math.

		auto AppendTriangleGeometry = [&](
			const UE::BillboardClouds::FSourceTriangle& Tri,
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

				// Preserve source UVs per channel. The material shader samples these;
				// this is what makes material functions, base color, opacity mask etc. work.
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
				VertexInstanceColors[VertexInstanceIDs[Corner]] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

				// CustomTextureCoordinates: one entry per vertex-instance in
				// triangle-emission order (see MaterialRenderItem.cpp line 259:
				// SrcVertIndex = FaceIndex * 3 + CornerIdx). Back-side bakes
				// reverse winding below, so custom UVs are appended in the same
				// order that the triangle is submitted.
				const FVector Projected = UE::BillboardClouds::ProjectPointToPlane(
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
			const UE::BillboardClouds::FSourceTriangle& Tri = Triangles[TriangleIndex];
			const FVector Positions[3] = { Tri.Vertices[0], Tri.Vertices[1], Tri.Vertices[2] };
			if (AppendTriangleGeometry(Tri, Positions))
			{
				++OutMatchingTriangleCount;
			}
		}

		for (const UE::BillboardClouds::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}
			const UE::BillboardClouds::FSourceTriangle& Tri = Triangles[Projection.TriangleIndex];
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
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::BillboardClouds::FCrackReductionProjection>& CrackReductionProjections,
		const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo,
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
			const UE::BillboardClouds::FSourceTriangle& Triangle,
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
			// FSourceTriangle::Normal is the mathematical cross product of UE's
			// index order. UE front-face winding is the opposite direction, so
			// source backfaces relative to the capture ray test with '< 0' here.
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

				const FVector ProjectedVertex = UE::BillboardClouds::ProjectPointToPlane(
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

					// MaterialBaking uses temporary mesh draw order rather than a
					// true per-pixel depth buffer. Store source sample depth here so
					// atlas writes can resolve overlapping trunk/branch fragments for
					// the current orthographic card side.
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
			const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector Positions[3] = { Triangle.Vertices[0], Triangle.Vertices[1], Triangle.Vertices[2] };
			RasterizeProjectedTriangle(Triangle, Positions);
		}

		for (const UE::BillboardClouds::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}

			const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[Projection.TriangleIndex];
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

	int32 GetSourceMeshMaxUVChannelCount(const UStaticMesh& StaticMesh)
	{
		const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription(0);
		if (!MeshDescription)
		{
			return 1;
		}
		const FStaticMeshConstAttributes Attributes(*MeshDescription);
		const int32 Channels = Attributes.GetVertexInstanceUVs().GetNumChannels();
		return FMath::Clamp(Channels, 1, UE::BillboardClouds::MaxMaterialBakeUVChannels);
	}

	// A pixel returned from the baker matches its BackgroundColor iff the render
	// pass never covered it (outside the triangle AND not filled by border smear).
	// We treat those as "no coverage". Everything else is a real material sample.
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

		// CaptureDepth is measured along the camera ray direction. Smaller depth
		// is closer to the orthographic camera, so it wins the tile pixel.
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

	// Merge GPU-baked BaseColor with an opacity mask: RGB comes from MP_BaseColor;
	// A comes from MP_OpacityMask or the projected evaluated/fallback mask.
	//
	// Coverage rules (why this is subtle):
	//   * Baker fills the RT with BackgroundColor, then rasterizes the material.
	//     Pixels outside the triangle keep BackgroundColor. Border smear then
	//     dilates the material's own colors into the surrounding area, but that
	//     dilation only kicks in inside the padding region.
	//   * A pixel is "covered by this bake" iff its BaseColor sample is not the
	//     background color. Covered pixels are still depth-resolved against other
	//     fragments that project into the same card tile.
	//   * Inside this merge step, the opacity mask bake only controls the alpha channel.
	//   * Opaque materials (no alpha export) get alpha = 255 always.
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
					// No material coverage here; leave the atlas pixel as-is.
					continue;
				}

				// Compose final color:
				//   RGB = baked BaseColor (re-encoded to sRGB byte if baker returned linear)
				//   A   = baked OpacityMask export (if material has one) else 255
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
						// The masked OpacityMask bake clips rejected pixels, so the
						// render target background means "alpha rejected" here.
						Out.A = 0;
						++InOutBakedClipZeroedPixels;
					}
					else
					{
						// MaterialBaking writes scalar mask output as R.
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
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::BillboardClouds::FCrackReductionProjection>& CrackReductionProjections,
		const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo,
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
			const UE::BillboardClouds::FSourceTriangle& Triangle,
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

				const FVector ProjectedVertex = UE::BillboardClouds::ProjectPointToPlane(
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

			const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector Positions[3] = { Triangle.Vertices[0], Triangle.Vertices[1], Triangle.Vertices[2] };
			RasterizeProjectedTriangle(Triangle, Positions);
		}

		for (const UE::BillboardClouds::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (!Triangles.IsValidIndex(Projection.TriangleIndex) || Projection.ClippedPolygon.Num() < 3)
			{
				continue;
			}

			const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[Projection.TriangleIndex];
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

	// Set of source material indices actually referenced by any triangle in the given list.
	TArray<int32> CollectReferencedMaterialIndices(
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::BillboardClouds::FCrackReductionProjection>& CrackReductionProjections)
	{
		TSet<int32> Set;
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (Triangles.IsValidIndex(TriangleIndex))
			{
				Set.Add(FMath::Max(0, Triangles[TriangleIndex].MaterialIndex));
			}
		}
		for (const UE::BillboardClouds::FCrackReductionProjection& Projection : CrackReductionProjections)
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
		const UE::BillboardClouds::FSourceTriangle& Triangle,
		const FVector& CaptureRayDirection)
	{
		const FVector Center = (Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
		return FVector::DotProduct(Center, CaptureRayDirection);
	}

	double ComputeProjectionCaptureDepth(
		const UE::BillboardClouds::FCrackReductionProjection& Projection,
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
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
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const FVector& CaptureRayDirection,
		TArray<int32>& TriangleIndices,
		TArray<UE::BillboardClouds::FCrackReductionProjection>& CrackReductionProjections)
	{
		// MaterialBaking renders the temporary mesh without depth testing. Emit
		// fragments far-to-near so the later rasterized pixels approximate an
		// orthographic depth resolve for the current card side.
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
			[&Triangles, &CaptureRayDirection](const UE::BillboardClouds::FCrackReductionProjection& A, const UE::BillboardClouds::FCrackReductionProjection& B)
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
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::BillboardClouds::FPlaneProxyMeshStats& ProxyStats,
		const UE::BillboardClouds::FPlaneCoverSettings& Settings,
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
			OutNormalPixels.Init(EncodeObjectSpaceNormalToColor(FVector::UpVector, 0), OutStats.Width * OutStats.Height);
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

		// Statistics + tile-count bookkeeping for the final report.
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
			if (bBackFace) { ++OutStats.BackTileCount; } else { ++OutStats.FrontTileCount; }
		};
		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
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

		IMaterialBakingModule& MaterialBakingModule = FModuleManager::Get().LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
		MaterialBakingModule.SetLinearBake(true);
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		const int32 NumSourceUVChannels = GetSourceMeshMaxUVChannelCount(SourceStaticMesh);
		const TArray<FMaterialBakeData> MaterialBakeData = BuildMaterialBakeData(SourceStaticMesh, Triangles, OutputSelection, Settings, OutStats);

		auto BakePlaneAndSide = [&](
			const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bReverseBakeWinding)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}

			// Gather primary triangles + (PlaneSpaceGreedy) valid-zone touching triangles + crack reduction.
			TArray<int32> PrimaryTriangleIndices;
			PrimaryTriangleIndices.Reserve(PlaneInfo.TriangleIndices.Num());
			TArray<UE::BillboardClouds::FCrackReductionProjection> CrackReductionProjectionsToBake;
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
			if (!PlaneInfo.bIsTrunkCard
				&& Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::PlaneSpaceGreedy)
			{
				for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
				{
					if (!QueuedTriangles[TriangleIndex]
						&& !Triangles[TriangleIndex].bTrunkCardOnly
						&& UE::BillboardClouds::DoesTriangleIntersectPlaneValidZone(
							Triangles[TriangleIndex], PlaneInfo.Normal, PlaneInfo.Rho, Settings))
					{
						QueuedTriangles[TriangleIndex] = true;
						PrimaryTriangleIndices.Add(TriangleIndex);
					}
				}
			}
			// Crack reduction (KMeans): add the already envelope-clipped cross-plane
			// projection fragments generated by the plane-cover pass.
			if (!PlaneInfo.bIsTrunkCard
				&& Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::KMeansClustering)
			{
				for (const UE::BillboardClouds::FCrackReductionProjection& Projection : PlaneInfo.CrackReductionProjections)
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

			// Group by source material index — one bake job per (plane, side, material).
			const TArray<int32> MaterialIndicesUsed = CollectReferencedMaterialIndices(Triangles, PrimaryTriangleIndices, CrackReductionProjectionsToBake);
			// Bake RT is sized to the tile interior. Tile padding in the atlas image
			// is filled by the atlas-side post-pass (DilateTransparentRgbInsideAtlasTiles
			// + CopyAtlasTileBorderIntoPadding), not inside the per-tile bake.
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

				// Magenta (255,0,255,255) is the canonical "no coverage" sentinel used by
				// UE's material baker (see FMaterialData::BackgroundColor default). It's chosen
				// because it's exceedingly unlikely to be a legitimate material output, so we
				// can reliably distinguish "material never drew here" from "material drew (0,0,0)".
				// Using (0,0,0,0) would collide with legitimate outputs like BaseColor=black or
				// OpacityMask=0.
				const FColor BakeBackground = FColor::Magenta;

				FMaterialData MaterialSettings;
				MaterialSettings.Material = MaterialInterface;
				// Bake raw material property values. Keeping this pass opaque
				// prevents the Masked pass from clipping pixels before we read
				// the raw OpacityMask output; the atlas merge applies the source
				// material's OpacityMaskClipValue below.
				MaterialSettings.BlendMode = BLEND_Opaque;
				MaterialSettings.bPerformBorderSmear = false;
				MaterialSettings.bPerformShrinking = false;
				// Force world-space source normals to tangent-space when necessary.
				// The atlas writer converts this baked material normal back to
				// object/local space using the source render-data TBN basis.
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
				// PolygonGroupID=0 is the only group in our temp mesh; MaterialBaking filters
				// triangles by matching PolygonGroupID against MaterialIndices, so we pass {0}.
				MeshSettings.MaterialIndices.Add(0);
				MeshSettings.TextureCoordinateBox = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				// TextureCoordinateIndex is NOT used when CustomTextureCoordinates is set for
				// vertex positioning (see MaterialRenderItem.cpp line 265-275). It's still
				// required to be < UV channel count (MaterialRenderItem.cpp line 242 check).
				// We keep it at 0 which always exists.
				MeshSettings.TextureCoordinateIndex = 0;
				MeshSettings.LightMapIndex = 0;
				MeshSettings.PrimitiveData = FPrimitiveData(&SourceStaticMesh);
				// Hand tile UVs to the baker as per-vertex-instance positions. The material's
				// pixel shader still samples VertexInstanceUVs (source UVs) for its textures.
				MeshSettings.CustomTextureCoordinates = MoveTemp(CustomTileUVs);

				TArray<FMaterialData*> MaterialSettingPtrs;
				MaterialSettingPtrs.Add(&MaterialSettings);
				TArray<FMeshData*> MeshSettingPtrs;
				MeshSettingPtrs.Add(&MeshSettings);

				TArray<FBakeOutput> BakeOutputs;
				MaterialBakingModule.BakeMaterials(MaterialSettingPtrs, MeshSettingPtrs, BakeOutputs);
				if (BakeOutputs.IsEmpty())
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
						|| !CandidateOpacityData || CandidateOpacityData->IsEmpty())
					{
						return false;
					}

					const uint8 ClipByte = UnitFloatToByte(SourceOpacityMaskClipValue);
					const int32 NumSamples = FMath::Min(BaseColorData->Num(), CandidateOpacityData->Num());
					int64 BaseCoveredPixels = 0;
					int64 KeptOpacityPixels = 0;
					int64 RejectedOpacityPixels = 0;
					for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
					{
						if (IsBakerBackgroundPixel((*BaseColorData)[SampleIndex], BakeBackground))
						{
							continue;
						}
						++BaseCoveredPixels;
						const FColor& OpacitySample = (*CandidateOpacityData)[SampleIndex];
						if (!IsBakerBackgroundPixel(OpacitySample, BakeBackground)
							&& OpacitySample.R >= ClipByte)
						{
							++KeptOpacityPixels;
						}
						else
						{
							++RejectedOpacityPixels;
						}
					}
					return BaseCoveredPixels > 0
						&& KeptOpacityPixels > 0
						&& RejectedOpacityPixels > 0;
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

				// Aggregate per-material bake diagnostics into OutStats so they surface
				// verbatim in the final report (see BuildProxySuccessReport). Aggregating
				// across all tile bakes for each material keeps the report short: one
				// summary block per material, regardless of how many planes were baked.
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
				if (!BakeSizePtr && NormalData) { BakeSizePtr = BakeOutput.PropertySizes.Find(MP_Normal); }
				if (!BakeSizePtr && AOData)     { BakeSizePtr = BakeOutput.PropertySizes.Find(MP_AmbientOcclusion); }
				if (!BakeSizePtr)               { continue; }
				const FIntPoint BakeSize = *BakeSizePtr;

				const bool bBaseColorLinear = BakeOutput.PropertyIsLinearColor.FindRef(MP_BaseColor);
				const bool bBaseColorWritesDepth = BaseColorData && !BaseColorData->IsEmpty();

				// BaseColor + Opacity into ColorOpacity atlas.
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

				// Normal atlas.
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
							// Skip background pixels (no material coverage from this bake pass).
							if (IsBakerBackgroundPixel(RawNormal, BakeBackground))
							{
								continue;
							}
							const int32 BasisIdx = LocalY * TileSize.X + LocalX;
							// If BaseColor ran for this material, only write Normal where
							// this same material won the depth-resolved color pixel.
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
							OutNormalPixels[DstIdx] = EncodeBakedTangentSpaceNormalToObjectSpaceColor(
								RawNormal,
								NormalBasisMap[BasisIdx],
								CoverageAlpha);
						}
					}
				}

				// Mix atlas (R=AO, G=Roughness, B=Metallic, A=Emission luminance).
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
								// If this pixel wasn't covered by ANY of the mix passes, skip.
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

		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
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

		// Post-processing: dilate transparent pixels inside each atlas tile,
		// then copy the tile border into the surrounding padding to survive bilinear + mipmaps.
		DilateTransparentRgbInsideAtlasTiles(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);
		CopyAtlasTileBorderIntoPadding(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);
		if (OutputSelection.bNormalMask)
		{
			DilateTransparentRgbInsideAtlasTiles(OutNormalPixels, OutStats.Width, OutStats.Height, PlaneInfos);
			CopyAtlasTileBorderIntoPadding(OutNormalPixels, OutStats.Width, OutStats.Height, PlaneInfos);
			NormalizeEncodedNormalAtlas(OutNormalPixels);
		}
		if (OutputSelection.bMix)
		{
			DilateTransparentRgbInsideAtlasTiles(OutMixPixels, OutStats.Width, OutStats.Height, PlaneInfos, &AtlasCoverage, true);
			CopyAtlasTileBorderIntoPadding(OutMixPixels, OutStats.Width, OutStats.Height, PlaneInfos, false);
		}
	}

	UTexture2D* CreateBillboardTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		const FString& AssetNameSuffix,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TextureCompressionSettings CompressionSettings,
		const bool bSRGB,
		const FString& EmptyPixelsError,
		FString& OutError)
	{
		OutError.Reset();
		if (Pixels.IsEmpty() || AtlasStats.Width <= 0 || AtlasStats.Height <= 0)
		{
			OutError = EmptyPixelsError;
			return nullptr;
		}

		const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
		const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
		const FString BaseAssetName = SourceStaticMesh.GetName() + AssetNameSuffix;
		const FString BasePackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *BaseAssetName);

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString UniquePackageName;
		FString UniqueAssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), UniquePackageName, UniqueAssetName);

		UPackage* Package = CreatePackage(*UniquePackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Could not create package %s."), *UniquePackageName);
			return nullptr;
		}

		Package->FullyLoad();
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *UniqueAssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Could not create Texture2D %s."), *UniqueAssetName);
			return nullptr;
		}

		Texture->PreEditChange(nullptr);
		Texture->Source.Init(AtlasStats.Width, AtlasStats.Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->CompressionSettings = CompressionSettings;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->SRGB = bSRGB;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(Texture);
		return Texture;
	}

	UTexture2D* CreateAtlasTextureAsset(const UStaticMesh& SourceStaticMesh, const TArray<FColor>& Pixels, const FAtlasBakeStats& AtlasStats, FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			TEXT("_BillboardCloudAtlas"),
			Pixels,
			AtlasStats,
			TC_Default,
			true,
			TEXT("No atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(const UStaticMesh& SourceStaticMesh, const TArray<FColor>& Pixels, const FAtlasBakeStats& AtlasStats, FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			TEXT("_BillboardCloudNormalAtlas"),
			Pixels,
			AtlasStats,
			TC_VectorDisplacementmap,
			false,
			TEXT("No normal atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(const UStaticMesh& SourceStaticMesh, const TArray<FColor>& Pixels, const FAtlasBakeStats& AtlasStats, FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			TEXT("_BillboardCloudMixAtlas"),
			Pixels,
			AtlasStats,
			TC_Masks,
			false,
			TEXT("No mix atlas pixels were generated."),
			OutError);
	}

	UMaterialInstanceConstant* CreateBillboardMaterialInstanceAsset(
		const UStaticMesh& SourceStaticMesh,
		UMaterialInstanceConstant* TemplateMaterialInstance,
		UTexture2D* AtlasTexture,
		UTexture2D* NormalAtlasTexture,
		UTexture2D* MixAtlasTexture,
		FString& OutError)
	{
		OutError.Reset();
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("Billboard material template instance is not set. Configure Billboard Material Template in Billboard Clouds settings.");
			return nullptr;
		}
		const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
		const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
		const FString BaseAssetName = SourceStaticMesh.GetName() + TEXT("_BillboardCloudMaterialInstance");
		const FString BasePackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *BaseAssetName);

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString UniquePackageName;
		FString UniqueAssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), UniquePackageName, UniqueAssetName);

		UPackage* Package = CreatePackage(*UniquePackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Could not create package %s."), *UniquePackageName);
			return nullptr;
		}

		Package->FullyLoad();
		UMaterialInstanceConstant* MaterialInstance = DuplicateObject<UMaterialInstanceConstant>(TemplateMaterialInstance, Package, *UniqueAssetName);
		if (!MaterialInstance)
		{
			OutError = FString::Printf(TEXT("Could not duplicate material instance %s."), *TemplateMaterialInstance->GetPathName());
			return nullptr;
		}

		MaterialInstance->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		MaterialInstance->ClearFlags(RF_Transient);
		MaterialInstance->PreEditChange(nullptr);
		if (AtlasTexture)
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("ColorOpacity"))), AtlasTexture);
		}
		if (NormalAtlasTexture)
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("NormalMask"))), NormalAtlasTexture);
		}
		if (MixAtlasTexture)
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Mix"))), MixAtlasTexture);
		}
		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(MaterialInstance);
		return MaterialInstance;
	}

	int32 EnsureBillboardProxyMaterialSlot(UStaticMesh& StaticMesh, UMaterialInterface* ProxyMaterial)
	{
		UMaterialInterface* Material = ProxyMaterial ? ProxyMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
		TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
		for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
		{
			FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
			if (StaticMaterial.MaterialSlotName == BillboardProxyMaterialSlotName
				|| StaticMaterial.ImportedMaterialSlotName == BillboardProxyMaterialSlotName)
			{
				StaticMaterial.MaterialInterface = Material;
				StaticMaterial.MaterialSlotName = BillboardProxyMaterialSlotName;
				StaticMaterial.ImportedMaterialSlotName = BillboardProxyMaterialSlotName;
				return MaterialIndex;
			}
		}

		return StaticMaterials.Add(FStaticMaterial(Material, BillboardProxyMaterialSlotName, BillboardProxyMaterialSlotName));
	}

	void ConfigureBillboardProxySourceModel(UStaticMesh& StaticMesh, const int32 LODIndex, const bool bPreserveExistingScreenSize = false)
	{
		if (!StaticMesh.IsSourceModelValid(LODIndex))
		{
			return;
		}

		FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(LODIndex);
		const float ExistingScreenSize = SourceModel.ScreenSize.Default;
		SourceModel.BuildSettings.bRecomputeNormals = false;
		SourceModel.BuildSettings.bRecomputeTangents = false;
		SourceModel.BuildSettings.bRemoveDegenerates = false;
		SourceModel.BuildSettings.bGenerateLightmapUVs = false;
		SourceModel.BuildSettings.SrcLightmapIndex = 0;
		SourceModel.BuildSettings.DstLightmapIndex = 0;
		SourceModel.BuildSettings.bUseFullPrecisionUVs = false;
		SourceModel.BuildSettings.DistanceFieldResolutionScale = 0.0f;
		SourceModel.ReductionSettings.PercentTriangles = 1.0f;
		SourceModel.ReductionSettings.PercentVertices = 1.0f;
		SourceModel.ReductionSettings.BaseLODModel = LODIndex;

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

	void KeepOnlyUvChannels(UStaticMesh& StaticMesh, const int32 LODIndex, const int32 DesiredChannelCount)
	{
		StaticMesh.SetLightMapCoordinateIndex(0);
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

		StaticMesh.SetLightMapCoordinateIndex(0);
	}

	void KeepOnlyUvChannels(UStaticMesh& StaticMesh, const int32 DesiredChannelCount)
	{
		KeepOnlyUvChannels(StaticMesh, 0, DesiredChannelCount);
	}

	UStaticMesh* CreateStaticMeshAssetFromDescription(const UStaticMesh& SourceStaticMesh, const FMeshDescription& MeshDescription, UMaterialInterface* ProxyMaterial, FString& OutError)
	{
		OutError.Reset();

		const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
		const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
		const FString BaseAssetName = SourceStaticMesh.GetName() + TEXT("_BillboardCloudProxy");
		const FString BasePackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *BaseAssetName);

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		FString UniquePackageName;
		FString UniqueAssetName;
		AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), UniquePackageName, UniqueAssetName);

		UPackage* Package = CreatePackage(*UniquePackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Could not create package %s."), *UniquePackageName);
			return nullptr;
		}

		Package->FullyLoad();

		UStaticMesh* ProxyMesh = NewObject<UStaticMesh>(Package, *UniqueAssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!ProxyMesh)
		{
			OutError = FString::Printf(TEXT("Could not create StaticMesh %s."), *UniqueAssetName);
			return nullptr;
		}

		ProxyMesh->InitResources();
		ProxyMesh->SetLightingGuid();

		EnsureBillboardProxyMaterialSlot(*ProxyMesh, ProxyMaterial);
		ProxyMesh->SetLightMapCoordinateIndex(0);
		ProxyMesh->SetLightMapResolution(64);
		ProxyMesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);

		TArray<const FMeshDescription*> MeshDescriptions;
		MeshDescriptions.Add(&MeshDescription);

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		BuildParams.bBuildSimpleCollision = false;
		BuildParams.bFastBuild = true;
		BuildParams.bCommitMeshDescription = true;
		BuildParams.bMarkPackageDirty = true;
		ProxyMesh->BuildFromMeshDescriptions(MeshDescriptions, BuildParams);

		ConfigureBillboardProxySourceModel(*ProxyMesh, 0);

		KeepOnlyUvChannels(*ProxyMesh, 3);
		ProxyMesh->PostEditChange();
		ProxyMesh->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(ProxyMesh);
		return ProxyMesh;
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

	bool InstallMeshDescriptionAsSourceMeshLOD(
		UStaticMesh& SourceStaticMesh,
		const FMeshDescription& MeshDescription,
		UMaterialInterface* ProxyMaterial,
		const EBillboardCloudsMeshOutputMode OutputMode,
		const int32 RequestedReplaceLODIndex,
		int32& OutLODIndex,
		FString& OutError)
	{
		OutLODIndex = INDEX_NONE;
		OutError.Reset();

		if (OutputMode == EBillboardCloudsMeshOutputMode::ReplaceSourceMeshLOD)
		{
			if (!SourceStaticMesh.IsSourceModelValid(RequestedReplaceLODIndex))
			{
				OutError = FString::Printf(
					TEXT("Cannot replace LOD %d on %s because that source LOD does not exist."),
					RequestedReplaceLODIndex,
					*SourceStaticMesh.GetName());
				return false;
			}
			OutLODIndex = RequestedReplaceLODIndex;
		}
		else if (OutputMode == EBillboardCloudsMeshOutputMode::AddToSourceMeshLOD)
		{
			OutLODIndex = SourceStaticMesh.GetNumSourceModels();
		}
		else
		{
			OutError = TEXT("InstallMeshDescriptionAsSourceMeshLOD requires a source-mesh LOD output mode.");
			return false;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		const bool bStaticMeshWasEdited = AssetEditorSubsystem && AssetEditorSubsystem->FindEditorForAsset(&SourceStaticMesh, false);
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

		SourceStaticMesh.Modify();
		SourceStaticMesh.PreEditChange(nullptr);

		if (OutputMode == EBillboardCloudsMeshOutputMode::AddToSourceMeshLOD)
		{
			SourceStaticMesh.AddSourceModel();
		}

		if (!SourceStaticMesh.IsSourceModelValid(OutLODIndex))
		{
			OutError = FString::Printf(TEXT("Could not allocate source LOD %d on %s."), OutLODIndex, *SourceStaticMesh.GetName());
			ReopenSourceMeshEditor();
			return false;
		}

		const int32 MaterialIndex = EnsureBillboardProxyMaterialSlot(SourceStaticMesh, ProxyMaterial);
		ClearSectionInfoForLOD(SourceStaticMesh, OutLODIndex);

		FMeshDescription MeshDescriptionCopy = MeshDescription;
		if (!SourceStaticMesh.CreateMeshDescription(OutLODIndex, MoveTemp(MeshDescriptionCopy)))
		{
			OutError = FString::Printf(TEXT("Could not create MeshDescription for source LOD %d on %s."), OutLODIndex, *SourceStaticMesh.GetName());
			ReopenSourceMeshEditor();
			return false;
		}

		UStaticMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = true;
		CommitParams.bUseHashAsGuid = false;
		SourceStaticMesh.CommitMeshDescription(OutLODIndex, CommitParams);

		ConfigureBillboardProxySourceModel(SourceStaticMesh, OutLODIndex, OutputMode == EBillboardCloudsMeshOutputMode::ReplaceSourceMeshLOD);
		KeepOnlyUvChannels(SourceStaticMesh, OutLODIndex, 3);

		FMeshSectionInfo SectionInfo;
		SectionInfo.MaterialIndex = MaterialIndex;
		SourceStaticMesh.GetSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
		SourceStaticMesh.GetOriginalSectionInfoMap().Set(OutLODIndex, 0, SectionInfo);
		SourceStaticMesh.SetLightMapCoordinateIndex(0);
		SourceStaticMesh.SetImportVersion(EImportStaticMeshVersion::LastVersion);
		SourceStaticMesh.PostEditChange();
		SourceStaticMesh.MarkPackageDirty();

		ReopenSourceMeshEditor();
		return true;
	}

	struct FProxyPlaneCoverBuildData
	{
		TArray<UE::BillboardClouds::FSourceTriangle> Triangles;
		UE::BillboardClouds::FPlaneCoverSettings Settings;
		FTrunkCardTriangleSplit TrunkSplit;
		UE::BillboardClouds::FPlaneCoverResult BillboardResult;
		UE::BillboardClouds::FPlaneCoverResult ProxyResult;
		bool bHasBillboardResult = false;
		int32 TrunkPlaneCount = 0;
	};

	struct FProxyMeshBuildData
	{
		FMeshDescription MeshDescription;
		UE::BillboardClouds::FPlaneProxyMeshStats Stats;
		TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo> PlaneInfos;
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

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		const FString MeshName = StaticMesh.GetName();
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *MeshName, *Error);
		UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FString BuildBillboardCloudsOrTrunkSummary(
		const UStaticMesh& StaticMesh,
		const FProxyPlaneCoverBuildData& CoverData)
	{
		FString Summary;
		if (CoverData.bHasBillboardResult)
		{
			Summary = UE::BillboardClouds::SummarizePlaneCover(StaticMesh.GetName(), CoverData.Settings, CoverData.BillboardResult);
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

	const TCHAR* GetTextureShootingMode(const UE::BillboardClouds::FPlaneCoverSettings& Settings)
	{
		if (Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::KMeansClustering)
		{
			return TEXT("GPU bake, per-plane tile UV, cluster projection");
		}
		if (Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::GodOfWarCards)
		{
			return TEXT("GPU bake, per-plane tile UV, card ortho bounds, closeness clipped, reclaimed faces only");
		}
		return TEXT("GPU bake, per-plane tile UV, valid-zone touching triangles included");
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const UBillboardCloudsEditorSettings& EditorSettings)
	{
		FAtlasOutputSelection OutputSelection;
		OutputSelection.bBaseColorOpacity = EditorSettings.bBakeBaseColorOpacityAtlas;
		OutputSelection.bNormalMask = EditorSettings.bBakeNormalMaskAtlas;
		OutputSelection.bMix = EditorSettings.bBakeMixAtlas;
		return OutputSelection;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const UBillboardCloudsEditorSettings& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		if (!UE::BillboardClouds::ExtractTrianglesFromStaticMesh(&StaticMesh, 0, OutData.Triangles, OutError))
		{
			return false;
		}

		OutData.Settings = BuildSettingsForMesh(StaticMesh, EditorSettings);
		const int32 RequestedTrunkPlaneCount = FMath::Clamp(EditorSettings.TrunkCardPlaneCount, 2, 4);
		OutData.TrunkSplit = SplitTrianglesForTrunkCards(StaticMesh, OutData.Triangles, EditorSettings.bEnableTrunkCards, EditorSettings.TrunkCardMaterialKeywords);

		if (!OutData.TrunkSplit.BillboardTriangles.IsEmpty())
		{
			OutData.BillboardResult = UE::BillboardClouds::BuildPlaneCover(OutData.TrunkSplit.BillboardTriangles, OutData.Settings);
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
		return UE::BillboardClouds::BuildPlaneProxyMeshDescription(
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
		const UBillboardCloudsEditorSettings& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable at least one of BaseColorOpacity, NormalMask, or Mix in Billboard Clouds settings.");
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
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				CropOutputSelection,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropStats);

			TArray<UE::BillboardClouds::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				CoverData.Settings.AlphaAwareTileCropGuardPixels,
				TileCrops);

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::BillboardClouds::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
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
			OutData.AtlasTexture = CreateAtlasTextureAsset(StaticMesh, OutData.AtlasPixels, OutData.AtlasStats, OutError);
			if (!OutData.AtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bNormalMask)
		{
			OutData.NormalAtlasTexture = CreateNormalAtlasTextureAsset(StaticMesh, OutData.NormalAtlasPixels, OutData.AtlasStats, OutError);
			if (!OutData.NormalAtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bMix)
		{
			OutData.MixAtlasTexture = CreateMixAtlasTextureAsset(StaticMesh, OutData.MixAtlasPixels, OutData.AtlasStats, OutError);
			if (!OutData.MixAtlasTexture)
			{
				return false;
			}
		}

		OutData.Material = CreateBillboardMaterialInstanceAsset(
			StaticMesh,
			EditorSettings.BillboardMaterialTemplate.LoadSynchronous(),
			OutData.AtlasTexture,
			OutData.NormalAtlasTexture,
			OutData.MixAtlasTexture,
			OutError);
		return OutData.Material != nullptr;
	}

	bool CreateProxyMeshAssetBundle(
		UStaticMesh& StaticMesh,
		const UBillboardCloudsEditorSettings& EditorSettings,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = EditorSettings.MeshOutputMode;

		if (EditorSettings.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset)
		{
			OutResult.ProxyMesh = CreateStaticMeshAssetFromDescription(StaticMesh, MeshData.MeshDescription, TextureData.Material, OutError);
			if (!OutResult.ProxyMesh)
			{
				return false;
			}
		}
		else
		{
			int32 InstalledLODIndex = INDEX_NONE;
			if (!InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				MeshData.MeshDescription,
				TextureData.Material,
				EditorSettings.MeshOutputMode,
				FMath::Max(0, EditorSettings.ReplaceSourceLODIndex),
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
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);

		// GPU bake diagnostics: one summary block per source material. Emitted verbatim
		// so both success + debugging surface here (not just Output Log).
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
		const FString TechniqueSummary = BuildBillboardCloudsOrTrunkSummary(StaticMesh, CoverData);
		const FString BaseAtlasPath = TextureData.AtlasTexture ? TextureData.AtlasTexture->GetPathName() : TEXT("disabled");
		const FString NormalAtlasPath = TextureData.NormalAtlasTexture ? TextureData.NormalAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MixAtlasPath = TextureData.MixAtlasTexture ? TextureData.MixAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EBillboardCloudsMeshOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions fast path")
			: TEXT("source StaticMesh LOD MeshDescription commit");

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, max padding=%d, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, readable material textures=%d, mix-texture materials=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, crack-reduction refs=%d, alpha refs=%d, masked refs=%d, gpu opacity mask refs=%d, gpu opacity mask failed refs=%d, gpu opacity clip zeroed pixels=%d, mix refs texture=%d, forced opaque=%d, shooting=%s, resolve=GPU material bake, side-aware per-plane material raster, far-to-near painter depth order\n  base/color opacity atlas: %s\n  normal atlas: %s, GPU-baked MP_Normal converted through matched source render-data TBN to object/local-space XYZ for all cards; two-sided tangent-space backfaces flip after decode; tangent-space normal maps, WPO and Custom nodes evaluated by real material pixel shader\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks, GPU-baked from material outputs\n  trunk/leaf mask: UV2 classification, trunk=(0,0), billboard/leaf=(1,0), trunk-white mask = 1 - UV2.x\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (copied from settings template; enabled parameters set: ColorOpacity, NormalMask, Mix)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, distance fields off\n  proxy winding: reversed UE front-face order, source-facing normals%s"),
			*TechniqueSummary,
			*AlphaPolicyDetails,
			*MeshOutputDetails,
			MeshData.Stats.PlaneCount,
			MeshData.Stats.QuadCount,
			MeshData.Stats.TriangleCount,
			TextureData.AtlasStats.Width,
			TextureData.AtlasStats.Height,
			TextureData.AtlasStats.TileResolution,
			TextureData.AtlasStats.TilePaddingPixels,
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
			GetTextureShootingMode(CoverData.Settings),
			*BaseAtlasPath,
			*NormalAtlasPath,
			*MixAtlasPath,
			*TextureData.Material->GetPathName(),
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails,
			*GpuBakeDiagnosticsBlock);
	}

	FProxyAssetBuildResult BuildBillboardCloudProxyAsset(
		UStaticMesh& StaticMesh,
		const UBillboardCloudsEditorSettings& EditorSettings)
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
		if (!BuildProxyTextureData(StaticMesh, EditorSettings, CoverData, MeshData, TextureData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(StaticMesh, EditorSettings, MeshData, TextureData, Result, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		Result.bSucceeded = true;
		Result.Report = BuildProxySuccessReport(StaticMesh, CoverData, MeshData, TextureData, Result);
		UE_LOG(LogBillboardCloudsEditor, Display, TEXT("\n%s"), *Result.Report);
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

void FBillboardCloudsEditorModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBillboardCloudsEditorModule::RegisterMenus));
}

void FBillboardCloudsEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FBillboardCloudsEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("BillboardClouds");
	Section.Label = LOCTEXT("BillboardCloudsSection", "Billboard Clouds");
	Section.AddMenuEntry(
		"BillboardCloudsCreatePlaneProxyMeshes",
		LOCTEXT("CreatePlaneProxyMeshesLabel", "Create Plane Proxy Meshes"),
		LOCTEXT("CreatePlaneProxyMeshesTooltip", "Create or install Static Mesh proxy geometry from the selected Billboard Clouds plane cover."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FBillboardCloudsEditorModule::ExecuteCreatePlaneProxyMeshes)
	);
}

void FBillboardCloudsEditorModule::ExecuteCreatePlaneProxyMeshes(const FToolMenuContext& MenuContext) const
{
	(void)MenuContext;

	const TArray<UStaticMesh*> StaticMeshes = GetSelectedStaticMeshes();

	if (StaticMeshes.IsEmpty())
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("NoStaticMeshSelectionForProxy", "Select one or more Static Mesh assets in the Content Browser, then run Tools > Billboard Clouds > Create Plane Proxy Meshes.")
		);
		return;
	}

	const UBillboardCloudsEditorSettings* EditorSettings = GetDefault<UBillboardCloudsEditorSettings>();
	FScopedSlowTask SlowTask(StaticMeshes.Num(), LOCTEXT("CreatePlaneProxyMeshesSlowTask", "Creating Billboard Clouds plane proxy meshes..."));
	SlowTask.MakeDialog();

	FString Report;
	TArray<UObject*> CreatedAssets;

	for (UStaticMesh* StaticMesh : StaticMeshes)
	{
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(StaticMesh->GetName()));

		const FProxyAssetBuildResult BuildResult = BuildBillboardCloudProxyAsset(*StaticMesh, *EditorSettings);
		Report += BuildResult.Report + TEXT("\n\n");
		if (BuildResult.bSucceeded)
		{
			AppendProxyCreatedAssets(BuildResult, CreatedAssets);
		}
	}

	if (!CreatedAssets.IsEmpty() && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBillboardCloudsEditorModule, BillboardCloudsEditor)
