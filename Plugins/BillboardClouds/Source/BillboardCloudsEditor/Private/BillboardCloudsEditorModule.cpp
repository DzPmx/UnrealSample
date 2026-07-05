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
#include "Materials/MaterialParameters.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FBillboardCloudsEditorModule"

DEFINE_LOG_CATEGORY_STATIC(LogBillboardCloudsEditor, Log, All);

namespace
{
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
		Settings.NormalThetaSteps = FMath::Max(4, EditorSettings.NormalThetaSteps);
		Settings.NormalPhiSteps = FMath::Max(3, EditorSettings.NormalPhiSteps);
		Settings.RhoBinCount = FMath::Max(8, EditorSettings.RhoBinCount);
		Settings.AdaptiveRefinementDepth = FMath::Clamp(EditorSettings.AdaptiveRefinementDepth, 0, 16);
		Settings.TextureTileResolution = FMath::Clamp(EditorSettings.TextureTileResolution, 16, 1024);
		Settings.TextureTilePaddingPixels = FMath::Clamp(EditorSettings.TextureTilePaddingPixels, 0, 128);
		Settings.TextureAtlasMaxResolution = FMath::Clamp(EditorSettings.TextureAtlasMaxResolution, 256, 8192);
		Settings.ErrorTolerance = FMath::Max(
			FMath::Max(0.0, EditorSettings.MinimumErrorCm),
			StaticMesh.GetBounds().SphereRadius * FMath::Max(0.0, EditorSettings.RelativeError));
		return Settings;
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

		FLinearColor Sample(const FVector2f& UV) const
		{
			if (!IsValid())
			{
				return FLinearColor::White;
			}

			const double U = static_cast<double>(UV.X) - FMath::FloorToDouble(static_cast<double>(UV.X));
			const double V = static_cast<double>(UV.Y) - FMath::FloorToDouble(static_cast<double>(UV.Y));
			const int32 X = FMath::Clamp(FMath::FloorToInt(U * static_cast<double>(Width)), 0, Width - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt(V * static_cast<double>(Height)), 0, Height - 1);
			const int64 PixelIndex = static_cast<int64>(Y) * Width + X;

			if (Format == TSF_G8)
			{
				const uint8 Gray = Bytes[PixelIndex];
				return FLinearColor(FColor(Gray, Gray, Gray, 255));
			}

			const int64 ByteIndex = PixelIndex * 4;
			const uint8 B = Bytes[ByteIndex + 0];
			const uint8 G = Bytes[ByteIndex + 1];
			const uint8 R = Bytes[ByteIndex + 2];
			const uint8 A = Bytes[ByteIndex + 3];
			return FLinearColor(FColor(R, G, B, A));
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
		bool bHasReadableBaseColorTexture = false;
		bool bHasReadableOpacityMaskTexture = false;
		bool bUseTextureAlphaAsOpacity = false;
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
		int32 SourceTexturedTriangles = 0;
		int32 FallbackTriangles = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 ReadableMaterialTextures = 0;
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
			const EBlendMode BlendMode = MaterialInterface ? MaterialInterface->GetBlendMode() : BLEND_Opaque;
			const bool bMaterialCanUseOpacity = BlendMode != BLEND_Opaque;
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
			InOutStats.MaterialAlphaPolicyDetails += FString::Printf(
				TEXT("\n    material=%d, slot=%s, asset=%s, blend=%s, alpha=%s"),
				MaterialIndex,
				*SlotName,
				*MaterialName,
				GetBlendModeName(BlendMode),
				*BakeData.OpacityMaskSource);
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
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
		{
			return;
		}

		TBitArray<> HasColor;
		HasColor.Init(false, Pixels.Num());

		TArray<int32> TileDilationIterations;
		TileDilationIterations.Reserve(PlaneInfos.Num());

		int32 MaxDilationIterations = 0;
		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
			const int32 MinX = FMath::Clamp(PlaneInfo.AtlasPixelMin.X, 0, Width - 1);
			const int32 MinY = FMath::Clamp(PlaneInfo.AtlasPixelMin.Y, 0, Height - 1);
			const int32 MaxX = FMath::Clamp(PlaneInfo.AtlasPixelMin.X + TileSize.X - 1, 0, Width - 1);
			const int32 MaxY = FMath::Clamp(PlaneInfo.AtlasPixelMin.Y + TileSize.Y - 1, 0, Height - 1);
			if (TileSize.X <= 0 || TileSize.Y <= 0 || MinX > MaxX || MinY > MaxY)
			{
				TileDilationIterations.Add(0);
				continue;
			}

			const int32 DilationIterations = FMath::Clamp(FMath::Max(4, PlaneInfo.AtlasTilePaddingPixels), 1, 32);
			TileDilationIterations.Add(DilationIterations);
			MaxDilationIterations = FMath::Max(MaxDilationIterations, DilationIterations);

			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const int32 PixelIndex = Y * Width + X;
					if (Pixels[PixelIndex].A > 0)
					{
						HasColor[PixelIndex] = true;
					}
				}
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

			for (int32 TileIndex = 0; TileIndex < PlaneInfos.Num(); ++TileIndex)
			{
				if (!TileDilationIterations.IsValidIndex(TileIndex) || Iteration >= TileDilationIterations[TileIndex])
				{
					continue;
				}

				const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[TileIndex];
				const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
				const int32 MinX = FMath::Clamp(PlaneInfo.AtlasPixelMin.X, 0, Width - 1);
				const int32 MinY = FMath::Clamp(PlaneInfo.AtlasPixelMin.Y, 0, Height - 1);
				const int32 MaxX = FMath::Clamp(PlaneInfo.AtlasPixelMin.X + TileSize.X - 1, 0, Width - 1);
				const int32 MaxY = FMath::Clamp(PlaneInfo.AtlasPixelMin.Y + TileSize.Y - 1, 0, Height - 1);
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
							++SourceCount;
						}

						if (SourceCount > 0)
						{
							FColor& DilatedColor = Scratch[PixelIndex];
							DilatedColor.R = static_cast<uint8>(FMath::Clamp((AccumR + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.G = static_cast<uint8>(FMath::Clamp((AccumG + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.B = static_cast<uint8>(FMath::Clamp((AccumB + SourceCount / 2) / SourceCount, 0, 255));
							DilatedColor.A = 0;
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
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos)
	{
		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const int32 Padding = PlaneInfo.AtlasTilePaddingPixels;
			const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
			if (Padding <= 0 || TileSize.X <= 0 || TileSize.Y <= 0)
			{
				continue;
			}

			const int32 InteriorMinX = PlaneInfo.AtlasPixelMin.X;
			const int32 InteriorMinY = PlaneInfo.AtlasPixelMin.Y;
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
					PaddingColor.A = 0;
					Pixels[Y * Width + X] = PaddingColor;
				}
			}
		}
	}

	bool IsTriangleValidForTexturePlane(
		const UE::BillboardClouds::FSourceTriangle& Triangle,
		const FVector& PlaneNormal,
		const double PlaneRho,
		const double ErrorTolerance)
	{
		const double Distance0 = FVector::DotProduct(PlaneNormal, Triangle.Vertices[0]) - PlaneRho;
		const double Distance1 = FVector::DotProduct(PlaneNormal, Triangle.Vertices[1]) - PlaneRho;
		const double Distance2 = FVector::DotProduct(PlaneNormal, Triangle.Vertices[2]) - PlaneRho;
		return FMath::Abs(Distance0) <= ErrorTolerance
			&& FMath::Abs(Distance1) <= ErrorTolerance
			&& FMath::Abs(Distance2) <= ErrorTolerance;
	}

	void RasterizeBillboardAtlas(
		const UStaticMesh& SourceStaticMesh,
		const TArray<UE::BillboardClouds::FSourceTriangle>& Triangles,
		const TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::BillboardClouds::FPlaneProxyMeshStats& ProxyStats,
		const UE::BillboardClouds::FPlaneCoverSettings& Settings,
		TArray<FColor>& OutPixels,
		FAtlasBakeStats& OutStats)
	{
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;
		OutStats.TilePaddingPixels = ProxyStats.AtlasTilePaddingPixels;
		OutPixels.Init(FColor(0, 0, 0, 0), OutStats.Width * OutStats.Height);

		TArray<double> DepthBuffer;
		DepthBuffer.Init(TNumericLimits<double>::Max(), OutPixels.Num());

		const TArray<FMaterialBakeData> MaterialBakeData = BuildMaterialBakeData(SourceStaticMesh, Triangles, OutStats);

		for (const UE::BillboardClouds::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
			if (TileSize.X <= 0 || TileSize.Y <= 0 || FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU) || FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				continue;
			}

			auto RasterizeTriangleToPlane = [&](const int32 TriangleIndex)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					return;
				}

				const UE::BillboardClouds::FSourceTriangle& Triangle = Triangles[TriangleIndex];
				const int32 MaterialIndex = MaterialBakeData.IsValidIndex(Triangle.MaterialIndex) ? Triangle.MaterialIndex : 0;
				const FMaterialBakeData& BakeData = MaterialBakeData[MaterialIndex];
				const bool bUseTexture = Triangle.bHasUVs && BakeData.bHasReadableBaseColorTexture;
				const bool bUseTextureAlphaAsOpacity = Triangle.bHasUVs && BakeData.bUseTextureAlphaAsOpacity && BakeData.bHasReadableOpacityMaskTexture;
				OutStats.SourceTexturedTriangles += bUseTexture ? 1 : 0;
				OutStats.FallbackTriangles += bUseTexture ? 0 : 1;
				OutStats.TextureAlphaOpacityReferences += bUseTextureAlphaAsOpacity ? 1 : 0;
				OutStats.ForcedOpaqueAlphaReferences += bUseTextureAlphaAsOpacity ? 0 : 1;
				++OutStats.RasterizedTriangleReferences;

				FVector2D ProjectedPoints[3];
				for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
				{
					const FVector& Vertex = Triangle.Vertices[VertexIndex];
					const double SignedDistance = FVector::DotProduct(PlaneInfo.Normal, Vertex) - PlaneInfo.Rho;
					const FVector ProjectedVertex = Vertex - PlaneInfo.Normal * SignedDistance;
					const double UFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisU) - PlaneInfo.MinU) / (PlaneInfo.MaxU - PlaneInfo.MinU);
					const double VFraction = (FVector::DotProduct(ProjectedVertex, PlaneInfo.AxisV) - PlaneInfo.MinV) / (PlaneInfo.MaxV - PlaneInfo.MinV);
					ProjectedPoints[VertexIndex] = FVector2D(
						PlaneInfo.AtlasPixelMin.X + UFraction * TileSize.X,
						PlaneInfo.AtlasPixelMin.Y + VFraction * TileSize.Y);
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), PlaneInfo.AtlasPixelMin.X, PlaneInfo.AtlasPixelMin.X + TileSize.X - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].X, ProjectedPoints[1].X, ProjectedPoints[2].X)), PlaneInfo.AtlasPixelMin.X, PlaneInfo.AtlasPixelMin.X + TileSize.X - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), PlaneInfo.AtlasPixelMin.Y, PlaneInfo.AtlasPixelMin.Y + TileSize.Y - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(ProjectedPoints[0].Y, ProjectedPoints[1].Y, ProjectedPoints[2].Y)), PlaneInfo.AtlasPixelMin.Y, PlaneInfo.AtlasPixelMin.Y + TileSize.Y - 1);
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
						const double PlaneDistance = FMath::Abs(FVector::DotProduct(PlaneInfo.Normal, SourcePoint) - PlaneInfo.Rho);
						if (PlaneDistance > Settings.ErrorTolerance)
						{
							continue;
						}

						const int32 PixelIndex = Y * OutStats.Width + X;
						if (!DepthBuffer.IsValidIndex(PixelIndex) || PlaneDistance > DepthBuffer[PixelIndex])
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
						DepthBuffer[PixelIndex] = PlaneDistance;
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

			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				if (!QueuedTriangles[TriangleIndex]
					&& IsTriangleValidForTexturePlane(Triangles[TriangleIndex], PlaneInfo.Normal, PlaneInfo.Rho, Settings.ErrorTolerance))
				{
					QueuedTriangles[TriangleIndex] = true;
					TextureTriangleIndices.Add(TriangleIndex);
				}
			}

			for (const int32 TriangleIndex : TextureTriangleIndices)
			{
				RasterizeTriangleToPlane(TriangleIndex);
			}
		}

		DilateTransparentRgbInsideAtlasTiles(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);
		CopyAtlasTileBorderIntoPadding(OutPixels, OutStats.Width, OutStats.Height, PlaneInfos);

		for (const FColor& Pixel : OutPixels)
		{
			OutStats.PaintedPixels += Pixel.A > 0 ? 1 : 0;
		}
	}

	UTexture2D* CreateAtlasTextureAsset(const UStaticMesh& SourceStaticMesh, const TArray<FColor>& Pixels, const FAtlasBakeStats& AtlasStats, FString& OutError)
	{
		OutError.Reset();
		if (Pixels.IsEmpty() || AtlasStats.Width <= 0 || AtlasStats.Height <= 0)
		{
			OutError = TEXT("No atlas pixels were generated.");
			return nullptr;
		}

		const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
		const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
		const FString BaseAssetName = SourceStaticMesh.GetName() + TEXT("_BillboardCloudAtlas");
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
		Texture->CompressionSettings = TC_Default;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->SRGB = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(Texture);
		return Texture;
	}

	UMaterial* CreateBillboardMaterialAsset(const UStaticMesh& SourceStaticMesh, UTexture2D* AtlasTexture, FString& OutError)
	{
		OutError.Reset();
		if (!AtlasTexture)
		{
			OutError = TEXT("Atlas texture is null.");
			return nullptr;
		}

		const FString SourcePackageName = SourceStaticMesh.GetOutermost()->GetName();
		const FString PackagePath = FPackageName::GetLongPackagePath(SourcePackageName);
		const FString BaseAssetName = SourceStaticMesh.GetName() + TEXT("_BillboardCloudMaterial");
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
		UMaterial* Material = NewObject<UMaterial>(Package, *UniqueAssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("Could not create Material %s."), *UniqueAssetName);
			return nullptr;
		}

		UMaterialExpressionTextureSample* TextureSample = NewObject<UMaterialExpressionTextureSample>(Material);
		TextureSample->Texture = AtlasTexture;
		TextureSample->SamplerType = SAMPLERTYPE_Color;
		TextureSample->MaterialExpressionEditorX = -300;
		TextureSample->MaterialExpressionEditorY = 0;
		Material->GetExpressionCollection().AddExpression(TextureSample);

		UMaterialEditorOnlyData* MaterialEditorOnly = Material->GetEditorOnlyData();
		MaterialEditorOnly->BaseColor.Connect(0, TextureSample);
		MaterialEditorOnly->OpacityMask.Connect(4, TextureSample);
		Material->BlendMode = BLEND_Masked;
		Material->OpacityMaskClipValue = 0.33333334f;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TwoSided = true;
		Material->PostEditChange();
		Material->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(Material);
		return Material;
	}

	void KeepOnlyUv0(UStaticMesh& StaticMesh)
	{
		StaticMesh.SetLightMapCoordinateIndex(0);

		if (StaticMesh.GetNumSourceModels() > 0)
		{
			FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(0);
			SourceModel.BuildSettings.bGenerateLightmapUVs = false;
			SourceModel.BuildSettings.SrcLightmapIndex = 0;
			SourceModel.BuildSettings.DstLightmapIndex = 0;
			SourceModel.BuildSettings.bUseFullPrecisionUVs = false;
		}

		if (FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription(0))
		{
			TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = FStaticMeshAttributes(*MeshDescription).GetVertexInstanceUVs();
			if (VertexInstanceUVs.GetNumChannels() != 1)
			{
				VertexInstanceUVs.SetNumChannels(1);
				StaticMesh.CommitMeshDescription(0);
			}
		}

		for (int32 Guard = 0; Guard < 8 && StaticMesh.GetNumUVChannels(0) > 1; ++Guard)
		{
			if (!StaticMesh.RemoveUVChannel(0, 1))
			{
				break;
			}
		}

		StaticMesh.SetLightMapCoordinateIndex(0);
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

		const FName MaterialSlotName(TEXT("BillboardProxy"));
		ProxyMesh->GetStaticMaterials().Add(FStaticMaterial(ProxyMaterial ? ProxyMaterial : UMaterial::GetDefaultMaterial(MD_Surface), MaterialSlotName, MaterialSlotName));
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

		if (ProxyMesh->GetNumSourceModels() > 0)
		{
			FStaticMeshSourceModel& SourceModel = ProxyMesh->GetSourceModel(0);
			SourceModel.BuildSettings.bRecomputeNormals = false;
			SourceModel.BuildSettings.bRecomputeTangents = false;
			SourceModel.BuildSettings.bRemoveDegenerates = false;
			SourceModel.BuildSettings.bGenerateLightmapUVs = false;
			SourceModel.BuildSettings.SrcLightmapIndex = 0;
			SourceModel.BuildSettings.DstLightmapIndex = 0;
			SourceModel.BuildSettings.bUseFullPrecisionUVs = false;
			SourceModel.BuildSettings.DistanceFieldResolutionScale = 0.0f;
		}

		KeepOnlyUv0(*ProxyMesh);
		ProxyMesh->PostEditChange();
		ProxyMesh->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(ProxyMesh);
		return ProxyMesh;
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
		"BillboardCloudsAnalyzeSelectedStaticMeshes",
		LOCTEXT("AnalyzeSelectedStaticMeshesLabel", "Analyze Selected Static Meshes"),
		LOCTEXT("AnalyzeSelectedStaticMeshesTooltip", "Run the Billboard Clouds paper-style plane-space cover on selected Static Mesh assets."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FBillboardCloudsEditorModule::ExecuteAnalyzeSelectedStaticMeshes)
	);
	Section.AddMenuEntry(
		"BillboardCloudsCreatePlaneProxyMeshes",
		LOCTEXT("CreatePlaneProxyMeshesLabel", "Create Plane Proxy Meshes"),
		LOCTEXT("CreatePlaneProxyMeshesTooltip", "Create Static Mesh assets from the selected Billboard Clouds paper-style plane cover."),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateRaw(this, &FBillboardCloudsEditorModule::ExecuteCreatePlaneProxyMeshes)
	);
}

void FBillboardCloudsEditorModule::ExecuteAnalyzeSelectedStaticMeshes(const FToolMenuContext& MenuContext) const
{
	const TArray<UStaticMesh*> StaticMeshes = GetSelectedStaticMeshes();

	if (StaticMeshes.IsEmpty())
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("NoStaticMeshSelection", "Select one or more Static Mesh assets in the Content Browser, then run Tools > Billboard Clouds > Analyze Selected Static Meshes.")
		);
		return;
	}

	const UBillboardCloudsEditorSettings* EditorSettings = GetDefault<UBillboardCloudsEditorSettings>();

	FString Report;
	for (UStaticMesh* StaticMesh : StaticMeshes)
	{
		TArray<UE::BillboardClouds::FSourceTriangle> Triangles;
		FString Error;
		if (!UE::BillboardClouds::ExtractTrianglesFromStaticMesh(StaticMesh, 0, Triangles, Error))
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		const UE::BillboardClouds::FPlaneCoverSettings Settings = BuildSettingsForMesh(*StaticMesh, *EditorSettings);
		const UE::BillboardClouds::FPlaneCoverResult Result = UE::BillboardClouds::BuildGreedyPlaneCover(Triangles, Settings);
		const FString Summary = UE::BillboardClouds::SummarizePlaneCover(StaticMesh->GetName(), Settings, Result);
		UE_LOG(LogBillboardCloudsEditor, Display, TEXT("\n%s"), *Summary);
		Report += Summary + TEXT("\n\n");
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
}

void FBillboardCloudsEditorModule::ExecuteCreatePlaneProxyMeshes(const FToolMenuContext& MenuContext) const
{
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

		TArray<UE::BillboardClouds::FSourceTriangle> Triangles;
		FString Error;
		if (!UE::BillboardClouds::ExtractTrianglesFromStaticMesh(StaticMesh, 0, Triangles, Error))
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		const UE::BillboardClouds::FPlaneCoverSettings Settings = BuildSettingsForMesh(*StaticMesh, *EditorSettings);
		const UE::BillboardClouds::FPlaneCoverResult Result = UE::BillboardClouds::BuildGreedyPlaneCover(Triangles, Settings);

		FMeshDescription ProxyMeshDescription;
		UE::BillboardClouds::FPlaneProxyMeshStats ProxyStats;
		TArray<UE::BillboardClouds::FPlaneProxyPlaneInfo> PlaneInfos;
		if (!UE::BillboardClouds::BuildPlaneProxyMeshDescription(Triangles, Result, Settings, ProxyMeshDescription, ProxyStats, Error, &PlaneInfos))
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		UTexture2D* AtlasTexture = nullptr;
		UMaterial* BillboardMaterial = nullptr;
		FAtlasBakeStats AtlasStats;
		TArray<FColor> AtlasPixels;
		RasterizeBillboardAtlas(*StaticMesh, Triangles, PlaneInfos, ProxyStats, Settings, AtlasPixels, AtlasStats);
		AtlasTexture = CreateAtlasTextureAsset(*StaticMesh, AtlasPixels, AtlasStats, Error);
		if (!AtlasTexture)
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		BillboardMaterial = CreateBillboardMaterialAsset(*StaticMesh, AtlasTexture, Error);
		if (!BillboardMaterial)
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		UStaticMesh* ProxyMesh = CreateStaticMeshAssetFromDescription(*StaticMesh, ProxyMeshDescription, BillboardMaterial, Error);
		if (!ProxyMesh)
		{
			const FString Failure = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh->GetName(), *Error);
			UE_LOG(LogBillboardCloudsEditor, Warning, TEXT("%s"), *Failure);
			Report += Failure + TEXT("\n\n");
			continue;
		}

		CreatedAssets.Add(ProxyMesh);
		if (AtlasTexture)
		{
			CreatedAssets.Add(AtlasTexture);
		}
		if (BillboardMaterial)
		{
			CreatedAssets.Add(BillboardMaterial);
		}

		const FString Summary = UE::BillboardClouds::SummarizePlaneCover(StaticMesh->GetName(), Settings, Result);
		const FString AlphaPolicyDetails = AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *AtlasStats.MaterialAlphaPolicyDetails);
		const FString Success = FString::Printf(
			TEXT("%s%s\n  proxy asset: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas: %s, %dx%d, max tile=%d, max padding=%d, painted pixels=%d, readable material textures=%d, alpha-mask materials=%d, source-textured refs=%d, fallback refs=%d, rasterized refs=%d, alpha refs texture=%d, forced opaque=%d, shooting=valid-zone clipped\n  material: %s (masked atlas, clip=0.5)\n  normal source triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: BuildFromMeshDescriptions fast path, recompute normals/tangents off, distance fields off\n  proxy winding: reversed UE front-face order, source-facing normals"),
			*Summary,
			*AlphaPolicyDetails,
			*ProxyMesh->GetPathName(),
			ProxyStats.PlaneCount,
			ProxyStats.QuadCount,
			ProxyStats.TriangleCount,
			*AtlasTexture->GetPathName(),
			AtlasStats.Width,
			AtlasStats.Height,
			AtlasStats.TileResolution,
			AtlasStats.TilePaddingPixels,
			AtlasStats.PaintedPixels,
			AtlasStats.ReadableMaterialTextures,
			AtlasStats.TextureAlphaOpacityMaterials,
			AtlasStats.SourceTexturedTriangles,
			AtlasStats.FallbackTriangles,
			AtlasStats.RasterizedTriangleReferences,
			AtlasStats.TextureAlphaOpacityReferences,
			AtlasStats.ForcedOpaqueAlphaReferences,
			*BillboardMaterial->GetPathName(),
			ProxyStats.SourceShadingNormalTriangleCount,
			ProxyStats.SourceTriangleCount,
			ProxyStats.AveragePlaneToShadingNormalDot,
			ProxyStats.AveragePlaneToShadingNormalAngleDegrees);
		UE_LOG(LogBillboardCloudsEditor, Display, TEXT("\n%s"), *Success);
		Report += Success + TEXT("\n\n");
	}

	if (!CreatedAssets.IsEmpty() && GEditor)
	{
		GEditor->SyncBrowserToObjects(CreatedAssets);
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBillboardCloudsEditorModule, BillboardCloudsEditor)


