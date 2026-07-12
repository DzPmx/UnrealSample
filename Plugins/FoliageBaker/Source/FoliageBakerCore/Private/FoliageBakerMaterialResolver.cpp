#include "FoliageBakerMaterialResolver.h"

#include "FoliageBakerMaterialBaker.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "MaterialBakingStructures.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace UE::FoliageBaker::MaterialResolver
{
	namespace
	{
		const TCHAR* GetOpacityMaskChannelName(const EOpacityMaskChannel Channel)
		{
			switch (Channel)
			{
			case EOpacityMaskChannel::Red:
				return TEXT("R");
			case EOpacityMaskChannel::Green:
				return TEXT("G");
			case EOpacityMaskChannel::Blue:
				return TEXT("B");
			case EOpacityMaskChannel::Alpha:
				return TEXT("A");
			default:
				return TEXT("?");
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

		bool DoesMaterialOrParentNameMatchKeywords(
			const UMaterialInterface* MaterialInterface,
			const TArray<FString>& Keywords)
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

		bool TextureNameLooksLikeBaseColor(const FString& Name)
		{
			const FString LowerName = Name.ToLower();
			if (LowerName.Contains(TEXT("normal"))
				|| LowerName.Contains(TEXT("_n"))
				|| LowerName.Contains(TEXT("rough"))
				|| LowerName.Contains(TEXT("metal"))
				|| LowerName.Contains(TEXT("ao"))
				|| LowerName.Contains(TEXT("orm"))
				|| LowerName.Contains(TEXT("mask")))
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
			if (LowerName.Contains(TEXT("normal"))
				|| LowerName.Contains(TEXT("_n"))
				|| LowerName.Contains(TEXT("rough"))
				|| LowerName.Contains(TEXT("metal"))
				|| LowerName.Contains(TEXT("orm"))
				|| LowerName.Contains(TEXT("ao")))
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

		EOpacityMaskChannel GetMaskChannelFromInput(const FExpressionInput& Input)
		{
			if (Input.MaskA != 0 || Input.OutputIndex == 4)
			{
				return EOpacityMaskChannel::Alpha;
			}
			if (Input.MaskG != 0 || Input.OutputIndex == 2)
			{
				return EOpacityMaskChannel::Green;
			}
			if (Input.MaskB != 0 || Input.OutputIndex == 3)
			{
				return EOpacityMaskChannel::Blue;
			}

			return EOpacityMaskChannel::Red;
		}

		bool DoesInputUseExplicitChannel(const FExpressionInput& Input)
		{
			return Input.MaskR != 0
				|| Input.MaskG != 0
				|| Input.MaskB != 0
				|| Input.MaskA != 0
				|| Input.OutputIndex > 0;
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
				if (Texture2D
					&& Texture2D->CompressionSettings != TC_Normalmap
					&& TextureNameLooksLikeBaseColor(Texture2D->GetName()))
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
			if (MaterialInterface->GetTextureParameterValue(
				FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)),
				Texture))
			{
				if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
				{
					OutSource = FString::Printf(
						TEXT("%s parameter %s -> %s"),
						PropertyLabel,
						*ParameterName.ToString(),
						*Texture2D->GetName());
					return Texture2D;
				}
			}

			return nullptr;
		}

		UTexture2D* FindMaterialPropertyTexture(
			UMaterialInterface* MaterialInterface,
			const EMaterialProperty Property,
			const TCHAR* PropertyLabel,
			EOpacityMaskChannel& OutChannel,
			bool& bOutUseLuminance,
			FString& OutSource)
		{
			OutChannel = EOpacityMaskChannel::Red;
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

						if (const UMaterialExpressionTextureSampleParameter* TextureParameter =
							Cast<UMaterialExpressionTextureSampleParameter>(TracedInput.Expression))
						{
							if (UTexture2D* Texture2D = FindMaterialPropertyTextureFromParameter(
								MaterialInterface,
								TextureParameter->ParameterName,
								PropertyLabel,
								OutSource))
							{
								OutSource += bOutUseLuminance
									? TEXT(" luminance")
									: FString::Printf(TEXT(" channel %s"), GetOpacityMaskChannelName(OutChannel));
								return Texture2D;
							}
						}

						if (const UMaterialExpressionTextureSample* TextureSample =
							Cast<UMaterialExpressionTextureSample>(TracedInput.Expression))
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
			if (MaterialInterface->GetTextureParameterValue(
				FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)),
				Texture))
			{
				if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
				{
					OutSource = FString::Printf(
						TEXT("opacity parameter %s -> %s"),
						*ParameterName.ToString(),
						*Texture2D->GetName());
					return Texture2D;
				}
			}

			return nullptr;
		}

		UTexture2D* FindOpacityMaskTexture(
			UMaterialInterface* MaterialInterface,
			EOpacityMaskChannel& OutChannel,
			FString& OutSource)
		{
			OutChannel = EOpacityMaskChannel::Alpha;
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

						if (const UMaterialExpressionTextureSampleParameter* TextureParameter =
							Cast<UMaterialExpressionTextureSampleParameter>(TracedInput.Expression))
						{
							if (UTexture2D* Texture2D = FindOpacityMaskTextureFromParameter(
								MaterialInterface,
								TextureParameter->ParameterName,
								OutSource))
							{
								OutSource += FString::Printf(TEXT(" channel %s"), GetOpacityMaskChannelName(OutChannel));
								return Texture2D;
							}
						}

						if (const UMaterialExpressionTextureSample* TextureSample =
							Cast<UMaterialExpressionTextureSample>(TracedInput.Expression))
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

				if (UTexture2D* Texture2D = FindOpacityMaskTextureFromParameter(
					MaterialInterface,
					ParameterInfo.Name,
					OutSource))
				{
					OutChannel = Texture2D->Source.GetFormat() == TSF_G8
						? EOpacityMaskChannel::Red
						: EOpacityMaskChannel::Alpha;
					OutSource += FString::Printf(TEXT(" inferred channel %s"), GetOpacityMaskChannelName(OutChannel));
					return Texture2D;
				}
			}

			TArray<UTexture*> UsedTextures;
			MaterialInterface->GetUsedTextures(UsedTextures);
			for (UTexture* Texture : UsedTextures)
			{
				UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
				if (Texture2D
					&& Texture2D->CompressionSettings != TC_Normalmap
					&& TextureNameLooksLikeOpacityMask(Texture2D->GetName()))
				{
					OutChannel = Texture2D->Source.GetFormat() == TSF_G8
						? EOpacityMaskChannel::Red
						: EOpacityMaskChannel::Alpha;
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
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			const int32 MaterialIndex,
			const FMaterialBakeData& BakeData)
		{
			if (!BakeData.bHasReadableOpacityMaskTexture)
			{
				return 0.0;
			}

			double TotalWeight = 0.0;
			double TransparentWeight = 0.0;
			for (const PlaneCover::FSourceTriangle& Triangle : Triangles)
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
					const float MaskValue = SampleOpacityMaskValue(
						BakeData.OpacityMaskTexture,
						UV,
						BakeData.OpacityMaskChannel);
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
				TEXT("Tint")
			};

			for (const FName ParameterName : ParameterNames)
			{
				FLinearColor Color;
				if (MaterialInterface->GetVectorParameterValue(
					FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)),
					Color))
				{
					return Color;
				}
			}

			return FLinearColor::White;
		}

		float ResolveMaterialScalarParameter(
			UMaterialInterface* MaterialInterface,
			const TConstArrayView<FName> ParameterNames,
			const float DefaultValue)
		{
			if (!MaterialInterface)
			{
				return DefaultValue;
			}

			for (const FName ParameterName : ParameterNames)
			{
				float Value = 0.0f;
				if (MaterialInterface->GetScalarParameterValue(
					FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)),
					Value))
				{
					return FMath::Clamp(Value, 0.0f, 1.0f);
				}
			}

			return DefaultValue;
		}

		float ResolveMaterialVectorParameterMaxChannel(
			UMaterialInterface* MaterialInterface,
			const TConstArrayView<FName> ParameterNames,
			const float DefaultValue)
		{
			if (!MaterialInterface)
			{
				return DefaultValue;
			}

			for (const FName ParameterName : ParameterNames)
			{
				FLinearColor Value;
				if (MaterialInterface->GetVectorParameterValue(
					FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)),
					Value))
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
			FMaterialResolveStats& InOutStats,
			const bool bUseVectorMaxParameterFallback = false)
		{
			FMaterialScalarBakeData BakeData;
			BakeData.Constant = bUseVectorMaxParameterFallback
				? ResolveMaterialVectorParameterMaxChannel(MaterialInterface, ScalarParameterNames, DefaultValue)
				: ResolveMaterialScalarParameter(MaterialInterface, ScalarParameterNames, DefaultValue);

			EOpacityMaskChannel Channel = EOpacityMaskChannel::Red;
			bool bUseLuminance = false;
			FString Source;
			if (UTexture2D* Texture = FindMaterialPropertyTexture(
				MaterialInterface,
				Property,
				PropertyLabel,
				Channel,
				bUseLuminance,
				Source))
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
			const TArray<PlaneCover::FSourceTriangle>& Triangles,
			UMaterialInterface* MaterialInterface,
			const FMaterialOutputSelection& OutputSelection,
			const int32 SourceMaterialBakeResolution,
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
			const int32 ClampedBakeResolution = FMath::Clamp(SourceMaterialBakeResolution, 256, 8192);
			const FIntPoint BakeSize(ClampedBakeResolution, ClampedBakeResolution);

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
			const int32 SourceUVChannelCount = FMath::Max(
				1,
				SourceAttributes.GetVertexInstanceUVs().GetNumChannels());
			MeshSettings.LightMapIndex = FMath::Clamp(
				SourceStaticMesh.GetLightMapCoordinateIndex(),
				0,
				SourceUVChannelCount - 1);
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
				EvaluatedOpacityMaskData.OpacityMaskChannel = EOpacityMaskChannel::Red;
				EvaluatedOpacityMaskData.OpacityMaskClipValue = InOutBakeData.OpacityMaskClipValue;
				const double EvaluatedTransparentRatio = EstimateOpacityMaskTransparentRatio(
					Triangles,
					MaterialIndex,
					EvaluatedOpacityMaskData);
				const double ExistingTransparentRatio = InOutBakeData.bHasReadableOpacityMaskTexture
					? EstimateOpacityMaskTransparentRatio(Triangles, MaterialIndex, InOutBakeData)
					: 0.0;
				constexpr double MinimumUsefulOpacityMaskTransparentRatio = 0.001;
				constexpr double MinimumEvaluatedToExistingOpacityRatio = 0.25;
				const bool bEvaluatedPreservesExistingCutout =
					ExistingTransparentRatio < MinimumUsefulOpacityMaskTransparentRatio
					|| EvaluatedTransparentRatio >= ExistingTransparentRatio * MinimumEvaluatedToExistingOpacityRatio;
				if (EvaluatedTransparentRatio >= MinimumUsefulOpacityMaskTransparentRatio
					&& bEvaluatedPreservesExistingCutout)
				{
					InOutBakeData.OpacityMaskTexture = MoveTemp(EvaluatedOpacityMaskTexture);
					InOutBakeData.bHasReadableOpacityMaskTexture = true;
					InOutBakeData.bUseTextureAlphaAsOpacity = true;
					InOutBakeData.OpacityMaskChannel = EOpacityMaskChannel::Red;
					InOutBakeData.OpacityMaskSource = FString::Printf(
						TEXT("evaluated material OpacityMask output %dx%d"),
						BakeSize.X,
						BakeSize.Y);
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

			auto ApplyScalarOutput = [&](
				const EMaterialProperty Property,
				const TCHAR* PropertyLabel,
				FMaterialScalarBakeData& ScalarBakeData,
				const bool bUseLuminance)
			{
				if (CopyBakedPropertyToTexture(BakeOutput, Property, ScalarBakeData.Texture))
				{
					ScalarBakeData.bHasReadableTexture = true;
					ScalarBakeData.Channel = EOpacityMaskChannel::Red;
					ScalarBakeData.bUseLuminance = bUseLuminance;
					ScalarBakeData.Source = FString::Printf(
						TEXT("evaluated material %s output %dx%d"),
						PropertyLabel,
						BakeSize.X,
						BakeSize.Y);
					bAppliedAnyProperty = true;
				}
			};

			ApplyScalarOutput(MP_AmbientOcclusion, TEXT("AmbientOcclusion"), InOutBakeData.AmbientOcclusion, false);
			ApplyScalarOutput(MP_Roughness, TEXT("Roughness"), InOutBakeData.Roughness, false);
			ApplyScalarOutput(MP_Metallic, TEXT("Metallic"), InOutBakeData.Metallic, false);
			ApplyScalarOutput(MP_EmissiveColor, TEXT("Emissive"), InOutBakeData.Emission, true);

			return bAppliedAnyProperty;
		}
	}

	bool FTexturePixels::IsValid() const
	{
		return Width > 0
			&& Height > 0
			&& !Bytes.IsEmpty()
			&& (Format == TSF_BGRA8 || Format == TSF_G8);
	}

	FColor FTexturePixels::SampleRawColor(const FVector2f& UV) const
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
		return FColor(
			Bytes[ByteIndex + 2],
			Bytes[ByteIndex + 1],
			Bytes[ByteIndex],
			Bytes[ByteIndex + 3]);
	}

	FLinearColor FTexturePixels::Sample(const FVector2f& UV) const
	{
		const FColor RawColor = SampleRawColor(UV);
		return bLinearColor ? RawColor.ReinterpretAsLinear() : FLinearColor(RawColor);
	}

	void FTexturePixels::SetFromColors(
		const TArray<FColor>& Colors,
		const FIntPoint& Size,
		const bool bInLinearColor)
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
			Bytes[ByteIndex] = Color.B;
			Bytes[ByteIndex + 1] = Color.G;
			Bytes[ByteIndex + 2] = Color.R;
			Bytes[ByteIndex + 3] = Color.A;
		}
	}

	bool FTexturePixels::AlphaLooksLikeCutoutMask() const
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

		const double NonOpaqueRatio = PixelCount > 0
			? static_cast<double>(NonOpaquePixels) / static_cast<double>(PixelCount)
			: 0.0;
		return NonOpaqueRatio >= 0.001;
	}

	bool FMaterialOutputSelection::HasAnyOutput() const
	{
		return bBaseColorOpacity || bNormalMask || bMix;
	}

	bool FMaterialKeywordMatchResult::IsMatch(const int32 MaterialIndex) const
	{
		return bEnabled
			&& MatchingMaterialFlags.IsValidIndex(MaterialIndex)
			&& MatchingMaterialFlags[MaterialIndex] != 0;
	}

	float SampleOpacityMaskValue(
		const FTexturePixels& Texture,
		const FVector2f& UV,
		const EOpacityMaskChannel Channel)
	{
		const FLinearColor Sample = Texture.Sample(UV);
		if (Texture.Format == TSF_G8)
		{
			return Sample.R;
		}

		switch (Channel)
		{
		case EOpacityMaskChannel::Red:
			return Sample.R;
		case EOpacityMaskChannel::Green:
			return Sample.G;
		case EOpacityMaskChannel::Blue:
			return Sample.B;
		case EOpacityMaskChannel::Alpha:
			return Sample.A;
		default:
			return Sample.A;
		}
	}

	FMaterialKeywordMatchResult ResolveMaterialKeywordMatches(
		const UStaticMesh& StaticMesh,
		const TArray<FString>& RawKeywords)
	{
		FMaterialKeywordMatchResult Result;
		const TArray<FString> Keywords = BuildNormalizedKeywords(RawKeywords);
		Result.bEnabled = !Keywords.IsEmpty();

		const TArray<FStaticMaterial>& SourceMaterials = StaticMesh.GetStaticMaterials();
		Result.MatchingMaterialFlags.SetNumZeroed(FMath::Max(1, SourceMaterials.Num()));
		if (!Result.bEnabled)
		{
			return Result;
		}

		for (int32 MaterialIndex = 0; MaterialIndex < Result.MatchingMaterialFlags.Num(); ++MaterialIndex)
		{
			const UMaterialInterface* MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
				? SourceMaterials[MaterialIndex].MaterialInterface
				: nullptr;
			if (DoesMaterialOrParentNameMatchKeywords(MaterialInterface, Keywords))
			{
				Result.MatchingMaterialFlags[MaterialIndex] = 1;
				++Result.MatchedMaterialCount;
			}
		}

		return Result;
	}

	TArray<FMaterialBakeData> ResolveMaterialBakeData(
		const UStaticMesh& SourceStaticMesh,
		const int32 SourceLODIndex,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const FMaterialOutputSelection& OutputSelection,
		const int32 SourceMaterialBakeResolution,
		const bool bBakeNormalTexture,
		FMaterialResolveStats& InOutStats)
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
			static const FName AmbientOcclusionParameterNames[] =
			{
				TEXT("AmbientOcclusion"),
				TEXT("Ambient Occlusion"),
				TEXT("AO"),
				TEXT("Occlusion")
			};
			static const FName RoughnessParameterNames[] = { TEXT("Roughness") };
			static const FName MetallicParameterNames[] = { TEXT("Metallic"), TEXT("Metalness"), TEXT("Metal") };
			static const FName EmissionParameterNames[] =
			{
				TEXT("Emission"),
				TEXT("Emissive"),
				TEXT("EmissiveIntensity"),
				TEXT("Emissive Strength")
			};

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

			if (BakeData.AmbientOcclusion.bHasReadableTexture
				|| BakeData.Roughness.bHasReadableTexture
				|| BakeData.Metallic.bHasReadableTexture
				|| BakeData.Emission.bHasReadableTexture)
			{
				++InOutStats.SourceMixTextureMaterials;
			}

			const EBlendMode BlendMode = MaterialInterface ? MaterialInterface->GetBlendMode() : BLEND_Opaque;
			const bool bMaterialCanUseOpacity = BlendMode != BLEND_Opaque;
			BakeData.bTwoSided = MaterialInterface ? MaterialInterface->IsTwoSided() : false;
			const UMaterial* SourceMaterial = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			BakeData.bSourceTangentSpaceNormal = !SourceMaterial || SourceMaterial->bTangentSpaceNormal;
			BakeData.OpacityMaskClipValue = MaterialInterface
				? MaterialInterface->GetOpacityMaskClipValue()
				: 0.33333334f;

			UTexture2D* LoadedBaseColorTexture = nullptr;
			if (UTexture2D* BaseColorTexture = FindBestBaseColorTexture(MaterialInterface))
			{
				BakeData.bHasReadableBaseColorTexture = TryLoadTexturePixels(
					BaseColorTexture,
					BakeData.BaseColorTexture);
				if (BakeData.bHasReadableBaseColorTexture)
				{
					LoadedBaseColorTexture = BaseColorTexture;
					++InOutStats.ReadableMaterialTextures;
				}
			}

			if (bMaterialCanUseOpacity)
			{
				EOpacityMaskChannel OpacityMaskChannel = EOpacityMaskChannel::Alpha;
				FString OpacityMaskSource;
				if (UTexture2D* OpacityMaskTexture = FindOpacityMaskTexture(
					MaterialInterface,
					OpacityMaskChannel,
					OpacityMaskSource))
				{
					BakeData.bHasReadableOpacityMaskTexture = TryLoadTexturePixels(
						OpacityMaskTexture,
						BakeData.OpacityMaskTexture);
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
				BakeData.OpacityMaskChannel = EOpacityMaskChannel::Alpha;
				BakeData.OpacityMaskSource = TEXT("base color alpha cutout fallback");
			}

			const bool bHadMixBeforeEvaluatedBake = BakeData.AmbientOcclusion.bHasReadableTexture
				|| BakeData.Roughness.bHasReadableTexture
				|| BakeData.Metallic.bHasReadableTexture
				|| BakeData.Emission.bHasReadableTexture;
			const bool bHadNormalBeforeEvaluatedBake = BakeData.bHasReadableNormalTexture;
			ApplyEvaluatedMaterialOutputs(
				SourceStaticMesh,
				SourceLODIndex,
				SourceLODBounds,
				MaterialIndex,
				Triangles,
				MaterialInterface,
				OutputSelection,
				SourceMaterialBakeResolution,
				bBakeNormalTexture,
				BakeData);

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
				BakeData.OpacityMaskTransparentRatio = EstimateOpacityMaskTransparentRatio(
					Triangles,
					MaterialIndex,
					BakeData);
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
}
