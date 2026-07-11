#include "FoliageBakerCardBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaterialBaker.h"
#include "FoliageBakerPlaneCover.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "MaterialBakingStructures.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerCardsCore, Log, All);

namespace
{
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

	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const FFoliageBakerCardBakeRequest& Request)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		Settings.TextureAtlasResolution = FMath::Clamp(Request.TextureResolution, 256, 4096);
		Settings.SourceMaterialBakeResolution = FMath::Clamp(Request.SourceMaterialBakeResolution, 256, 4096);
		Settings.DoubleSidedBakeMode = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes
			: UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
		Settings.TrunkCardAtlasScale = 1.0;
		FBoxSphereBounds SourceBounds(ForceInitToZero);
		ComputeSourceTriangleBounds(SourceTriangles, SourceBounds);
		Settings.ErrorTolerance = FMath::Max(0.01, static_cast<double>(SourceBounds.SphereRadius) * 1.0e-6);
		Settings.bEnableAlphaAwareTileCrop = true;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 0, 16);
		return Settings;
	}

	struct FTrunkLeafClassification
	{
		int32 MatchedMaterialCount = 0;
		int32 TrunkTriangleCount = 0;
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

	FTrunkLeafClassification ClassifyTrianglesForTrunkLeafMask(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<FString>& RawKeywords)
	{
		FTrunkLeafClassification Classification;
		const TArray<FString> Keywords = BuildNormalizedKeywords(RawKeywords);
		const bool bClassificationEnabled = !Keywords.IsEmpty();

		TArray<uint8> bMaterialIsTrunk;
		const TArray<FStaticMaterial>& SourceMaterials = StaticMesh.GetStaticMaterials();
		bMaterialIsTrunk.SetNumZeroed(FMath::Max(1, SourceMaterials.Num()));
		if (bClassificationEnabled)
		{
			for (int32 MaterialIndex = 0; MaterialIndex < bMaterialIsTrunk.Num(); ++MaterialIndex)
			{
				const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (DoesMaterialOrParentNameMatchKeywords(MaterialInterface, Keywords))
				{
					bMaterialIsTrunk[MaterialIndex] = 1;
					++Classification.MatchedMaterialCount;
				}
			}
		}

		for (UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : SourceTriangles)
		{
			const bool bIsTrunk = bClassificationEnabled
				&& bMaterialIsTrunk.IsValidIndex(Triangle.MaterialIndex)
				&& bMaterialIsTrunk[Triangle.MaterialIndex] != 0;
			Triangle.bTrunkCardOnly = bIsTrunk;
			if (bIsTrunk)
			{
				++Classification.TrunkTriangleCount;
			}
		}

		return Classification;
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
		FTexturePixels NormalTexture;
		FTexturePixels OpacityMaskTexture;
		FMaterialScalarBakeData AmbientOcclusion;
		FMaterialScalarBakeData Roughness;
		FMaterialScalarBakeData Metallic;
		FMaterialScalarBakeData Emission;
		bool bHasReadableBaseColorTexture = false;
		bool bHasReadableNormalTexture = false;
		bool bHasReadableOpacityMaskTexture = false;
		bool bUseTextureAlphaAsOpacity = false;
		bool bTwoSided = false;
		bool bSourceTangentSpaceNormal = true;
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
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 SourceTexturedTriangles = 0;
		int32 FallbackTriangles = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 ReadableMaterialTextures = 0;
		int32 SourceMixTextureMaterials = 0;
		int32 SourceMixTextureReferences = 0;
		int32 TextureAlphaOpacityMaterials = 0;
		int32 TextureAlphaOpacityReferences = 0;
		int32 ForcedOpaqueAlphaReferences = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
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
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const int32 MaterialIndex,
		const FMaterialBakeData& BakeData)
	{
		if (!BakeData.bHasReadableOpacityMaskTexture)
		{
			return 0.0;
		}

		double TotalWeight = 0.0;
		double TransparentWeight = 0.0;
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
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
		const int32 SourceLODIndex,
		const FBoxSphereBounds& SourceLODBounds,
		const int32 MaterialIndex,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		UMaterialInterface* MaterialInterface,
		const FAtlasOutputSelection& OutputSelection,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const bool bBakeNormalTexture,
		FMaterialBakeData& InOutBakeData)
	{
		if (!MaterialInterface)
		{
			return false;
		}

		const FMeshDescription* SourceMeshDescription = SourceStaticMesh.GetMeshDescription(SourceLODIndex);
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
		if (bBakeNormalTexture)
		{
			AddProperty(MP_Normal);
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
		const FStaticMeshConstAttributes SourceAttributes(*SourceMeshDescription);
		const int32 SourceUVChannelCount = FMath::Max(1, SourceAttributes.GetVertexInstanceUVs().GetNumChannels());
		MeshSettings.LightMapIndex = FMath::Clamp(SourceStaticMesh.GetLightMapCoordinateIndex(), 0, SourceUVChannelCount - 1);
		MeshSettings.PrimitiveData = FPrimitiveData(SourceLODBounds);

		TArray<FMaterialData*> MaterialSettingPtrs;
		MaterialSettingPtrs.Add(&MaterialSettings);
		TArray<FMeshData*> MeshSettingPtrs;
		MeshSettingPtrs.Add(&MeshSettings);

		TArray<FBakeOutput> BakeOutputs;
		if (!FFoliageBakerMaterialBaker::BakeMaterials(MaterialSettingPtrs, MeshSettingPtrs, BakeOutputs))
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
		if (CopyBakedPropertyToTexture(BakeOutput, MP_Normal, InOutBakeData.NormalTexture))
		{
			InOutBakeData.bHasReadableNormalTexture = true;
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
		const int32 SourceLODIndex,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const FAtlasOutputSelection& OutputSelection,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const bool bBakeNormalTexture,
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
			const UMaterial* SourceMaterial = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			BakeData.bSourceTangentSpaceNormal = !SourceMaterial || SourceMaterial->bTangentSpaceNormal;
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
			const bool bHadNormalBeforeEvaluatedBake = BakeData.bHasReadableNormalTexture;
			ApplyEvaluatedMaterialOutputs(SourceStaticMesh, SourceLODIndex, SourceLODBounds, MaterialIndex, Triangles, MaterialInterface, OutputSelection, Settings, bBakeNormalTexture, BakeData);
			if (!bHadNormalBeforeEvaluatedBake && BakeData.bHasReadableNormalTexture)
			{
				++InOutStats.ReadableMaterialTextures;
			}
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

	struct FCardVisibleFragment
	{
		FVector2f Barycentric01 = FVector2f::ZeroVector;
		float CaptureDepth = TNumericLimits<float>::Max();
		int32 TriangleIndex = INDEX_NONE;
		uint8 ClassificationAlpha = 255;

		bool IsValid() const
		{
			return TriangleIndex != INDEX_NONE;
		}
	};

	struct FSharedCaptureDepthRange
	{
		double MinDepth = 0.0;
		double MaxDepth = 1.0;

		uint8 Encode(const double CaptureDepth) const
		{
			const double Extent = MaxDepth - MinDepth;
			const double LinearDepth = Extent > UE_DOUBLE_SMALL_NUMBER
				? FMath::Clamp((CaptureDepth - MinDepth) / Extent, 0.0, 1.0)
				: 0.0;
			return static_cast<uint8>(FMath::RoundToInt(LinearDepth * 255.0));
		}
	};

	FSharedCaptureDepthRange ComputeSharedCaptureDepthRange(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos)
	{
		double MinDepth = TNumericLimits<double>::Max();
		double MaxDepth = TNumericLimits<double>::Lowest();
		auto AccumulateDirection = [&](const FVector& InCaptureRayDirection)
		{
			const FVector CaptureRayDirection = InCaptureRayDirection.GetSafeNormal();
			if (CaptureRayDirection.IsNearlyZero())
			{
				return;
			}
			for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
			{
				for (const FVector& Vertex : Triangle.Vertices)
				{
					const double CaptureDepth = FVector::DotProduct(Vertex, CaptureRayDirection);
					MinDepth = FMath::Min(MinDepth, CaptureDepth);
					MaxDepth = FMath::Max(MaxDepth, CaptureDepth);
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateDirection(-PlaneInfo.Normal);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateDirection(PlaneInfo.Normal);
			}
		}

		FSharedCaptureDepthRange Result;
		if (FMath::IsFinite(MinDepth) && FMath::IsFinite(MaxDepth) && MaxDepth >= MinDepth)
		{
			Result.MinDepth = MinDepth;
			Result.MaxDepth = MaxDepth;
		}
		return Result;
	}


	void BakeCardAtlasOrthographic(
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
		auto AccumulateTileStats = [&](const FIntPoint& TileSize, const int32 Padding, const bool bBackFace)
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
			const int32 Padding = FMath::Max(0, PlaneInfo.AtlasTilePaddingPixels);
			AccumulateTileStats(PlaneInfo.AtlasTileSize, Padding, false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileStats(PlaneInfo.BackAtlasTileSize, Padding, true);
			}
		}
		OutStats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, AtlasPixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, AtlasPixelCount);
		const FSharedCaptureDepthRange SharedDepthRange = ComputeSharedCaptureDepthRange(Triangles, PlaneInfos);
		FAtlasOutputSelection SourcePropertySelection = OutputSelection;


		SourcePropertySelection.bBaseColorOpacity = true;
		const TArray<FMaterialBakeData> MaterialBakeData = BuildMaterialBakeData(
			SourceStaticMesh,
			SourceLODIndex,
			SourceLODBounds,
			Triangles,
			SourcePropertySelection,
			Settings,
			true,
			OutStats);
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0
				|| FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				return;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			TArray<FCardVisibleFragment> VisibleFragments;
			VisibleFragments.SetNum(TilePixelCount);
			const double UExtent = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, UE_DOUBLE_SMALL_NUMBER);
			const double VExtent = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, UE_DOUBLE_SMALL_NUMBER);

			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}
				const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
				if (Triangle.Area <= 0.0
					|| FVector::CrossProduct(
						Triangle.Vertices[1] - Triangle.Vertices[0],
						Triangle.Vertices[2] - Triangle.Vertices[0]).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}

				const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialBakeData.Num() - 1);
				if (!MaterialBakeData.IsValidIndex(MaterialIndex))
				{
					continue;
				}
				const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
				++OutStats.RasterizedTriangleReferences;
				if (BakeData.bHasReadableBaseColorTexture)
				{
					++OutStats.SourceTexturedTriangles;
				}
				else
				{
					++OutStats.FallbackTriangles;
				}
				if (BakeData.bUseTextureAlphaAsOpacity && BakeData.bHasReadableOpacityMaskTexture)
				{
					++OutStats.TextureAlphaOpacityReferences;
				}
				else
				{
					++OutStats.ForcedOpaqueAlphaReferences;
				}
				if (OutputSelection.bMix
					&& (BakeData.AmbientOcclusion.bHasReadableTexture
						|| BakeData.Roughness.bHasReadableTexture
						|| BakeData.Metallic.bHasReadableTexture
						|| BakeData.Emission.bHasReadableTexture))
				{
					++OutStats.SourceMixTextureReferences;
				}
				const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
					? SourceMaterials[MaterialIndex].MaterialInterface
					: nullptr;
				if (MaterialInterface && MaterialInterface->GetBlendMode() == BLEND_Masked)
				{
					++OutStats.MaskedMaterialBakeReferences;
				}

				FVector2D ProjectedPoints[3];
				for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
				{
					const FVector ProjectedVertex = UE::FoliageBaker::PlaneCover::ProjectPointToPlane(
						Triangle.Vertices[VertexIndex],
						PlaneInfo.Normal,
						PlaneInfo.Rho);
					const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
					const double PlaneVFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
					ProjectedPoints[VertexIndex] = FVector2D(
						UFraction * TileSize.X,
						(1.0 - PlaneVFraction) * TileSize.Y);
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
						if (!ComputeBarycentric2D(
							FVector2D(X + 0.5, Y + 0.5),
							ProjectedPoints[0],
							ProjectedPoints[1],
							ProjectedPoints[2],
							W0,
							W1,
							W2))
						{
							continue;
						}

						const FVector2f SourceUV = Triangle.bHasUVs
							? Triangle.UVs[0] * static_cast<float>(W0)
								+ Triangle.UVs[1] * static_cast<float>(W1)
								+ Triangle.UVs[2] * static_cast<float>(W2)
							: FVector2f::ZeroVector;
						const uint8 ClassificationAlpha = Triangle.bTrunkCardOnly ? 128 : 255;
						if (Triangle.bHasUVs
							&& BakeData.bUseTextureAlphaAsOpacity
							&& BakeData.bHasReadableOpacityMaskTexture
							&& BakeData.OpacityMaskTexture.IsValid())
						{
							const float Opacity = SampleOpacityMaskValue(
								BakeData.OpacityMaskTexture,
								SourceUV,
								BakeData.OpacityMaskChannel);
							if (Opacity < BakeData.OpacityMaskClipValue)
							{
								continue;
							}
						}

						const FVector SourcePoint = Triangle.Vertices[0] * W0
							+ Triangle.Vertices[1] * W1
							+ Triangle.Vertices[2] * W2;
						const float CaptureDepth = static_cast<float>(FVector::DotProduct(SourcePoint, CaptureRayDirection));
						const int32 PixelIndex = Y * TileSize.X + X;
						if (!VisibleFragments.IsValidIndex(PixelIndex)
							|| CaptureDepth > VisibleFragments[PixelIndex].CaptureDepth + 1.0e-6f)
						{
							continue;
						}

						FCardVisibleFragment& Fragment = VisibleFragments[PixelIndex];
						Fragment.Barycentric01 = FVector2f(static_cast<float>(W0), static_cast<float>(W1));
						Fragment.CaptureDepth = CaptureDepth;
						Fragment.TriangleIndex = TriangleIndex;
						Fragment.ClassificationAlpha = ClassificationAlpha;
					}
				}
			}

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
					if (!VisibleFragments.IsValidIndex(TilePixelIndex)
						|| !VisibleFragments[TilePixelIndex].IsValid())
					{
						continue;
					}
					const FCardVisibleFragment& Fragment = VisibleFragments[TilePixelIndex];
					if (!Triangles.IsValidIndex(Fragment.TriangleIndex))
					{
						continue;
					}
					const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[Fragment.TriangleIndex];
					const int32 MaterialIndex = FMath::Clamp(Triangle.MaterialIndex, 0, MaterialBakeData.Num() - 1);
					if (!MaterialBakeData.IsValidIndex(MaterialIndex))
					{
						continue;
					}
					const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
					const double W0 = Fragment.Barycentric01.X;
					const double W1 = Fragment.Barycentric01.Y;
					const double W2 = 1.0 - W0 - W1;
					const FVector2f SourceUV = Triangle.bHasUVs
						? Triangle.UVs[0] * static_cast<float>(W0)
							+ Triangle.UVs[1] * static_cast<float>(W1)
							+ Triangle.UVs[2] * static_cast<float>(W2)
						: FVector2f::ZeroVector;
					const bool bFlipTwoSidedBackFaceOutputNormal = BakeData.bTwoSided
						&& BakeData.bSourceTangentSpaceNormal
						&& FVector::DotProduct(Triangle.Normal, CaptureRayDirection) < 0.0;
					FNormalBakeBasisSample Basis = MakeNormalBakeBasisSample(
						Triangle,
						W0,
						W1,
						W2,
						bFlipTwoSidedBackFaceOutputNormal);
					Basis.CaptureDepth = Fragment.CaptureDepth;
					const int32 AtlasPixelIndex = AtlasY * OutStats.Width + AtlasX;

					if (OutputSelection.bBaseColorOpacity)
					{
						const FLinearColor BaseColor = BakeData.bHasReadableBaseColorTexture
							? BakeData.BaseColorTexture.Sample(SourceUV)
							: BakeData.BaseColor;
						FColor Color = BaseColor.ToFColorSRGB();
						Color.A = Fragment.ClassificationAlpha;
						OutPixels[AtlasPixelIndex] = Color;
					}
					if (AtlasCoverage.IsValidIndex(AtlasPixelIndex))
					{
						AtlasCoverage[AtlasPixelIndex] = true;
					}

					if (OutputSelection.bNormalMask)
					{
						const uint8 EncodedDepth = SharedDepthRange.Encode(Basis.CaptureDepth);
						OutNormalPixels[AtlasPixelIndex] = BakeData.bHasReadableNormalTexture
							? EncodeBakedTangentSpaceNormalToObjectSpaceColor(
								BakeData.NormalTexture.SampleRawColor(SourceUV),
								Basis,
								EncodedDepth)
							: EncodeObjectSpaceNormalToColor(
								Basis.Normal * static_cast<double>(Basis.OutputNormalSign),
								EncodedDepth);
						if (NormalCoverage.IsValidIndex(AtlasPixelIndex))
						{
							NormalCoverage[AtlasPixelIndex] = true;
						}
					}

					if (OutputSelection.bMix)
					{
						OutMixPixels[AtlasPixelIndex] = FColor(
							UnitFloatToByte(SampleMaterialScalar(BakeData.AmbientOcclusion, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Roughness, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Metallic, SourceUV)),
							UnitFloatToByte(SampleMaterialScalar(BakeData.Emission, SourceUV)));
					}
					++OutStats.PaintedPixels;
				}
			}
		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			BakePlaneAndSide(
				PlaneInfo,
				PlaneInfo.AtlasPixelMin,
				PlaneInfo.AtlasTileSize,
				-PlaneInfo.Normal);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal);
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

	bool TrimUnusedAtlasSpace(
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FMeshDescription& MeshDescription,
		TArray<FColor>& AtlasPixels,
		TArray<FColor>& NormalAtlasPixels,
		TArray<FColor>& MixAtlasPixels,
		FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		const int32 OldWidth = AtlasStats.Width;
		const int32 OldHeight = AtlasStats.Height;
		if (OldWidth <= 0 || OldHeight <= 0 || PlaneInfos.IsEmpty())
		{
			return true;
		}

		int32 UsedMinX = OldWidth;
		int32 UsedMinY = OldHeight;
		int32 UsedMaxX = 0;
		int32 UsedMaxY = 0;
		auto AccumulateTileBounds = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize, const int32 Padding)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			const int32 SafePadding = FMath::Max(0, Padding);
			UsedMinX = FMath::Min(UsedMinX, FMath::Max(0, PixelMin.X - SafePadding));
			UsedMinY = FMath::Min(UsedMinY, FMath::Max(0, PixelMin.Y - SafePadding));
			UsedMaxX = FMath::Max(UsedMaxX, FMath::Min(OldWidth, PixelMin.X + TileSize.X + SafePadding));
			UsedMaxY = FMath::Max(UsedMaxY, FMath::Min(OldHeight, PixelMin.Y + TileSize.Y + SafePadding));
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateTileBounds(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileBounds(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			}
		}
		if (UsedMaxX <= UsedMinX || UsedMaxY <= UsedMinY)
		{
			OutError = TEXT("Could not determine the used atlas bounds for unused-space trimming.");
			return false;
		}



		constexpr int32 TextureBlockSize = 4;
		const int32 CropMinX = FMath::Clamp((UsedMinX / TextureBlockSize) * TextureBlockSize, 0, OldWidth - 1);
		const int32 CropMinY = FMath::Clamp((UsedMinY / TextureBlockSize) * TextureBlockSize, 0, OldHeight - 1);
		const int32 CropMaxX = FMath::Clamp(((UsedMaxX + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize, CropMinX + 1, OldWidth);
		const int32 CropMaxY = FMath::Clamp(((UsedMaxY + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize, CropMinY + 1, OldHeight);
		const int32 NewWidth = CropMaxX - CropMinX;
		const int32 NewHeight = CropMaxY - CropMinY;
		if (CropMinX == 0 && CropMinY == 0 && NewWidth == OldWidth && NewHeight == OldHeight)
		{
			return true;
		}

		auto BuildCroppedPixels = [&](const TArray<FColor>& SourcePixels, TArray<FColor>& OutCroppedPixels) -> bool
		{
			OutCroppedPixels.Reset();
			if (SourcePixels.IsEmpty())
			{
				return true;
			}
			if (SourcePixels.Num() != OldWidth * OldHeight)
			{
				return false;
			}
			OutCroppedPixels.SetNumUninitialized(NewWidth * NewHeight);
			for (int32 Y = 0; Y < NewHeight; ++Y)
			{
				const FColor* SourceRow = SourcePixels.GetData() + (CropMinY + Y) * OldWidth + CropMinX;
				FColor* DestinationRow = OutCroppedPixels.GetData() + Y * NewWidth;
				FMemory::Memcpy(DestinationRow, SourceRow, static_cast<SIZE_T>(NewWidth) * sizeof(FColor));
			}
			return true;
		};

		TArray<FColor> CroppedAtlasPixels;
		TArray<FColor> CroppedNormalAtlasPixels;
		TArray<FColor> CroppedMixAtlasPixels;
		if (!BuildCroppedPixels(AtlasPixels, CroppedAtlasPixels)
			|| !BuildCroppedPixels(NormalAtlasPixels, CroppedNormalAtlasPixels)
			|| !BuildCroppedPixels(MixAtlasPixels, CroppedMixAtlasPixels))
		{
			OutError = TEXT("Atlas pixel count did not match the atlas dimensions during unused-space trimming.");
			return false;
		}

		FStaticMeshAttributes MeshAttributes(MeshDescription);
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();
		if (VertexInstanceUVs.GetNumChannels() < 2)
		{
			OutError = TEXT("Generated card mesh does not contain UV0 and UV1 for atlas trimming.");
			return false;
		}
		auto RemapAtlasUV = [&](const FVector2f& OldUV)
		{
			return FVector2f(
				(static_cast<float>(OldUV.X) * static_cast<float>(OldWidth) - static_cast<float>(CropMinX)) / static_cast<float>(NewWidth),
				(static_cast<float>(OldUV.Y) * static_cast<float>(OldHeight) - static_cast<float>(CropMinY)) / static_cast<float>(NewHeight));
		};
		for (const FVertexInstanceID VertexInstanceID : MeshDescription.VertexInstances().GetElementIDs())
		{
			VertexInstanceUVs.Set(VertexInstanceID, 0, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 0)));
			VertexInstanceUVs.Set(VertexInstanceID, 1, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 1)));
		}

		for (UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			PlaneInfo.AtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PlaneInfo.BackAtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			}
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PlaneInfo.AtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.AtlasUVs[CornerIndex]);
				PlaneInfo.BackAtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.BackAtlasUVs[CornerIndex]);
			}
		}

		AtlasPixels = MoveTemp(CroppedAtlasPixels);
		NormalAtlasPixels = MoveTemp(CroppedNormalAtlasPixels);
		MixAtlasPixels = MoveTemp(CroppedMixAtlasPixels);
		AtlasStats.Width = NewWidth;
		AtlasStats.Height = NewHeight;

		int64 PackedPaddedTilePixels = 0;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const int32 Padding = FMath::Max(0, PlaneInfo.AtlasTilePaddingPixels);
			PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.AtlasTileSize.X + Padding * 2)
				* static_cast<int64>(PlaneInfo.AtlasTileSize.Y + Padding * 2);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.BackAtlasTileSize.X + Padding * 2)
					* static_cast<int64>(PlaneInfo.BackAtlasTileSize.Y + Padding * 2);
			}
		}
		const int64 NewAtlasPixelCount = static_cast<int64>(NewWidth) * static_cast<int64>(NewHeight);
		AtlasStats.PackedTileUtilizationPercent = NewAtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(NewAtlasPixelCount)
			: 0.0;
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
		const FFoliageBakerCardBakeRequest& EditorSettings,
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
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			EditorSettings.TextureNamePrefix,
			EditorSettings.NormalDepthTextureSuffix,
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
		const FFoliageBakerCardBakeRequest& EditorSettings,
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

	const TCHAR* GetMeshOutputModeText(const EFoliageBakerMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
		case EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD:
			return TEXT("replaced source mesh LOD");
		case EFoliageBakerMeshOutputMode::SeparateMeshAsset:
		default:
			return TEXT("separate mesh asset");
		}
	}

	struct FProxyPlaneCoverBuildData
	{
		int32 SourceLODIndex = INDEX_NONE;
		FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		FTrunkLeafClassification TrunkLeafClassification;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
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
		EFoliageBakerMeshOutputMode MeshOutputMode = EFoliageBakerMeshOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh.GetName(), *Error);
		UE_LOG(LogFoliageBakerCardsCore, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const FFoliageBakerCardBakeRequest& Settings)
	{
		FAtlasOutputSelection Selection;
		Selection.bBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Selection.bNormalMask = Settings.bBakeNormalDepth;
		Selection.bMix = Settings.bBakeMix;
		return Selection;
	}

	FVector ResolveSingleCaptureNormal(const EFoliageBakerCaptureAxis Axis)
	{
		switch (Axis)
		{
		case EFoliageBakerCaptureAxis::NegativeX: return FVector(-1.0, 0.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveY: return FVector(0.0, 1.0, 0.0);
		case EFoliageBakerCaptureAxis::NegativeY: return FVector(0.0, -1.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveX:
		default: return FVector(1.0, 0.0, 0.0);
		}
	}

	EFoliageBakerMeshAssetOutputMode ToAssetOutputMode(const EFoliageBakerMeshOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshOutputMode::AddToSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD;
		case EFoliageBakerMeshOutputMode::ReplaceSourceMeshLOD:
			return EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD;
		case EFoliageBakerMeshOutputMode::SeparateMeshAsset:
		default:
			return EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		}
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(const FFoliageBakerCardBakeRequest& Request)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = ToAssetOutputMode(Request.MeshOutputMode);
		Params.RequestedReplaceLODIndex = Request.ReplaceSourceLODIndex;
		Params.SourceLODIndex = Request.SourceLODIndex;
		Params.DesiredUVChannelCount = 2;
		Params.RebuildLODMetadataKey = Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			? FName(TEXT("FoliageBaker.SingleBillboardLOD"))
			: FName(TEXT("FoliageBaker.CrossCardsLOD"));
		return Params;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		OutData.SourceLODIndex = EditorSettings.SourceLODIndex;
		if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			OutData.SourceLODIndex,
			OutData.Triangles,
			OutError))
		{
			return false;
		}

		if (OutData.Triangles.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Source LOD %d contains no bakeable triangles."), OutData.SourceLODIndex);
			return false;
		}
		if (!ComputeSourceTriangleBounds(OutData.Triangles, OutData.SourceLODBounds))
		{
			OutError = FString::Printf(TEXT("Source LOD %d has no valid bounds."), OutData.SourceLODIndex);
			return false;
		}




		OutData.TrunkLeafClassification = ClassifyTrianglesForTrunkLeafMask(
			StaticMesh,
			OutData.Triangles,
			EditorSettings.TrunkMaterialKeywords);

		OutData.Settings = BuildSettingsForMesh(OutData.Triangles, EditorSettings);
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


		const int32 PlaneCount = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			? 1
			: FMath::Clamp(EditorSettings.CrossCardPlaneCount, 2, 5);
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneCount; ++PlaneIndex)
		{
			const FVector Normal = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis)
				: FVector(
					FMath::Cos(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
					FMath::Sin(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
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
			CoverData.ProxyResult,
			CoverData.Settings,
			OutData.MeshDescription,
			OutData.Stats,
			OutError,
			&OutData.PlaneInfos);
	}

	bool BuildProxyTextureData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable ColorOpacity, NormalMask, or Mix.");
			return false;
		}
		UMaterialInstanceConstant* TemplateMaterialInstance = EditorSettings.MaterialTemplate;
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("A Material Instance Constant template is required.");
			return false;
		}

		auto BakeFeatureAtlas = [&](const FAtlasOutputSelection& OutputSelection,
			TArray<FColor>& AtlasPixels,
			TArray<FColor>& NormalPixels,
			TArray<FColor>& MixPixels,
			FAtlasBakeStats& AtlasStats)
		{
			BakeCardAtlasOrthographic(
				StaticMesh,
				CoverData.SourceLODIndex,
				CoverData.SourceLODBounds,
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				OutputSelection,
				AtlasPixels,
				NormalPixels,
				MixPixels,
				AtlasStats);
		};

		int32 AlphaAwareCroppedPlaneCount = 0;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			const uint8 AlphaCropThreshold = TemplateMaterialInstance->GetBlendMode() == BLEND_Masked
				? static_cast<uint8>(FMath::Clamp(FMath::CeilToInt(TemplateMaterialInstance->GetOpacityMaskClipValue() * 255.0f), 1, 255))
				: 1;
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			FAtlasBakeStats CropStats;
			BakeFeatureAtlas(
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
				AlphaCropThreshold,
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

		BakeFeatureAtlas(
			OutData.OutputSelection,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.AtlasStats);
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? CoverData.Settings.AlphaAwareTileCropGuardPixels
			: 0;
		if (EditorSettings.bTrimUnusedAtlasSpace)
		{
			if (!TrimUnusedAtlasSpace(
				MeshData.PlaneInfos,
				MeshData.MeshDescription,
				OutData.AtlasPixels,
				OutData.NormalAtlasPixels,
				OutData.MixAtlasPixels,
				OutData.AtlasStats,
				OutError))
			{
				return false;
			}
			MeshData.Stats.AtlasWidth = OutData.AtlasStats.Width;
			MeshData.Stats.AtlasHeight = OutData.AtlasStats.Height;
		}

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
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = EditorSettings.MeshOutputMode;

		if (EditorSettings.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? TEXT("_Billboard")
				: TEXT("_CrossCards");
			MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
			MeshParams.DesiredUVChannelCount = 2;
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
			const FFoliageBakerSourceLODAssetParams LODParams = BuildSourceLODAssetParams(EditorSettings);
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				LODParams,
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
		const FFoliageBakerCardBakeRequest& Request,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);

		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm\n  feature: %s, capture=%s, selected-LOD projected bounds, per-angle alpha crop\n  trunk/leaf classification: shared material/parent keyword rule, matched materials=%d, trunk triangles=%d"),
			*StaticMesh.GetName(),
			CoverData.SourceLODIndex,
			CoverData.SourceLODBounds.SphereRadius,
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard ? TEXT("Single Billboard") : TEXT("Cross Cards"),
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard ? TEXT("one selected axis, one baked side") : TEXT("equally spaced over 180 degrees, front and back baked"),
			CoverData.TrunkLeafClassification.MatchedMaterialCount,
			CoverData.TrunkLeafClassification.TrunkTriangleCount);
		const FString BaseAtlasPath = TextureData.AtlasTexture ? TextureData.AtlasTexture->GetPathName() : TEXT("disabled");
		const FString NormalAtlasPath = TextureData.NormalAtlasTexture ? TextureData.NormalAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MixAtlasPath = TextureData.MixAtlasTexture ? TextureData.MixAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/Opacity=%s, Normal/Depth=%s, Mix=%s"),
			*Request.BaseColorOpacityTextureParameterName.ToString(),
			*Request.NormalDepthTextureParameterName.ToString(),
			*Request.MixTextureParameterName.ToString());

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, readable material textures=%d, mix-texture materials=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, alpha refs=%d, masked refs=%d, mix refs texture=%d, forced opaque=%d, shooting=%s, resolve=%s\n  base/color opacity atlas: %s\n  normal/depth atlas: %s, RGB=object/local-space normal, A=one linear Min/Max range shared by all capture views (global nearest selected-LOD geometry point 0, global farthest 1, uncovered 1)\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission\n  trunk/leaf mask: ColorOpacity.A, transparent=0, trunk=0.5 (128), leaf=1 (255); source opacity is evaluated first for coverage; Impostor uses the same contract when implemented\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; Single Billboard uses one baked side\n  material instance: %s (copied from the supplied MIC template; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals"),
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
			TextureData.AtlasStats.TextureAlphaOpacityReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			TextureData.AtlasStats.SourceMixTextureReferences,
			TextureData.AtlasStats.ForcedOpaqueAlphaReferences,
			Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? TEXT("dedicated fixed-axis orthographic capture, all selected-LOD triangles, WPO disabled")
				: TEXT("dedicated fixed-angle orthographic capture, front and back per plane, all selected-LOD triangles, WPO disabled"),
			TEXT("opacity rejection before exact per-pixel nearest-depth selection; winning source UV samples BaseColor, Normal, and Mix"),
			*BaseAtlasPath,
			*NormalAtlasPath,
			*MixAtlasPath,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails);
	}

	FProxyAssetBuildResult BuildCardProxyAsset(
		UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings)
	{
		FString Error;
		if (EditorSettings.MeshOutputMode != EFoliageBakerMeshOutputMode::SeparateMeshAsset
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
		UE_LOG(LogFoliageBakerCardsCore, Display, TEXT("\n%s"), *Result.Report);
		return Result;
	}

	void AppendCardCreatedAssets(const FProxyAssetBuildResult& BuildResult, TArray<UObject*>& OutCreatedAssets)
	{
		if (BuildResult.MeshOutputMode == EFoliageBakerMeshOutputMode::SeparateMeshAsset && BuildResult.ProxyMesh)
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


FFoliageBakerCardBakeResult FFoliageBakerCardBaker::Bake(const FFoliageBakerCardBakeRequest& Request)
{
	FFoliageBakerCardBakeResult OutResult;
	if (!Request.SourceStaticMesh)
	{
		OutResult.Report = TEXT("Foliage Baker failed: source Static Mesh is null.");
		return OutResult;
	}
	if (Request.SourceLODIndex < 0 || Request.SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: source LOD index %d is outside the supported range 0-%d."),
			*Request.SourceStaticMesh->GetName(),
			Request.SourceLODIndex,
			MAX_STATIC_MESH_LODS - 1);
		return OutResult;
	}
	if (!Request.MaterialTemplate)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: a Material Instance Constant template is required."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	if (!Request.bBakeBaseColorOpacity && !Request.bBakeNormalDepth && !Request.bBakeMix)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: no texture output is enabled."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	TSet<FName> UsedTextureParameterNames;
	FString TextureParameterError;
	auto ValidateTextureParameterName = [&](const bool bEnabled, const FName ParameterName, const TCHAR* OutputLabel) -> bool
	{
		if (!bEnabled)
		{
			return true;
		}
		if (ParameterName.IsNone())
		{
			TextureParameterError = FString::Printf(
				TEXT("%s output is enabled, but its Material texture parameter name is None."),
				OutputLabel);
			return false;
		}
		if (UsedTextureParameterNames.Contains(ParameterName))
		{
			TextureParameterError = FString::Printf(
				TEXT("Material texture parameter '%s' is assigned to more than one enabled output."),
				*ParameterName.ToString());
			return false;
		}
		UsedTextureParameterNames.Add(ParameterName);
		return true;
	};
	if (!ValidateTextureParameterName(Request.bBakeBaseColorOpacity, Request.BaseColorOpacityTextureParameterName, TEXT("BaseColor/Opacity"))
		|| !ValidateTextureParameterName(Request.bBakeNormalDepth, Request.NormalDepthTextureParameterName, TEXT("Normal/Depth"))
		|| !ValidateTextureParameterName(Request.bBakeMix, Request.MixTextureParameterName, TEXT("Mix")))
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*Request.SourceStaticMesh->GetName(),
			*TextureParameterError);
		return OutResult;
	}
	if (Request.TextureResolution < 256 || Request.TextureResolution > 4096)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: texture resolution must be between 256 and 4096."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}

	FFoliageBakerCardBakeRequest SanitizedRequest = Request;
	SanitizedRequest.SourceLODIndex = Request.SourceLODIndex;
	SanitizedRequest.CrossCardPlaneCount = FMath::Clamp(Request.CrossCardPlaneCount, 2, 5);
	SanitizedRequest.SourceMaterialBakeResolution = FMath::Clamp(Request.SourceMaterialBakeResolution, 256, 4096);
	SanitizedRequest.AlphaCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 0, 16);
	const FString FeatureSuffix = Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
		? TEXT("_Billboard")
		: TEXT("_CrossCards");
	SanitizedRequest.BaseColorOpacityTextureSuffix = FeatureSuffix + Request.BaseColorOpacityTextureSuffix;
	SanitizedRequest.NormalDepthTextureSuffix = FeatureSuffix + Request.NormalDepthTextureSuffix;
	SanitizedRequest.MixTextureSuffix = FeatureSuffix + Request.MixTextureSuffix;
	SanitizedRequest.MaterialInstanceNameSuffix = FeatureSuffix + Request.MaterialInstanceNameSuffix;

	const FProxyAssetBuildResult InternalResult = BuildCardProxyAsset(*Request.SourceStaticMesh, SanitizedRequest);
	OutResult.bSucceeded = InternalResult.bSucceeded;
	OutResult.ProxyMesh = InternalResult.ProxyMesh;
	OutResult.SourceMeshLODIndex = InternalResult.SourceMeshLODIndex;
	OutResult.ColorOpacityTexture = InternalResult.AtlasTexture;
	OutResult.NormalDepthTexture = InternalResult.NormalAtlasTexture;
	OutResult.MixTexture = InternalResult.MixAtlasTexture;
	OutResult.MaterialInstance = InternalResult.Material;
	OutResult.Report = InternalResult.Report;
	AppendCardCreatedAssets(InternalResult, OutResult.CreatedAssets);
	return OutResult;
}
