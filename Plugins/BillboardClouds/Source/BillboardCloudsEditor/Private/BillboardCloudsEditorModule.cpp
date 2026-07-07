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
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TextureResource.h"
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
		case EBillboardCloudsCrackReductionMode::PaperExact:
			Settings.KMeansCrackReductionMode = UE::BillboardClouds::EKMeansCrackReductionMode::PaperExact;
			break;
		case EBillboardCloudsCrackReductionMode::BoundaryAware:
			Settings.KMeansCrackReductionMode = UE::BillboardClouds::EKMeansCrackReductionMode::BoundaryAware;
			break;
		case EBillboardCloudsCrackReductionMode::Off:
		default:
			Settings.KMeansCrackReductionMode = UE::BillboardClouds::EKMeansCrackReductionMode::Off;
			break;
		}
		Settings.KMeansBoundaryCrackReductionWidth = FMath::Clamp(EditorSettings.KMeansBoundaryCrackReductionWidthCm, 0.0, 200.0);
		Settings.GodOfWarGeodesicSubdivisions = FMath::Clamp(EditorSettings.GodOfWarGeodesicSubdivisions, 0, 5);
		Settings.GodOfWarCandidateSpacingMultiplier = FMath::Clamp(EditorSettings.GodOfWarCandidateSpacingMultiplier, 0.1, 8.0);
		Settings.TextureTilePaddingPixels = FMath::Clamp(EditorSettings.TextureTilePaddingPixels, 0, 128);
		Settings.TextureAtlasResolution = FMath::Clamp(EditorSettings.TextureAtlasResolution, 256, 8192);
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

	FString BuildTrunkCrossCardSummary(const FTrunkCardTriangleSplit& Split, const int32 TrunkPlaneCount)
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
			TEXT("\n  trunk cards: enabled, matched materials=%d, trunk triangles=%d, billboard input triangles=%d, vertical planes=%d (%s), origin-centered, shooting=horizontal ortho trunk-only"),
			Split.MatchedMaterialCount,
			Split.TrunkTriangleIndices.Num(),
			Split.BillboardTriangles.Num(),
			TrunkPlaneCount,
			LayoutName);
	}

	struct FTexturePixels
	{
		TArray64<uint8> Bytes;
		int32 Width = 0;
		int32 Height = 0;
		ETextureSourceFormat Format = TSF_Invalid;

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
			return FLinearColor(SampleRawColor(UV));
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
		FTexturePixels NormalTexture;
		FMaterialScalarBakeData AmbientOcclusion;
		FMaterialScalarBakeData Roughness;
		FMaterialScalarBakeData Metallic;
		FMaterialScalarBakeData Emission;
		bool bHasReadableBaseColorTexture = false;
		bool bHasReadableOpacityMaskTexture = false;
		bool bHasReadableNormalTexture = false;
		bool bUseTextureAlphaAsOpacity = false;
		bool bFlipNormalGreenChannel = false;
		bool bTwoSided = false;
		EBillboardOpacityMaskChannel OpacityMaskChannel = EBillboardOpacityMaskChannel::Alpha;
		float OpacityMaskClipValue = 0.33333334f;
		double OpacityMaskTransparentRatio = 0.0;
		FString OpacityMaskSource;
		FString NormalTextureSource;
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
		int32 SourceNormalMapMaterials = 0;
		int32 SourceNormalMapReferences = 0;
		int32 SourceMixTextureMaterials = 0;
		int32 SourceMixTextureReferences = 0;
		int32 TextureAlphaOpacityMaterials = 0;
		int32 TextureAlphaOpacityReferences = 0;
		int32 ForcedOpaqueAlphaReferences = 0;
		FString MaterialAlphaPolicyDetails;
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

	UTexture2D* FindNormalTextureFromParameter(
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
				OutSource = FString::Printf(TEXT("normal parameter %s -> %s"), *ParameterName.ToString(), *Texture2D->GetName());
				return Texture2D;
			}
		}

		return nullptr;
	}

	UTexture2D* FindNormalTexture(UMaterialInterface* MaterialInterface, FString& OutSource)
	{
		OutSource.Reset();
		if (!MaterialInterface)
		{
			return nullptr;
		}

		if (UMaterial* Material = MaterialInterface->GetMaterial())
		{
			if (FExpressionInput* NormalInput = Material->GetExpressionInputForProperty(MP_Normal))
			{
				const FExpressionInput TracedInput = NormalInput->GetTracedInput();
				if (TracedInput.Expression)
				{
					if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = FindNormalTextureFromParameter(MaterialInterface, TextureParameter->ParameterName, OutSource))
						{
							return Texture2D;
						}
					}

					if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(TracedInput.Expression))
					{
						if (UTexture2D* Texture2D = Cast<UTexture2D>(TextureSample->Texture))
						{
							OutSource = FString::Printf(TEXT("normal input texture %s"), *Texture2D->GetName());
							return Texture2D;
						}
					}
				}
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

	TArray<FMaterialBakeData> BuildMaterialBakeData(
		const UStaticMesh& SourceStaticMesh,
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
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

			FString NormalTextureSource;
			if (UTexture2D* NormalTexture = FindNormalTexture(MaterialInterface, NormalTextureSource))
			{
				BakeData.bHasReadableNormalTexture = TryLoadTexturePixels(NormalTexture, BakeData.NormalTexture);
				if (BakeData.bHasReadableNormalTexture && BakeData.NormalTexture.Format == TSF_BGRA8)
				{
					BakeData.bFlipNormalGreenChannel = NormalTexture->bFlipGreenChannel;
					BakeData.NormalTextureSource = NormalTextureSource;
					++InOutStats.ReadableMaterialTextures;
					++InOutStats.SourceNormalMapMaterials;
				}
				else
				{
					BakeData.bHasReadableNormalTexture = false;
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
			const FString NormalSource = BakeData.bHasReadableNormalTexture
				? FString::Printf(TEXT("source normal texture %s%s"), *BakeData.NormalTextureSource, BakeData.bFlipNormalGreenChannel ? TEXT(", flip green") : TEXT(""))
				: TEXT("geometry vertex normals");
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

	FLinearColor MakeFallbackTriangleColor(const FMaterialBakeData& BakeData)
	{
		return BakeData.BaseColor;
	}

	float SampleMaterialScalar(const FMaterialScalarBakeData& BakeData, const FVector2f& UV)
	{
		if (!BakeData.bHasReadableTexture)
		{
			return FMath::Clamp(BakeData.Constant, 0.0f, 1.0f);
		}

		const FColor Sample = BakeData.Texture.SampleRawColor(UV);
		const float R = static_cast<float>(Sample.R) / 255.0f;
		const float G = static_cast<float>(Sample.G) / 255.0f;
		const float B = static_cast<float>(Sample.B) / 255.0f;
		const float A = static_cast<float>(Sample.A) / 255.0f;
		if (BakeData.bUseLuminance)
		{
			return FMath::Clamp(FMath::Max3(R, G, B), 0.0f, 1.0f);
		}

		switch (BakeData.Channel)
		{
		case EBillboardOpacityMaskChannel::Red:
			return FMath::Clamp(R, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Green:
			return FMath::Clamp(G, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Blue:
			return FMath::Clamp(B, 0.0f, 1.0f);
		case EBillboardOpacityMaskChannel::Alpha:
			return FMath::Clamp(A, 0.0f, 1.0f);
		default:
			return FMath::Clamp(BakeData.Constant, 0.0f, 1.0f);
		}
	}

	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	FColor PackMixAtlasPixel(const FMaterialBakeData& BakeData, const FVector2f& SourceUV)
	{
		return FColor(
			UnitFloatToByte(SampleMaterialScalar(BakeData.AmbientOcclusion, SourceUV)),
			UnitFloatToByte(SampleMaterialScalar(BakeData.Roughness, SourceUV)),
			UnitFloatToByte(SampleMaterialScalar(BakeData.Metallic, SourceUV)),
			UnitFloatToByte(SampleMaterialScalar(BakeData.Emission, SourceUV)));
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

	FVector DecodeTangentSpaceNormalSample(const FTexturePixels& NormalTexture, const FVector2f& UV, const bool bFlipGreenChannel)
	{
		const FColor Sample = NormalTexture.SampleRawColor(UV);
		FVector TangentNormal(
			static_cast<double>(Sample.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Sample.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(Sample.B) / 255.0 * 2.0 - 1.0);
		if (bFlipGreenChannel)
		{
			TangentNormal.Y *= -1.0;
		}

		return TangentNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	FVector ComputeBakedObjectSpaceNormal(
		const UE::BillboardClouds::FSourceTriangle& Triangle,
		const FMaterialBakeData& BakeData,
		const FVector2f& SourceUV,
		const FVector& CaptureViewVector,
		const double W0,
		const double W1,
		const double W2)
	{
		FVector ObjectNormal = (Triangle.VertexNormals[0] * W0 + Triangle.VertexNormals[1] * W1 + Triangle.VertexNormals[2] * W2).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Triangle.ShadingNormal);
		if (ObjectNormal.IsNearlyZero())
		{
			ObjectNormal = Triangle.Normal;
		}

		const double SourceTwoSidedSign = BakeData.bTwoSided && FVector::DotProduct(Triangle.Normal, CaptureViewVector) < 0.0 ? -1.0 : 1.0;
		if (!Triangle.bHasUVs || !Triangle.bHasSourceTangents || !BakeData.bHasReadableNormalTexture)
		{
			return ObjectNormal * SourceTwoSidedSign;
		}

		FVector ObjectTangent = (Triangle.VertexTangents[0] * W0 + Triangle.VertexTangents[1] * W1 + Triangle.VertexTangents[2] * W2);
		ObjectTangent = (ObjectTangent - ObjectNormal * FVector::DotProduct(ObjectTangent, ObjectNormal)).GetSafeNormal();
		if (ObjectTangent.IsNearlyZero())
		{
			return ObjectNormal;
		}

		const double WeightedBinormalSign = static_cast<double>(Triangle.VertexBinormalSigns[0]) * W0
			+ static_cast<double>(Triangle.VertexBinormalSigns[1]) * W1
			+ static_cast<double>(Triangle.VertexBinormalSigns[2]) * W2;
		const double BinormalSign = WeightedBinormalSign >= 0.0 ? 1.0 : -1.0;
		const FVector ObjectBinormal = FVector::CrossProduct(ObjectNormal, ObjectTangent).GetSafeNormal() * BinormalSign;
		if (ObjectBinormal.IsNearlyZero())
		{
			return ObjectNormal;
		}

		const FVector TangentNormal = DecodeTangentSpaceNormalSample(BakeData.NormalTexture, SourceUV, BakeData.bFlipNormalGreenChannel);
		ObjectNormal = (ObjectTangent * TangentNormal.X + ObjectBinormal * TangentNormal.Y + ObjectNormal * TangentNormal.Z).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, ObjectNormal);
		return ObjectNormal * SourceTwoSidedSign;
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

	void RasterizeBillboardAtlas(
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

		TArray<double> DepthBuffer;
		DepthBuffer.Init(-TNumericLimits<double>::Max(), OutPixels.Num());
		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, OutPixels.Num());

		const TArray<FMaterialBakeData> MaterialBakeData = BuildMaterialBakeData(SourceStaticMesh, Triangles, OutStats);

		for (int32 PlaneInfoIndex = 0; PlaneInfoIndex < PlaneInfos.Num(); ++PlaneInfoIndex)
		{
			const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneInfoIndex];
			const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
			if (TileSize.X <= 0 || TileSize.Y <= 0 || FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU) || FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				continue;
			}

			auto RasterizeTriangleToPlane = [&](
				const int32 TriangleIndex,
				const UE::BillboardClouds::FPlaneProxyPlaneInfo* SourceEnvelopeClip,
				const bool bBoundaryAwareCrackReduction,
				const FIntPoint& TargetPixelMin,
				const FIntPoint& TargetTileSize,
				const bool bBackFace)
			{
				if (TargetTileSize.X <= 0 || TargetTileSize.Y <= 0)
				{
					return;
				}

				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					return;
				}

				const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[TriangleIndex];
				const int32 MaterialIndex = MaterialBakeData.IsValidIndex(Triangle.MaterialIndex) ? Triangle.MaterialIndex : 0;
				const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
				const bool bUseTexture = Triangle.bHasUVs && BakeData.bHasReadableBaseColorTexture;
				const bool bUseTextureAlphaAsOpacity = Triangle.bHasUVs && BakeData.bUseTextureAlphaAsOpacity && BakeData.bHasReadableOpacityMaskTexture;
				const bool bUseNormalTexture = Triangle.bHasUVs && Triangle.bHasSourceTangents && BakeData.bHasReadableNormalTexture;
				const bool bUseMixTexture = BakeData.AmbientOcclusion.bHasReadableTexture || BakeData.Roughness.bHasReadableTexture || BakeData.Metallic.bHasReadableTexture || BakeData.Emission.bHasReadableTexture;
				OutStats.SourceTexturedTriangles += bUseTexture ? 1 : 0;
				OutStats.FallbackTriangles += bUseTexture ? 0 : 1;
				OutStats.TextureAlphaOpacityReferences += bUseTextureAlphaAsOpacity ? 1 : 0;
				OutStats.SourceNormalMapReferences += OutputSelection.bNormalMask && bUseNormalTexture ? 1 : 0;
				OutStats.SourceMixTextureReferences += OutputSelection.bMix && bUseMixTexture ? 1 : 0;
				OutStats.ForcedOpaqueAlphaReferences += bUseTextureAlphaAsOpacity ? 0 : 1;
				++OutStats.RasterizedTriangleReferences;

				FVector2D ProjectedPoints[3];
				for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
				{
					const FVector& Vertex = Triangle.Vertices[VertexIndex];
					const FVector ProjectedVertex = UE::BillboardClouds::ProjectPointToPlane(Vertex, PlaneInfo.Normal, PlaneInfo.Rho);
					const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / (PlaneInfo.MaxU - PlaneInfo.MinU);
					const double VFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / (PlaneInfo.MaxV - PlaneInfo.MinV);
					ProjectedPoints[VertexIndex] = FVector2D(
						TargetPixelMin.X + UFraction * TargetTileSize.X,
						TargetPixelMin.Y + VFraction * TargetTileSize.Y);
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), TargetPixelMin.X, TargetPixelMin.X + TargetTileSize.X - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), TargetPixelMin.X, TargetPixelMin.X + TargetTileSize.X - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), TargetPixelMin.Y, TargetPixelMin.Y + TargetTileSize.Y - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), TargetPixelMin.Y, TargetPixelMin.Y + TargetTileSize.Y - 1);
				const FLinearColor FallbackColor = MakeFallbackTriangleColor(BakeData);

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

						const FVector SourcePoint = Triangle.Vertices[0] * W0 + Triangle.Vertices[1] * W1 + Triangle.Vertices[2] * W2;
						const double SignedPlaneDistance = FVector::DotProduct(PlaneInfo.Normal, SourcePoint) - PlaneInfo.Rho;
						if (SourceEnvelopeClip
							&& (!IsPointInsidePlaneProxyEnvelope(PlaneInfo, SourcePoint, 1.0e-4)
								|| !IsPointInsidePlaneProxyEnvelope(*SourceEnvelopeClip, SourcePoint, 1.0e-4)
								|| (bBoundaryAwareCrackReduction
									&& !IsPointNearPlaneProxyEnvelopeBoundary(PlaneInfo, SourcePoint, Settings.KMeansBoundaryCrackReductionWidth, 1.0e-4)
									&& !IsPointNearPlaneProxyEnvelopeBoundary(*SourceEnvelopeClip, SourcePoint, Settings.KMeansBoundaryCrackReductionWidth, 1.0e-4))))
						{
							continue;
						}
						if (!PlaneInfo.bIsTrunkCard
							&& (Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::PlaneSpaceGreedy
								|| Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::GodOfWarCards)
							&& !UE::BillboardClouds::IsPointWithinPlaneError(SourcePoint, PlaneInfo.Normal, PlaneInfo.Rho, Settings))
						{
							continue;
						}

						const int32 PixelIndex = Y * OutStats.Width + X;
						const double TextureDepth = bBackFace ? -SignedPlaneDistance : SignedPlaneDistance;
						if (!DepthBuffer.IsValidIndex(PixelIndex) || TextureDepth < DepthBuffer[PixelIndex])
						{
							continue;
						}

						FLinearColor LinearColor = FallbackColor;
						const FVector2f SourceUV = Triangle.UVs[0] * static_cast<float>(W0)
							+ Triangle.UVs[1] * static_cast<float>(W1)
							+ Triangle.UVs[2] * static_cast<float>(W2);
						if (bUseTexture)
						{
							LinearColor = BakeData.BaseColorTexture.Sample(SourceUV) * BakeData.BaseColor;
						}

						const float PixelAlpha = 1.0f;
						if (bUseTextureAlphaAsOpacity)
						{
							const float MaskValue = SampleOpacityMaskValue(BakeData.OpacityMaskTexture, SourceUV, BakeData.OpacityMaskChannel);
							if (MaskValue < BakeData.OpacityMaskClipValue)
							{
								continue;
							}
						}

						LinearColor.R *= PixelAlpha;
						LinearColor.G *= PixelAlpha;
						LinearColor.B *= PixelAlpha;
						OutPixels[PixelIndex] = LinearColor.ToFColorSRGB();
						OutPixels[PixelIndex].A = static_cast<uint8>(FMath::RoundToInt(PixelAlpha * 255.0f));
						AtlasCoverage[PixelIndex] = true;
						if (OutputSelection.bNormalMask && OutNormalPixels.IsValidIndex(PixelIndex))
						{
							const FVector CaptureViewVector = bBackFace ? -PlaneInfo.Normal : PlaneInfo.Normal;
							const FVector ObjectNormal = ComputeBakedObjectSpaceNormal(Triangle, BakeData, SourceUV, CaptureViewVector, W0, W1, W2);
							OutNormalPixels[PixelIndex] = EncodeObjectSpaceNormalToColor(ObjectNormal, OutPixels[PixelIndex].A);
						}
						if (OutputSelection.bMix && OutMixPixels.IsValidIndex(PixelIndex))
						{
							OutMixPixels[PixelIndex] = PackMixAtlasPixel(BakeData, SourceUV);
						}
						DepthBuffer[PixelIndex] = TextureDepth;
					}
				}
			};

			TBitArray<> QueuedTriangles;
			QueuedTriangles.Init(false, Triangles.Num());

			TArray<int32> TextureTriangleIndices;
			TextureTriangleIndices.Reserve(PlaneInfo.TriangleIndices.Num());
			for (const int32 TriangleIndex : PlaneInfo.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
				{
					QueuedTriangles[TriangleIndex] = true;
					TextureTriangleIndices.Add(TriangleIndex);
				}
			}

			if (!PlaneInfo.bIsTrunkCard
				&& Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::PlaneSpaceGreedy)
			{
				for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
				{
					if (!QueuedTriangles[TriangleIndex]
						&& !Triangles[TriangleIndex].bTrunkCardOnly
						&& UE::BillboardClouds::DoesTriangleIntersectPlaneValidZone(Triangles[TriangleIndex], PlaneInfo.Normal, PlaneInfo.Rho, Settings))
					{
						QueuedTriangles[TriangleIndex] = true;
						TextureTriangleIndices.Add(TriangleIndex);
					}
				}
			}

			for (const int32 TriangleIndex : TextureTriangleIndices)
			{
				RasterizeTriangleToPlane(TriangleIndex, nullptr, false, PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, false);
				if (PlaneInfo.bHasBackFaceAtlas)
				{
					RasterizeTriangleToPlane(TriangleIndex, nullptr, false, PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, true);
				}
			}

			if (!PlaneInfo.bIsTrunkCard
				&& Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::KMeansClustering
				&& !PlaneInfo.CrackReductionProjections.IsEmpty())
			{
				TSet<uint64> CrackQueuedProjectionKeys;
				for (const UE::BillboardClouds::FCrackReductionProjection& Projection : PlaneInfo.CrackReductionProjections)
				{
					const int32 TriangleIndex = Projection.TriangleIndex;
					if (!Triangles.IsValidIndex(TriangleIndex)
						|| QueuedTriangles[TriangleIndex]
						|| !PlaneInfos.IsValidIndex(Projection.SourcePlaneInfoIndex)
						|| Projection.SourcePlaneInfoIndex == PlaneInfoIndex)
					{
						continue;
					}

					const uint64 ProjectionKey = (static_cast<uint64>(static_cast<uint32>(Projection.SourcePlaneInfoIndex)) << 32)
						| static_cast<uint64>(static_cast<uint32>(TriangleIndex));
					if (CrackQueuedProjectionKeys.Contains(ProjectionKey))
					{
						continue;
					}

					CrackQueuedProjectionKeys.Add(ProjectionKey);
					++OutStats.CrackReductionTriangleReferences;
					RasterizeTriangleToPlane(TriangleIndex, &PlaneInfos[Projection.SourcePlaneInfoIndex], Projection.bBoundaryAware, PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, false);
					if (PlaneInfo.bHasBackFaceAtlas)
					{
						RasterizeTriangleToPlane(TriangleIndex, &PlaneInfos[Projection.SourcePlaneInfoIndex], Projection.bBoundaryAware, PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, true);
					}
				}
			}
		}

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

		for (const FColor& Pixel : OutPixels)
		{
			OutStats.PaintedPixels += Pixel.A > 0 ? 1 : 0;
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

		Summary += BuildTrunkCrossCardSummary(CoverData.TrunkSplit, CoverData.TrunkPlaneCount);
		return Summary;
	}

	const TCHAR* GetTextureShootingMode(const UE::BillboardClouds::FPlaneCoverSettings& Settings)
	{
		if (Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::KMeansClustering)
		{
			return TEXT("cluster projection");
		}
		if (Settings.Technique == UE::BillboardClouds::EPlaneCoverTechnique::GodOfWarCards)
		{
			return TEXT("card ortho bounds, closeness clipped, reclaimed faces only");
		}
		return TEXT("valid-zone clipped");
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
		const FProxyMeshBuildData& MeshData,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable at least one of BaseColorOpacity, NormalMask, or Mix in Billboard Clouds settings.");
			return false;
		}

		RasterizeBillboardAtlas(
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
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, max padding=%d, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, readable material textures=%d, normal-map materials=%d, mix-texture materials=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, crack-reduction refs=%d, alpha refs texture=%d, normal refs texture=%d, mix refs texture=%d, forced opaque=%d, shooting=%s\n  base/color opacity atlas: %s\n  normal atlas: %s, object-space XYZ, linear vector texture, same UV0/UV1 front/back layout as color atlas\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks\n  trunk/leaf mask: UV2 classification, trunk=(0,0), billboard/leaf=(1,0), trunk-white mask = 1 - UV2.x\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (copied from settings template; enabled parameters set: ColorOpacity, NormalMask, Mix)\n  normal source triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, distance fields off\n  proxy winding: reversed UE front-face order, source-facing normals"),
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
			TextureData.AtlasStats.ReadableMaterialTextures,
			TextureData.AtlasStats.SourceNormalMapMaterials,
			TextureData.AtlasStats.SourceMixTextureMaterials,
			TextureData.AtlasStats.TextureAlphaOpacityMaterials,
			TextureData.AtlasStats.SourceTexturedTriangles,
			TextureData.AtlasStats.FallbackTriangles,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.CrackReductionTriangleReferences,
			TextureData.AtlasStats.TextureAlphaOpacityReferences,
			TextureData.AtlasStats.SourceNormalMapReferences,
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
			MeshBuildPathDetails);
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


