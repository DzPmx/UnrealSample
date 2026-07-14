#include "FoliageBakerMaskedMaterialBaker.h"

#include "DynamicMeshBuilder.h"
#include "EngineModule.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ExportMaterialProxy.h"
#include "MaterialBakingStructures.h"
#include "MaterialRenderItem.h"
#include "MaterialShaderType.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "MeshMaterialShader.h"
#include "MeshPassProcessor.h"
#include "MeshPassProcessor.inl"
#include "InstanceCulling/InstanceCullingContext.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RenderGraphBuilder.h"
#include "RendererInterface.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "SceneUniformBuffer.h"
#include "StaticMeshAttributes.h"
#include "SystemTextures.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	class FFoliageBakerDepthCorrectTileVS final : public FMeshMaterialShader
	{
		DECLARE_SHADER_TYPE(FFoliageBakerDepthCorrectTileVS, MeshMaterial);

	public:
		FFoliageBakerDepthCorrectTileVS() = default;

		FFoliageBakerDepthCorrectTileVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FMeshMaterialShader(Initializer)
		{
		}

		static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
		{
			return IsPCPlatform(Parameters.Platform)
				&& Parameters.VertexFactoryType
				&& Parameters.VertexFactoryType->GetFName() == FName(TEXT("FLocalVertexFactory"));
		}
	};

	class FFoliageBakerDepthCorrectTilePS final : public FMeshMaterialShader
	{
		DECLARE_SHADER_TYPE(FFoliageBakerDepthCorrectTilePS, MeshMaterial);

	public:
		FFoliageBakerDepthCorrectTilePS() = default;

		FFoliageBakerDepthCorrectTilePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FMeshMaterialShader(Initializer)
		{
		}

		static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
		{
			return FFoliageBakerDepthCorrectTileVS::ShouldCompilePermutation(Parameters);
		}
	};

	BEGIN_SHADER_PARAMETER_STRUCT(FFoliageBakerDepthCorrectTilePassParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FFoliageBakerDepthCorrectTileMeshProcessor final : public FMeshPassProcessor
	{
	public:
		FFoliageBakerDepthCorrectTileMeshProcessor(
			const ERHIFeatureLevel::Type FeatureLevel,
			const FSceneView* View,
			const FMeshPassProcessorRenderState& RenderState,
			FMeshPassDrawListContext* DrawListContext)
			: FMeshPassProcessor(
				TEXT("FoliageBakerDepthCorrectTile"),
				nullptr,
				FeatureLevel,
				View,
				DrawListContext)
			, PassDrawRenderState(RenderState)
		{
		}

		virtual void AddMeshBatch(
			const FMeshBatch& MeshBatch,
			const uint64 BatchElementMask,
			const FPrimitiveSceneProxy* PrimitiveSceneProxy,
			const int32 StaticMeshId = -1) override
		{
			const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
			if (!MaterialRenderProxy)
			{
				return;
			}
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (!Material || !Material->GetRenderingThreadShaderMap())
			{
				return;
			}
			FMaterialShaderTypes ShaderTypes;
			ShaderTypes.AddShaderType<FFoliageBakerDepthCorrectTileVS>();
			ShaderTypes.AddShaderType<FFoliageBakerDepthCorrectTilePS>();
			FMaterialShaders Shaders;
			if (!Material->TryGetShaders(ShaderTypes, MeshBatch.VertexFactory->GetType(), Shaders))
			{
				return;
			}
			TMeshProcessorShaders<
				FFoliageBakerDepthCorrectTileVS,
				FFoliageBakerDepthCorrectTilePS> PassShaders;
			Shaders.TryGetVertexShader(PassShaders.VertexShader);
			Shaders.TryGetPixelShader(PassShaders.PixelShader);
			if (!PassShaders.VertexShader.IsValid() || !PassShaders.PixelShader.IsValid())
			{
				return;
			}

			FMeshMaterialShaderElementData ShaderElementData;
			ShaderElementData.InitializeMeshMaterialData(
				ViewIfDynamicMeshCommand,
				PrimitiveSceneProxy,
				MeshBatch,
				StaticMeshId,
				false);
			const FMeshDrawingPolicyOverrideSettings OverrideSettings =
				ComputeMeshOverrideSettings(MeshBatch);
			FMeshDrawCommandSortKey SortKey = FMeshDrawCommandSortKey::Default;
			SortKey.Generic.VertexShaderHash = 0;
			SortKey.Generic.PixelShaderHash = static_cast<uint32>(FMath::Max(MeshBatch.SegmentIndex, 0));
			BuildMeshDrawCommands(
				MeshBatch,
				BatchElementMask,
				PrimitiveSceneProxy,
				*MaterialRenderProxy,
				*Material,
				PassDrawRenderState,
				PassShaders,
				ComputeMeshFillMode(*Material, OverrideSettings),
				ComputeMeshCullMode(*Material, OverrideSettings),
				SortKey,
				EMeshPassFeatures::Default,
				ShaderElementData);
		}

	private:
		FMeshPassProcessorRenderState PassDrawRenderState;
	};

	IMPLEMENT_MATERIAL_SHADER_TYPE(
		,
		FFoliageBakerDepthCorrectTileVS,
		TEXT("/Plugin/FoliageBaker/Private/FoliageBakerDepthCorrectTile.usf"),
		TEXT("MainVS"),
		SF_Vertex);
	IMPLEMENT_MATERIAL_SHADER_TYPE(
		,
		FFoliageBakerDepthCorrectTilePS,
		TEXT("/Plugin/FoliageBaker/Private/FoliageBakerDepthCorrectTile.usf"),
		TEXT("MainPS"),
		SF_Pixel);

	enum class EFoliageBakerMaskedOutput : uint8
	{
		BaseColor,
		ObjectSpaceNormal,
		SourceTriangleId,
		AmbientOcclusion,
		Roughness,
		Metallic,
		Emission,
	};

	const TCHAR* GetMaskedOutputName(const EFoliageBakerMaskedOutput Output)
	{
		switch (Output)
		{
		case EFoliageBakerMaskedOutput::BaseColor: return TEXT("BaseColor");
		case EFoliageBakerMaskedOutput::SourceTriangleId: return TEXT("SourceTriangleId");
		case EFoliageBakerMaskedOutput::AmbientOcclusion: return TEXT("AmbientOcclusion");
		case EFoliageBakerMaskedOutput::Roughness: return TEXT("Roughness");
		case EFoliageBakerMaskedOutput::Metallic: return TEXT("Metallic");
		case EFoliageBakerMaskedOutput::Emission: return TEXT("Emission");
		case EFoliageBakerMaskedOutput::ObjectSpaceNormal:
		default: return TEXT("ObjectSpaceNormal");
		}
	}

	EMaterialShaderMapUsage::Type GetMaskedOutputShaderMapUsage(
		const EFoliageBakerMaskedOutput Output)
	{
		switch (Output)
		{
		case EFoliageBakerMaskedOutput::BaseColor:
			return EMaterialShaderMapUsage::MaterialExportBaseColor;
		case EFoliageBakerMaskedOutput::SourceTriangleId:
			return EMaterialShaderMapUsage::MaterialExportEmissive;
		case EFoliageBakerMaskedOutput::AmbientOcclusion:
			return EMaterialShaderMapUsage::MaterialExportAO;
		case EFoliageBakerMaskedOutput::Roughness:
			return EMaterialShaderMapUsage::MaterialExportRoughness;
		case EFoliageBakerMaskedOutput::Metallic:
			return EMaterialShaderMapUsage::MaterialExportMetallic;
		case EFoliageBakerMaskedOutput::Emission:
			return EMaterialShaderMapUsage::MaterialExportEmissive;
		case EFoliageBakerMaskedOutput::ObjectSpaceNormal:
		default:
			return EMaterialShaderMapUsage::MaterialExportNormal;
		}
	}

	FGuid MakeMaskedOutputMaterialId(
		const FGuid& SourceMaterialId,
		const EFoliageBakerMaskedOutput Output)
	{
		// Each direct proxy needs a stable plugin-specific material id. Its masked
		// base-pass shader contains the source custom outputs and must not reuse an
		// ordinary MaterialBaking shader map or the other output mode's shader map.
		if (Output == EFoliageBakerMaskedOutput::BaseColor)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424231u, // "FBB1"
				SourceMaterialId.B ^ 0x42415345u, // "BASE"
				SourceMaterialId.C ^ 0x434F4C4Fu, // "COLO"
				SourceMaterialId.D ^ 0x52535247u); // "RSRG"
		}
		if (Output == EFoliageBakerMaskedOutput::ObjectSpaceNormal)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424E32u, // "FBN2"
				SourceMaterialId.B ^ 0x4F424A45u, // "OBJE"
				SourceMaterialId.C ^ 0x43544E4Fu, // "CTNO"
				SourceMaterialId.D ^ 0x524D414Cu); // "RMAL"
		}
		if (Output == EFoliageBakerMaskedOutput::AmbientOcclusion)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642414Fu,
				SourceMaterialId.B ^ 0x414D4249u,
				SourceMaterialId.C ^ 0x454E544Fu,
				SourceMaterialId.D ^ 0x43434C55u);
		}
		if (Output == EFoliageBakerMaskedOutput::Roughness)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642524Fu,
				SourceMaterialId.B ^ 0x5547484Eu,
				SourceMaterialId.C ^ 0x45535330u,
				SourceMaterialId.D ^ 0x4D495831u);
		}
		if (Output == EFoliageBakerMaskedOutput::Metallic)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424D45u,
				SourceMaterialId.B ^ 0x54414C4Cu,
				SourceMaterialId.C ^ 0x49433030u,
				SourceMaterialId.D ^ 0x4D495832u);
		}
		if (Output == EFoliageBakerMaskedOutput::Emission)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642454Du,
				SourceMaterialId.B ^ 0x49535349u,
				SourceMaterialId.C ^ 0x4F4E3030u,
				SourceMaterialId.D ^ 0x4D495833u);
		}
		return FGuid(
			SourceMaterialId.A ^ 0x46424932u, // "FBI2"
			SourceMaterialId.B ^ 0x534F5552u, // "SOUR"
			SourceMaterialId.C ^ 0x43455452u, // "CETR"
			SourceMaterialId.D ^ 0x49414E47u); // "IANG"
	}

	// FExportMaterialProxy caches shaders inside its constructor. A derived proxy
	// therefore cannot participate in that first translation, and a second
	// CacheShaders call replaces the good first map with a null map. This direct
	// proxy includes the real clip value and EarlyOpacityMask output during its
	// one and only shader translation.
	class FFoliageBakerMaskedMaterialProxy final : public FMaterial, public FMaterialRenderProxy
	{
	public:
		FFoliageBakerMaskedMaterialProxy(
			UMaterialInterface& InMaterialInterface,
			const EFoliageBakerMaskedOutput InOutput)
			: FMaterial()
			, FMaterialRenderProxy(GetPathNameSafe(InMaterialInterface.GetMaterial()))
			, MaterialInterface(&InMaterialInterface)
			, Material(InMaterialInterface.GetMaterial())
			, ReferencedTextures(InMaterialInterface.GetReferencedTextures())
			, ReferencedTextureCollections(InMaterialInterface.GetReferencedTextureCollections())
			, Output(InOutput)
			, bUseSourceMaskedClip(InMaterialInterface.GetBlendMode() == BLEND_Masked)
		{
			SetQualityLevelProperties(GMaxRHIShaderPlatform);
			const FMaterialResource* Resource = InMaterialInterface.GetMaterialResource(GMaxRHIShaderPlatform);
			if (!Resource || !Material)
			{
				return;
			}

			MaskedOutputMaterialId = MakeMaskedOutputMaterialId(Material->StateId, Output);
			FMaterialShaderMapId ResourceId;
			Resource->BuildShaderMapId(ResourceId, nullptr);

			TArray<FShaderType*> ShaderTypes;
			TArray<FVertexFactoryType*> VertexFactoryTypes;
			TArray<const FShaderPipelineType*> ShaderPipelineTypes;
			GetDependentShaderAndVFTypes(
				ResourceId.LayoutParams,
				ShaderTypes,
				ShaderPipelineTypes,
				VertexFactoryTypes);
			ResourceId.SetShaderDependencies(
				ShaderTypes,
				ShaderPipelineTypes,
				VertexFactoryTypes,
				GMaxRHIShaderPlatform);
			ResourceId.Usage = GetMaskedOutputShaderMapUsage(Output);
			ResourceId.UsageCustomOutput.Reset();
			ResourceId.BaseMaterialId = MaskedOutputMaterialId;
			SetAllowPixelDepthOffset(false);
			bShaderCacheSucceeded = CacheShaders(
				ResourceId,
				EMaterialShaderPrecompileMode::Synchronous,
				nullptr);
		}

		bool DidCacheShadersSucceed() const { return bShaderCacheSucceeded; }
		virtual bool RequiresSynchronousCompilation() const override { return true; }

		virtual bool ShouldCache(const FShaderType* ShaderType, const FVertexFactoryType* VertexFactoryType) const override
		{
			const bool bCorrectVertexFactory = VertexFactoryType
				== FindVertexFactoryType(FName(TEXT("FLocalVertexFactory"), FNAME_Find));
			const bool bPCPlatform = !IsConsolePlatform(GetShaderPlatform());
			const bool bCorrectFrequency = ShaderType->GetFrequency() == SF_Vertex
				|| ShaderType->GetFrequency() == SF_Pixel;
			return bCorrectVertexFactory && bPCPlatform && bCorrectFrequency;
		}

		virtual TArrayView<const TObjectPtr<UObject>> GetReferencedTextures() const override
		{
			return ReferencedTextures;
		}

		virtual TConstArrayView<TObjectPtr<UTextureCollection>> GetReferencedTextureCollections() const override
		{
			return ReferencedTextureCollections;
		}

		virtual void GetStaticParameterSet(FStaticParameterSet& OutSet) const override
		{
			if (const FMaterialResource* Resource = MaterialInterface
				? MaterialInterface->GetMaterialResource(GMaxRHIShaderPlatform)
				: nullptr)
			{
				Resource->GetStaticParameterSet(OutSet);
			}
		}

		virtual const FMaterial* GetMaterialNoFallback(ERHIFeatureLevel::Type) const override
		{
			return GetRenderingThreadShaderMap() ? this : nullptr;
		}

		virtual const FMaterialRenderProxy* GetFallback(ERHIFeatureLevel::Type) const override
		{
			return UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
		}

		virtual bool GetParameterValue(
			EMaterialParameterType Type,
			const FHashedMaterialParameterInfo& ParameterInfo,
			FMaterialParameterValue& OutValue,
			const FMaterialRenderContext& Context) const override
		{
			return MaterialInterface->GetRenderProxy()->GetParameterValue(Type, ParameterInfo, OutValue, Context);
		}

		virtual int32 CompilePropertyAndSetMaterialProperty(
			EMaterialProperty Property,
			FMaterialCompiler* Compiler,
			EShaderFrequency OverrideShaderFrequency,
			bool bUsePreviousFrameTime) const override
		{
			Compiler->SetMaterialProperty(Property, OverrideShaderFrequency, bUsePreviousFrameTime);
			const int32 Result = CompilePropertyWithoutCast(Property, Compiler);
			return Compiler->ForceCast(
				Result,
				FMaterialAttributeDefinitionMap::GetValueType(Property),
				MFCF_ExactMatch | MFCF_ReplicateValue);
		}

		int32 CompilePropertyWithoutCast(EMaterialProperty Property, FMaterialCompiler* Compiler) const
		{
			if (Property == MP_EmissiveColor)
			{
				FExportMaterialCompiler ProxyCompiler(Compiler);
				const uint32 ForceCastFlags = MFCF_ForceCast | MFCF_ExactMatch | MFCF_ReplicateValue;
				if (Output == EFoliageBakerMaskedOutput::BaseColor)
				{
					Compiler->SetSubstrateMaterialExportType(
						SME_BaseColor,
						ESubstrateMaterialExportContext::SMEC_Opaque,
						BLEND_Opaque);
					return MaterialInterface->CompileProperty(
						&ProxyCompiler,
						MP_BaseColor,
						ForceCastFlags);
				}
				if (Output == EFoliageBakerMaskedOutput::SourceTriangleId)
				{
					Compiler->SetSubstrateMaterialExportType(
						SME_Emissive,
						ESubstrateMaterialExportContext::SMEC_Opaque,
						BLEND_Opaque);
					const int32 TriangleIdUV = Compiler->TextureCoordinate(7, false, false);
					const int32 OneBasedTriangleId = Compiler->ComponentMask(
						TriangleIdUV,
						false,
						true,
						false,
						false);
					const int32 Base256 = Compiler->Constant(256.0f);
					const int32 Base65536 = Compiler->Constant(65536.0f);
					const int32 Inv255 = Compiler->Constant(1.0f / 255.0f);
					const int32 HighByte = Compiler->Floor(Compiler->Div(OneBasedTriangleId, Base65536));
					const int32 Remainder = Compiler->Fmod(OneBasedTriangleId, Base65536);
					const int32 MidByte = Compiler->Floor(Compiler->Div(Remainder, Base256));
					const int32 LowByte = Compiler->Fmod(Remainder, Base256);
					return Compiler->AppendVector(
						Compiler->AppendVector(
							Compiler->Mul(HighByte, Inv255),
							Compiler->Mul(MidByte, Inv255)),
						Compiler->Mul(LowByte, Inv255));
				}
				if (Output == EFoliageBakerMaskedOutput::AmbientOcclusion
					|| Output == EFoliageBakerMaskedOutput::Roughness
					|| Output == EFoliageBakerMaskedOutput::Metallic
					|| Output == EFoliageBakerMaskedOutput::Emission)
				{
					EMaterialProperty SourceProperty = MP_AmbientOcclusion;
					if (Output == EFoliageBakerMaskedOutput::Roughness)
					{
						SourceProperty = MP_Roughness;
						Compiler->SetSubstrateMaterialExportType(
							SME_Roughness,
							ESubstrateMaterialExportContext::SMEC_Opaque,
							BLEND_Opaque);
					}
					else if (Output == EFoliageBakerMaskedOutput::Metallic)
					{
						SourceProperty = MP_Metallic;
						Compiler->SetSubstrateMaterialExportType(
							SME_Metallic,
							ESubstrateMaterialExportContext::SMEC_Opaque,
							BLEND_Opaque);
					}
					else if (Output == EFoliageBakerMaskedOutput::Emission)
					{
						SourceProperty = MP_EmissiveColor;
						Compiler->SetSubstrateMaterialExportType(
							SME_Emissive,
							ESubstrateMaterialExportContext::SMEC_Opaque,
							BLEND_Opaque);
					}
					return MaterialInterface->CompileProperty(
						&ProxyCompiler,
						SourceProperty,
						ForceCastFlags);
				}

				Compiler->SetSubstrateMaterialExportType(
					SME_Normal,
					ESubstrateMaterialExportContext::SMEC_Opaque,
					BLEND_Opaque);
				int32 ObjectSpaceNormal = MaterialInterface->CompileProperty(
					&ProxyCompiler,
					MP_Normal,
					ForceCastFlags);
				ObjectSpaceNormal = Compiler->Normalize(ObjectSpaceNormal);
				if (Material && Material->bTangentSpaceNormal)
				{
					ObjectSpaceNormal = Compiler->TransformVector(MCB_Tangent, MCB_World, ObjectSpaceNormal);
					ObjectSpaceNormal = Compiler->Normalize(ObjectSpaceNormal);
				}
				// Runtime applies TwoSidedSign after transforming the material normal to
				// world space (MaterialTemplate.ush). Apply the same post-transform sign
				// here so the encoded local normal matches the source shaded surface.
				if (Material && Material->bTangentSpaceNormal && MaterialInterface->IsTwoSided())
				{
					ObjectSpaceNormal = Compiler->Mul(ObjectSpaceNormal, Compiler->TwoSidedSign());
				}
				ObjectSpaceNormal = Compiler->TransformVector(MCB_World, MCB_Local, ObjectSpaceNormal);
				ObjectSpaceNormal = Compiler->Normalize(ObjectSpaceNormal);
				return Compiler->Add(
					Compiler->Mul(ObjectSpaceNormal, Compiler->Constant(0.5f)),
					Compiler->Constant(0.5f));
			}
			if (Property == MP_WorldPositionOffset || Property == MP_Displacement)
			{
				return Compiler->Constant(0.0f);
			}
			if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
			{
				return MaterialInterface->CompileProperty(Compiler, Property);
			}
			if (Property == MP_OpacityMask)
			{
				// Keep regular OpacityMask in the same MaterialBaking source-position
				// context as exported color/normal. EarlyOpacityMask is intentionally
				// compiled by the engine as a custom output; that node's own contract
				// only permits UV/texture/constant/vertex-color inputs.
				FExportMaterialCompiler ProxyCompiler(Compiler);
				return MaterialInterface->CompileProperty(&ProxyCompiler, MP_OpacityMask);
			}
			if (Property == MP_ShadingModel)
			{
				return MaterialInterface->CompileProperty(Compiler, MP_ShadingModel);
			}
			if (Property == MP_SurfaceThickness)
			{
				return MaterialInterface->CompileProperty(Compiler, MP_SurfaceThickness);
			}
			if (Property == MP_FrontMaterial)
			{
				return Substrate::IsSubstrateEnabled()
					? MaterialInterface->CompileProperty(Compiler, MP_FrontMaterial)
					: Compiler->SubstrateCreateAndRegisterNullMaterial();
			}
			if (Substrate::IsSubstrateEnabled() && Property == MP_Roughness)
			{
				return Compiler->Constant(0.5f);
			}
			return Compiler->Constant(1.0f);
		}

		virtual EMaterialShaderMapUsage::Type GetShaderMapUsage() const override
		{
			return GetMaskedOutputShaderMapUsage(Output);
		}

		virtual FString GetMaterialUsageDescription() const override
		{
			return FString::Printf(
				TEXT("FoliageBakerMasked%s_v3_%s"),
				GetMaskedOutputName(Output),
				*GetNameSafe(MaterialInterface));
		}

		virtual EMaterialDomain GetMaterialDomain() const override { return MD_Surface; }
		virtual bool IsTangentSpaceNormal() const override
		{
			if (const FMaterialResource* Resource = MaterialInterface
				? MaterialInterface->GetMaterialResource(GMaxRHIShaderPlatform)
				: nullptr)
			{
				return Resource->IsTangentSpaceNormal();
			}
			return false;
		}
		virtual bool IsTwoSided() const override { return MaterialInterface && MaterialInterface->IsTwoSided(); }
		virtual bool IsThinSurface() const override { return MaterialInterface && MaterialInterface->IsThinSurface(); }
		virtual bool IsDitheredLODTransition() const override { return MaterialInterface && MaterialInterface->IsDitheredLODTransition(); }
		virtual bool IsLightFunction() const override { return Material && Material->MaterialDomain == MD_LightFunction; }
		virtual bool IsDeferredDecal() const override { return false; }
		virtual bool IsUIMaterial() const override { return Material && Material->MaterialDomain == MD_UI; }
		virtual bool IsVolumetricPrimitive() const override { return Material && Material->MaterialDomain == MD_Volume; }
		virtual bool IsSpecialEngineMaterial() const override
		{
			// This transient export material is compiled synchronously and consumed
			// immediately.  With the editor's per-shader JobCache/DDC path enabled,
			// ProcessCompiledShaderMaps only marks a synchronous material shader map
			// successful when the material is required-complete.  Without this flag the
			// finished clone is installed, left with bCompiledSuccessfully == false,
			// and then cleared by BeginCompileShaderMap before Bake can render it.
			return true;
		}
		virtual bool IsWireframe() const override { return Material && Material->Wireframe == 1; }
		virtual bool IsMasked() const override { return bUseSourceMaskedClip; }
		virtual EBlendMode GetBlendMode() const override
		{
			return bUseSourceMaskedClip ? BLEND_Masked : BLEND_Opaque;
		}
		virtual ERefractionMode GetRefractionMode() const override
		{
			return Material ? static_cast<ERefractionMode>(Material->RefractionMethod) : RM_None;
		}
		virtual bool GetRootNodeOverridesDefaultRefraction() const override { return false; }
		virtual FMaterialShadingModelField GetShadingModels() const override
		{
			return Substrate::IsSubstrateEnabled() && Material
				? Material->GetShadingModels()
				: MSM_DefaultLit;
		}
		virtual bool IsShadingModelFromMaterialExpression() const override { return false; }
		virtual float GetOpacityMaskClipValue() const override
		{
			return MaterialInterface ? MaterialInterface->GetOpacityMaskClipValue() : 0.5f;
		}
		virtual bool GetCastDynamicShadowAsMasked() const override { return false; }
		virtual EMaterialShadingRate GetShadingRate() const override { return EMaterialShadingRate::MSR_1x1; }
		virtual FString GetFriendlyName() const override
		{
			return FString::Printf(
				TEXT("FFoliageBakerMasked%s %s"),
				GetMaskedOutputName(Output),
				MaterialInterface ? *MaterialInterface->GetName() : TEXT("NULL"));
		}
		virtual bool IsPersistent() const override { return true; }
		virtual FGuid GetMaterialId() const override { return MaskedOutputMaterialId; }
		virtual UMaterialInterface* GetMaterialInterface() const override { return MaterialInterface; }
		virtual bool IsUsedWithStaticLighting() const override { return true; }

		virtual void GatherCustomOutputExpressions(TArray<UMaterialExpressionCustomOutput*>& OutCustomOutputs) const override
		{
			if (!Material)
			{
				return;
			}
			Material->GetAllCustomOutputExpressions(OutCustomOutputs);
			Material->IterateDependentFunctions(
				[&OutCustomOutputs](UMaterialFunctionInterface* Function) -> bool
				{
					if (Function)
					{
						for (UMaterialExpression* Expression : Function->GetExpressions())
						{
							if (UMaterialExpressionCustomOutput* CustomOutput = Cast<UMaterialExpressionCustomOutput>(Expression))
							{
								OutCustomOutputs.AddUnique(CustomOutput);
							}
						}
					}
					return true;
				});
		}

		virtual void GatherExpressionsForCustomInterpolators(TArray<UMaterialExpression*>& OutExpressions) const override
		{
			if (Material)
			{
				Material->GetAllExpressionsForCustomInterpolators(OutExpressions);
			}
		}

		virtual bool CheckInValidStateForCompilation(FMaterialCompiler* Compiler) const override
		{
			return Material && Material->CheckInValidStateForCompilation(Compiler);
		}

	private:
		UMaterialInterface* MaterialInterface = nullptr;
		UMaterial* Material = nullptr;
		TArray<TObjectPtr<UObject>> ReferencedTextures;
		TArray<TObjectPtr<UTextureCollection>> ReferencedTextureCollections;
		FGuid MaskedOutputMaterialId;
		EFoliageBakerMaskedOutput Output = EFoliageBakerMaskedOutput::SourceTriangleId;
		bool bUseSourceMaskedClip = false;
		bool bShaderCacheSucceeded = false;
	};

	class FFoliageBakerDepthCorrectTileMesh final
	{
	public:
		FFoliageBakerDepthCorrectTileMesh(
			const FIntPoint& InTextureSize,
			const FMeshData& InMeshSettings,
			const TArray<int32>& InRasterSourceTriangleIndices,
			const FVector& InCaptureRayDirection,
			const FBoxSphereBounds& InSourceBounds)
			: MeshSettings(InMeshSettings)
			, TextureSize(InTextureSize)
			, RasterSourceTriangleIndices(InRasterSourceTriangleIndices)
			, CaptureRayDirection(InCaptureRayDirection.GetSafeNormal())
			, SourceBounds(InSourceBounds)
			, LCI(new FMeshRenderInfo(
				InMeshSettings.LightMap,
				nullptr,
				nullptr,
				InMeshSettings.LightmapResourceCluster))
		{
			PopulateWithMeshData();
		}

		~FFoliageBakerDepthCorrectTileMesh()
		{
			delete LCI;
			LCI = nullptr;
			ENQUEUE_RENDER_COMMAND(ReleaseFoliageBakerDepthCorrectTileResources)(
				[ResourcesToRelease = MoveTemp(MeshBuilderResources)](FRHICommandListImmediate&) {});
		}

		void PrepareMeshElement(
			const FSceneView& View,
			FMaterialRenderProxy& InitialMaterialRenderProxy)
		{
			if (bMeshElementInitialized)
			{
				return;
			}

			FDynamicMeshBuilder DynamicMeshBuilder(
				View.GetFeatureLevel(),
				MAX_STATIC_TEXCOORDS,
				MeshSettings.LightMapIndex);
			DynamicMeshBuilder.AddVertices(Vertices);
			DynamicMeshBuilder.AddTriangles(Indices);

			const FPrimitiveData DefaultPrimitiveData;
			const FPrimitiveData& PrimitiveData = MeshSettings.PrimitiveData.Get(DefaultPrimitiveData);
			const FPrimitiveUniformShaderParameters PrimitiveParameters =
				FPrimitiveUniformShaderParametersBuilder{}
					.Defaults()
					.LocalToWorld(PrimitiveData.LocalToWorld)
					.ActorWorldPosition(PrimitiveData.ActorPosition)
					.WorldBounds(PrimitiveData.WorldBounds)
					.LocalBounds(PrimitiveData.LocalBounds)
					.PreSkinnedLocalBounds(PrimitiveData.PreSkinnedLocalBounds)
					.CustomPrimitiveData(PrimitiveData.CustomPrimitiveData)
					.ReceivesDecals(false)
					.OutputVelocity(false)
					.Build();

			DynamicMeshBuilder.GetMeshElement(
				PrimitiveParameters,
				&InitialMaterialRenderProxy,
				SDPG_Foreground,
				true,
				0,
				MeshBuilderResources,
				MeshElement);
			LCI->CreatePrecomputedLightingUniformBuffer_RenderingThread(View.GetFeatureLevel());
			MeshElement.LCI = LCI;
			bMeshElementInitialized = true;
		}

		FMeshBatch MakeMeshBatch(
			FMaterialRenderProxy& MaterialRenderProxy,
			const int32 MaterialOrder) const
		{
			FMeshBatch Result = MeshElement;
			Result.MaterialRenderProxy = &MaterialRenderProxy;
			Result.SegmentIndex = MaterialOrder;
			for (FMeshBatchElement& BatchElement : Result.Elements)
			{
				BatchElement.PrimitiveIdMode = PrimID_ForceZero;
			}
			return Result;
		}

		bool HasGeometry() const
		{
			return !Vertices.IsEmpty() && !Indices.IsEmpty();
		}

	private:
		void PopulateWithMeshData()
		{
			check(MeshSettings.MeshDescription);
			const FMatrix44f WorldToLocal = MeshSettings.PrimitiveData.IsSet()
				? FMatrix44f(MeshSettings.PrimitiveData->LocalToWorld.Inverse())
				: FMatrix44f::Identity;
			const FMeshDescription& RawMesh = *MeshSettings.MeshDescription;
			check(RasterSourceTriangleIndices.Num() == RawMesh.Triangles().Num());

			FStaticMeshConstAttributes Attributes(RawMesh);
			const TArrayView<const FVector3f> VertexPositions = Attributes.GetVertexPositions().GetRawArray();
			const TArrayView<const FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals().GetRawArray();
			const TArrayView<const FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents().GetRawArray();
			const TArrayView<const float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns().GetRawArray();
			const TArrayView<const FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors().GetRawArray();
			const TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
			const bool bUseCustomUVs = !MeshSettings.CustomTextureCoordinates.IsEmpty();
			if (bUseCustomUVs)
			{
				check(MeshSettings.CustomTextureCoordinates.Num() == VertexInstanceUVs.GetNumElements());
			}

			constexpr int32 VertexPositionStoredUVChannel = 6;
			const int32 NumTexcoords = FMath::Min(
				VertexInstanceUVs.GetNumChannels(),
				VertexPositionStoredUVChannel);
			const double DepthRadius = FMath::Max(
				static_cast<double>(SourceBounds.SphereRadius),
				UE_DOUBLE_SMALL_NUMBER);

			Vertices.Empty(RawMesh.Triangles().Num() * 3);
			Indices.Empty(RawMesh.Triangles().Num() * 3);
			int32 FaceIndex = 0;
			for (const FTriangleID& TriangleID : RawMesh.Triangles().GetElementIDs())
			{
				const FPolygonGroupID PolygonGroupID = RawMesh.GetTrianglePolygonGroup(TriangleID);
				if (MeshSettings.MaterialIndices.Contains(PolygonGroupID.GetValue()))
				{
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const int32 CornerIndex = !MeshSettings.bMirrored ? Corner : 2 - Corner;
						const int32 SourceVertexIndex = FaceIndex * 3 + CornerIndex;
						const FVertexInstanceID SourceVertexInstanceID =
							RawMesh.GetTriangleVertexInstance(TriangleID, Corner);
						const FVertexID SourceVertexID =
							RawMesh.GetVertexInstanceVertex(SourceVertexInstanceID);
						const FVector SourcePosition(VertexPositions[SourceVertexID]);
						const FVector2D BakeUV = bUseCustomUVs
							? MeshSettings.CustomTextureCoordinates[SourceVertexIndex]
							: FVector2D(VertexInstanceUVs.Get(
								SourceVertexInstanceID,
								MeshSettings.TextureCoordinateIndex));
						const double SignedDepth = FVector::DotProduct(
							SourcePosition - SourceBounds.Origin,
							CaptureRayDirection);
						const double LinearDepth = FMath::Clamp(
							(SignedDepth + DepthRadius) / (2.0 * DepthRadius),
							0.0,
							1.0);

						FDynamicMeshVertex& Vertex = Vertices.AddDefaulted_GetRef();
						Vertex.Position = WorldToLocal.TransformPosition(FVector3f(
							static_cast<float>(BakeUV.X * TextureSize.X),
							static_cast<float>(BakeUV.Y * TextureSize.Y),
							static_cast<float>(1.0 - LinearDepth)));

						const FVector3f TangentX = WorldToLocal.TransformVector(
							VertexInstanceTangents[SourceVertexInstanceID]);
						const FVector3f TangentZ = WorldToLocal.TransformVector(
							VertexInstanceNormals[SourceVertexInstanceID]);
						const FVector3f TangentY = FVector3f::CrossProduct(TangentZ, TangentX).GetSafeNormal()
							* VertexInstanceBinormalSigns[SourceVertexInstanceID];
						Vertex.SetTangents(TangentX, TangentY, TangentZ);

						for (int32 UVChannel = 0; UVChannel < NumTexcoords; ++UVChannel)
						{
							Vertex.TextureCoordinate[UVChannel] =
								VertexInstanceUVs.Get(SourceVertexInstanceID, UVChannel);
						}
						for (int32 UVChannel = NumTexcoords; UVChannel < VertexPositionStoredUVChannel; ++UVChannel)
						{
							Vertex.TextureCoordinate[UVChannel] =
								Vertex.TextureCoordinate[FMath::Max(NumTexcoords - 1, 0)];
						}
						Vertex.TextureCoordinate[6] = FVector2f(SourcePosition.X, SourcePosition.Y);
						Vertex.TextureCoordinate[7] = FVector2f(
							SourcePosition.Z,
							static_cast<float>(RasterSourceTriangleIndices[FaceIndex] + 1));
						Vertex.Color = FLinearColor(VertexInstanceColors[SourceVertexInstanceID]).ToFColor(true);
						Indices.Add(Indices.Num());
					}
				}
				++FaceIndex;
			}
		}

		const FMeshData& MeshSettings;
		FIntPoint TextureSize;
		TArray<int32> RasterSourceTriangleIndices;
		FVector CaptureRayDirection;
		FBoxSphereBounds SourceBounds;
		TArray<FDynamicMeshVertex> Vertices;
		TArray<uint32> Indices;
		FMeshRenderInfo* LCI = nullptr;
		FMeshBatch MeshElement;
		FMeshBuilderResources MeshBuilderResources;
		bool bMeshElementInitialized = false;
	};

	void SetMaskedBakeError(FString* OutError, const TCHAR* Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
	}

	struct FFoliageBakerDepthCorrectTileMaterialResources
	{
		TUniquePtr<FFoliageBakerDepthCorrectTileMesh> Mesh;
		FFoliageBakerMaskedMaterialProxy* SourceTriangleIdProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* BaseColorProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* ObjectSpaceNormalProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* AmbientOcclusionProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* RoughnessProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* MetallicProxy = nullptr;
		FFoliageBakerMaskedMaterialProxy* EmissionProxy = nullptr;
	};

	TStrongObjectPtr<UTextureRenderTarget2D> CreateDepthCorrectTileRenderTarget(
		const FIntPoint& TextureSize,
		const FLinearColor& ClearColor,
		const bool bSrgb)
	{
		TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget(
			NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
		if (!RenderTarget.IsValid())
		{
			return RenderTarget;
		}
		RenderTarget->ClearColor = ClearColor;
		RenderTarget->TargetGamma = 0.0f;
		RenderTarget->InitCustomFormat(
			TextureSize.X,
			TextureSize.Y,
			PF_B8G8R8A8,
			!bSrgb);
		RenderTarget->UpdateResourceImmediate(true);
		return RenderTarget;
	}

	FMatrix BuildDepthCorrectTileProjectionMatrix(const FIntPoint& TextureSize)
	{
		const double Width = FMath::Max(TextureSize.X, 1);
		const double Height = FMath::Max(TextureSize.Y, 1);
		return AdjustProjectionMatrixForRHI(FMatrix(
			FPlane(2.0 / Width, 0.0, 0.0, 0.0),
			FPlane(0.0, -2.0 / Height, 0.0, 0.0),
			FPlane(0.0, 0.0, 1.0, 0.0),
			FPlane(-1.0, 1.0, 0.0, 1.0)));
	}

	void AddDepthCorrectTileDrawPass(
		FRDGBuilder& GraphBuilder,
		const TCHAR* PassName,
		FRDGTextureRef ColorTarget,
		FRDGTextureRef DepthTarget,
		const ERenderTargetLoadAction ColorLoadAction,
		const ERenderTargetLoadAction DepthLoadAction,
		const FExclusiveDepthStencil DepthStencilAccess,
		const FMeshPassProcessorRenderState& RenderState,
		const FSceneView& View,
		const TRDGUniformBufferRef<FSceneUniformParameters>& SceneUniformBuffer,
		const TRDGUniformBufferRef<FInstanceCullingGlobalUniforms>& InstanceCullingUniformBuffer,
		const FIntRect& ViewRect,
		const TArray<FMeshBatch>& MeshBatches)
	{
		FFoliageBakerDepthCorrectTilePassParameters* PassParameters =
			GraphBuilder.AllocParameters<FFoliageBakerDepthCorrectTilePassParameters>();
		PassParameters->View.View = View.ViewUniformBuffer;
		PassParameters->View.InstancedView = View.GetInstancedViewUniformBuffer();
		PassParameters->InstanceCullingDrawParams.Scene = SceneUniformBuffer;
		PassParameters->InstanceCullingDrawParams.InstanceCulling = InstanceCullingUniformBuffer;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ColorLoadAction);
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DepthTarget,
			DepthLoadAction,
			ERenderTargetLoadAction::ENoAction,
			DepthStencilAccess);

		AddDrawDynamicMeshPass(
			GraphBuilder,
			RDG_EVENT_NAME("%s", PassName),
			PassParameters,
			View,
			ViewRect,
			[&View, RenderState, &MeshBatches](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
			{
				FFoliageBakerDepthCorrectTileMeshProcessor PassMeshProcessor(
					View.GetFeatureLevel(),
					&View,
					RenderState,
					DynamicMeshPassContext);
				for (const FMeshBatch& MeshBatch : MeshBatches)
				{
					PassMeshProcessor.AddMeshBatch(MeshBatch, ~0ull, nullptr);
				}
			},
			true,
			true);
	}

}

bool FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
	const FFoliageBakerDepthCorrectTileRequest& Request,
	FFoliageBakerDepthCorrectTileResult& OutResult,
	FString* OutError)
{
	OutResult = FFoliageBakerDepthCorrectTileResult();
	if (OutError)
	{
		OutError->Reset();
	}
	if (!IsInGameThread())
	{
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake must run on the game thread."));
		return false;
	}
	const int64 ExpectedPixelCount = static_cast<int64>(Request.TextureSize.X)
		* static_cast<int64>(Request.TextureSize.Y);
	if (Request.TextureSize.X <= 0
		|| Request.TextureSize.Y <= 0
		|| ExpectedPixelCount <= 0
		|| ExpectedPixelCount > MAX_int32
		|| Request.CaptureRayDirection.IsNearlyZero()
		|| Request.SourceBounds.SphereRadius <= UE_DOUBLE_SMALL_NUMBER
		|| Request.Materials.IsEmpty())
	{
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake received invalid capture settings."));
		return false;
	}
	for (const FFoliageBakerDepthCorrectTileMaterialInput& Input : Request.Materials)
	{
		if (!Input.MaterialInterface
			|| !Input.MeshSettings
			|| !Input.MeshSettings->MeshDescription
			|| !Input.RasterSourceTriangleIndices
			|| Input.RasterSourceTriangleIndices->Num()
				!= Input.MeshSettings->MeshDescription->Triangles().Num())
		{
			SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake received invalid material geometry."));
			return false;
		}
		for (const int32 SourceTriangleIndex : *Input.RasterSourceTriangleIndices)
		{
			if (SourceTriangleIndex < 0 || SourceTriangleIndex > 0xFFFFFE)
			{
				SetMaskedBakeError(OutError, TEXT("Depth-correct tile source triangle exceeds the supported 24-bit range."));
				return false;
			}
		}
	}

	TStrongObjectPtr<UTextureRenderTarget2D> SourceTriangleIdTarget =
		CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
	TStrongObjectPtr<UTextureRenderTarget2D> BaseColorTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> ObjectSpaceNormalTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> AmbientOcclusionTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> RoughnessTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> MetallicTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> EmissionTarget;
	if (Request.bBakeBaseColor)
	{
		BaseColorTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, true);
	}
	if (Request.bBakeObjectSpaceNormal)
	{
		ObjectSpaceNormalTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
	}
	if (Request.bBakePackedMix)
	{
		AmbientOcclusionTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::White, false);
		RoughnessTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor(0.5f, 0.5f, 0.5f), false);
		MetallicTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
		EmissionTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
	}
	if (!SourceTriangleIdTarget.IsValid()
		|| (Request.bBakeBaseColor && !BaseColorTarget.IsValid())
		|| (Request.bBakeObjectSpaceNormal && !ObjectSpaceNormalTarget.IsValid())
		|| (Request.bBakePackedMix
			&& (!AmbientOcclusionTarget.IsValid()
				|| !RoughnessTarget.IsValid()
				|| !MetallicTarget.IsValid()
				|| !EmissionTarget.IsValid())))
	{
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake could not allocate its render targets."));
		return false;
	}

	TArray<FFoliageBakerDepthCorrectTileMaterialResources> Resources;
	Resources.Reserve(Request.Materials.Num());
	auto ReleaseResources = [&Resources]()
	{
		for (FFoliageBakerDepthCorrectTileMaterialResources& Resource : Resources)
		{
			if (Resource.SourceTriangleIdProxy)
			{
				FMaterial::DeferredDelete(Resource.SourceTriangleIdProxy);
				Resource.SourceTriangleIdProxy = nullptr;
			}
			if (Resource.BaseColorProxy)
			{
				FMaterial::DeferredDelete(Resource.BaseColorProxy);
				Resource.BaseColorProxy = nullptr;
			}
			if (Resource.ObjectSpaceNormalProxy)
			{
				FMaterial::DeferredDelete(Resource.ObjectSpaceNormalProxy);
				Resource.ObjectSpaceNormalProxy = nullptr;
			}
			if (Resource.AmbientOcclusionProxy)
			{
				FMaterial::DeferredDelete(Resource.AmbientOcclusionProxy);
				Resource.AmbientOcclusionProxy = nullptr;
			}
			if (Resource.RoughnessProxy)
			{
				FMaterial::DeferredDelete(Resource.RoughnessProxy);
				Resource.RoughnessProxy = nullptr;
			}
			if (Resource.MetallicProxy)
			{
				FMaterial::DeferredDelete(Resource.MetallicProxy);
				Resource.MetallicProxy = nullptr;
			}
			if (Resource.EmissionProxy)
			{
				FMaterial::DeferredDelete(Resource.EmissionProxy);
				Resource.EmissionProxy = nullptr;
			}
			Resource.Mesh.Reset();
		}
		Resources.Reset();
	};
	auto FinishProxy = [OutError](
		FFoliageBakerMaskedMaterialProxy& Proxy,
		const TCHAR* OutputName) -> bool
	{
		Proxy.FinishCompilation();
		if (Proxy.DidCacheShadersSucceed() && Proxy.GetGameThreadShaderMap())
		{
			return true;
		}
		FString CompileErrorDetails;
#if WITH_EDITOR
		const TArray<FString>& CompileErrors = Proxy.GetCompileErrors();
		if (!CompileErrors.IsEmpty())
		{
			CompileErrorDetails = FString::Printf(
				TEXT(" Details: %s"),
				*FString::Join(CompileErrors, TEXT(" | ")));
		}
#endif
		const FString Message = FString::Printf(
			TEXT("Depth-correct %s shader compilation failed.%s"),
			OutputName,
			*CompileErrorDetails);
		SetMaskedBakeError(OutError, *Message);
		return false;
	};

	for (const FFoliageBakerDepthCorrectTileMaterialInput& Input : Request.Materials)
	{
		FFoliageBakerDepthCorrectTileMaterialResources& Resource = Resources.AddDefaulted_GetRef();
		Resource.Mesh = MakeUnique<FFoliageBakerDepthCorrectTileMesh>(
			Request.TextureSize,
			*Input.MeshSettings,
			*Input.RasterSourceTriangleIndices,
			Request.CaptureRayDirection,
			Request.SourceBounds);
		Resource.SourceTriangleIdProxy = new FFoliageBakerMaskedMaterialProxy(
			*Input.MaterialInterface,
			EFoliageBakerMaskedOutput::SourceTriangleId);
		if (!FinishProxy(*Resource.SourceTriangleIdProxy, TEXT("source-triangle-id")))
		{
			FlushRenderingCommands();
			ReleaseResources();
			return false;
		}
		if (Request.bBakeBaseColor)
		{
			Resource.BaseColorProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::BaseColor);
			if (!FinishProxy(*Resource.BaseColorProxy, TEXT("BaseColor")))
			{
				FlushRenderingCommands();
				ReleaseResources();
				return false;
			}
		}
		if (Request.bBakeObjectSpaceNormal)
		{
			Resource.ObjectSpaceNormalProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::ObjectSpaceNormal);
			if (!FinishProxy(*Resource.ObjectSpaceNormalProxy, TEXT("object-space-normal")))
			{
				FlushRenderingCommands();
				ReleaseResources();
				return false;
			}
		}
		if (Request.bBakePackedMix)
		{
			Resource.AmbientOcclusionProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::AmbientOcclusion);
			Resource.RoughnessProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::Roughness);
			Resource.MetallicProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::Metallic);
			Resource.EmissionProxy = new FFoliageBakerMaskedMaterialProxy(
				*Input.MaterialInterface,
				EFoliageBakerMaskedOutput::Emission);
			if (!FinishProxy(*Resource.AmbientOcclusionProxy, TEXT("ambient-occlusion"))
				|| !FinishProxy(*Resource.RoughnessProxy, TEXT("roughness"))
				|| !FinishProxy(*Resource.MetallicProxy, TEXT("metallic"))
				|| !FinishProxy(*Resource.EmissionProxy, TEXT("emission")))
			{
				FlushRenderingCommands();
				ReleaseResources();
				return false;
			}
		}
	}

	UTextureRenderTarget2D* SourceTriangleIdTargetPtr = SourceTriangleIdTarget.Get();
	UTextureRenderTarget2D* BaseColorTargetPtr = BaseColorTarget.Get();
	UTextureRenderTarget2D* ObjectSpaceNormalTargetPtr = ObjectSpaceNormalTarget.Get();
	UTextureRenderTarget2D* AmbientOcclusionTargetPtr = AmbientOcclusionTarget.Get();
	UTextureRenderTarget2D* RoughnessTargetPtr = RoughnessTarget.Get();
	UTextureRenderTarget2D* MetallicTargetPtr = MetallicTarget.Get();
	UTextureRenderTarget2D* EmissionTargetPtr = EmissionTarget.Get();
	TArray<FFoliageBakerDepthCorrectTileMaterialResources>* ResourcesPtr = &Resources;
	const FIntPoint TextureSize = Request.TextureSize;
	ENQUEUE_RENDER_COMMAND(RenderFoliageBakerDepthCorrectTile)(
		[SourceTriangleIdTargetPtr,
		 BaseColorTargetPtr,
		 ObjectSpaceNormalTargetPtr,
		 AmbientOcclusionTargetPtr,
		 RoughnessTargetPtr,
		 MetallicTargetPtr,
		 EmissionTargetPtr,
		 ResourcesPtr,
		 TextureSize](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRenderTargetResource* SourceTriangleIdResource =
				SourceTriangleIdTargetPtr->GetRenderTargetResource();
			FSceneViewFamilyContext ViewFamily(
				FSceneViewFamily::ConstructionValues(
					SourceTriangleIdResource,
					nullptr,
					FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime()));
			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = &ViewFamily;
			ViewInitOptions.SetViewRectangle(FIntRect(FIntPoint::ZeroValue, TextureSize));
			ViewInitOptions.ViewOrigin = FVector::ZeroVector;
			ViewInitOptions.ViewRotationMatrix = FMatrix::Identity;
			ViewInitOptions.ProjectionMatrix = BuildDepthCorrectTileProjectionMatrix(TextureSize);
			ViewInitOptions.BackgroundColor = FLinearColor::Black;
			ViewInitOptions.OverlayColor = FLinearColor::White;
			GetRendererModule().CreateAndInitSingleView(
				RHICmdList,
				&ViewFamily,
				&ViewInitOptions);
			const FSceneView& View = *ViewFamily.Views[0];

			for (FFoliageBakerDepthCorrectTileMaterialResources& Resource : *ResourcesPtr)
			{
				Resource.SourceTriangleIdProxy->UpdateUniformExpressionCacheIfNeeded(
					RHICmdList,
					View.GetFeatureLevel());
				if (Resource.BaseColorProxy)
				{
					Resource.BaseColorProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
				}
				if (Resource.ObjectSpaceNormalProxy)
				{
					Resource.ObjectSpaceNormalProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
				}
				if (Resource.AmbientOcclusionProxy)
				{
					Resource.AmbientOcclusionProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
					Resource.RoughnessProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
					Resource.MetallicProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
					Resource.EmissionProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
				}
			}
			FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

			TArray<FMeshBatch> SourceTriangleIdBatches;
			TArray<FMeshBatch> BaseColorBatches;
			TArray<FMeshBatch> ObjectSpaceNormalBatches;
			TArray<FMeshBatch> AmbientOcclusionBatches;
			TArray<FMeshBatch> RoughnessBatches;
			TArray<FMeshBatch> MetallicBatches;
			TArray<FMeshBatch> EmissionBatches;
			SourceTriangleIdBatches.Reserve(ResourcesPtr->Num());
			BaseColorBatches.Reserve(ResourcesPtr->Num());
			ObjectSpaceNormalBatches.Reserve(ResourcesPtr->Num());
			AmbientOcclusionBatches.Reserve(ResourcesPtr->Num());
			RoughnessBatches.Reserve(ResourcesPtr->Num());
			MetallicBatches.Reserve(ResourcesPtr->Num());
			EmissionBatches.Reserve(ResourcesPtr->Num());
			for (int32 ResourceIndex = 0; ResourceIndex < ResourcesPtr->Num(); ++ResourceIndex)
			{
				FFoliageBakerDepthCorrectTileMaterialResources& Resource = (*ResourcesPtr)[ResourceIndex];
				if (!Resource.Mesh->HasGeometry())
				{
					continue;
				}
				Resource.Mesh->PrepareMeshElement(View, *Resource.SourceTriangleIdProxy);
				SourceTriangleIdBatches.Add(Resource.Mesh->MakeMeshBatch(
					*Resource.SourceTriangleIdProxy,
					ResourceIndex));
				if (Resource.BaseColorProxy)
				{
					BaseColorBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.BaseColorProxy,
						ResourceIndex));
				}
				if (Resource.ObjectSpaceNormalProxy)
				{
					ObjectSpaceNormalBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.ObjectSpaceNormalProxy,
						ResourceIndex));
				}
				if (Resource.AmbientOcclusionProxy)
				{
					AmbientOcclusionBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.AmbientOcclusionProxy,
						ResourceIndex));
					RoughnessBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.RoughnessProxy,
						ResourceIndex));
					MetallicBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.MetallicProxy,
						ResourceIndex));
					EmissionBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.EmissionProxy,
						ResourceIndex));
				}
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			GetRendererModule().InitializeSystemTextures(RHICmdList);
			FRDGSystemTextures::Create(GraphBuilder);
			FRDGTextureRef SourceTriangleIdTexture =
				SourceTriangleIdResource->GetRenderTargetTexture(GraphBuilder);
			FRDGTextureRef BaseColorTexture = BaseColorTargetPtr
				? BaseColorTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef ObjectSpaceNormalTexture = ObjectSpaceNormalTargetPtr
				? ObjectSpaceNormalTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef AmbientOcclusionTexture = AmbientOcclusionTargetPtr
				? AmbientOcclusionTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef RoughnessTexture = RoughnessTargetPtr
				? RoughnessTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef MetallicTexture = MetallicTargetPtr
				? MetallicTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef EmissionTexture = EmissionTargetPtr
				? EmissionTargetPtr->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef DepthTexture = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(
					TextureSize,
					PF_DepthStencil,
					FClearValueBinding::DepthFar,
					TexCreate_DepthStencilTargetable | TexCreate_ShaderResource),
				TEXT("FoliageBaker.DepthCorrectTile.Depth"));

			if (!SourceTriangleIdBatches.IsEmpty())
			{
				FSceneUniformBuffer* SceneUniforms =
					GetRendererModule().CreateSinglePrimitiveSceneUniformBuffer(
						GraphBuilder,
						View.GetFeatureLevel(),
						SourceTriangleIdBatches[0]);
				const TRDGUniformBufferRef<FSceneUniformParameters> SceneUniformBuffer =
					SceneUniforms->GetBuffer(GraphBuilder);
				const TRDGUniformBufferRef<FInstanceCullingGlobalUniforms> InstanceCullingUniformBuffer =
					FInstanceCullingContext::CreateDummyInstanceCullingUniformBuffer(GraphBuilder);

				FMeshPassProcessorRenderState WinnerRenderState;
				WinnerRenderState.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
				WinnerRenderState.SetDepthStencilState(
					TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
				const FIntRect ViewRect(FIntPoint::ZeroValue, TextureSize);
				AddDepthCorrectTileDrawPass(
					GraphBuilder,
					TEXT("FoliageBaker.DepthCorrectTile.Winner"),
					SourceTriangleIdTexture,
					DepthTexture,
					ERenderTargetLoadAction::EClear,
					ERenderTargetLoadAction::EClear,
					FExclusiveDepthStencil::DepthWrite_StencilNop,
					WinnerRenderState,
					View,
					SceneUniformBuffer,
					InstanceCullingUniformBuffer,
					ViewRect,
					SourceTriangleIdBatches);

				FMeshPassProcessorRenderState AttributeRenderState;
				AttributeRenderState.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
				AttributeRenderState.SetDepthStencilState(
					TStaticDepthStencilState<false, CF_Equal>::GetRHI());
				if (BaseColorTexture)
				{
					AddDepthCorrectTileDrawPass(
						GraphBuilder,
						TEXT("FoliageBaker.DepthCorrectTile.BaseColor"),
						BaseColorTexture,
						DepthTexture,
						ERenderTargetLoadAction::EClear,
						ERenderTargetLoadAction::ELoad,
						FExclusiveDepthStencil::DepthRead_StencilNop,
						AttributeRenderState,
						View,
						SceneUniformBuffer,
						InstanceCullingUniformBuffer,
						ViewRect,
						BaseColorBatches);
				}
				if (ObjectSpaceNormalTexture)
				{
					AddDepthCorrectTileDrawPass(
						GraphBuilder,
						TEXT("FoliageBaker.DepthCorrectTile.ObjectSpaceNormal"),
						ObjectSpaceNormalTexture,
						DepthTexture,
						ERenderTargetLoadAction::EClear,
						ERenderTargetLoadAction::ELoad,
						FExclusiveDepthStencil::DepthRead_StencilNop,
						AttributeRenderState,
						View,
						SceneUniformBuffer,
						InstanceCullingUniformBuffer,
						ViewRect,
						ObjectSpaceNormalBatches);
				}
				auto AddMixAttributePass = [&](const TCHAR* PassName, FRDGTextureRef Target, const TArray<FMeshBatch>& Batches)
				{
					if (!Target)
					{
						return;
					}
					AddDepthCorrectTileDrawPass(
						GraphBuilder,
						PassName,
						Target,
						DepthTexture,
						ERenderTargetLoadAction::EClear,
						ERenderTargetLoadAction::ELoad,
						FExclusiveDepthStencil::DepthRead_StencilNop,
						AttributeRenderState,
						View,
						SceneUniformBuffer,
						InstanceCullingUniformBuffer,
						ViewRect,
						Batches);
				};
				AddMixAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.AmbientOcclusion"),
					AmbientOcclusionTexture,
					AmbientOcclusionBatches);
				AddMixAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Roughness"),
					RoughnessTexture,
					RoughnessBatches);
				AddMixAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Metallic"),
					MetallicTexture,
					MetallicBatches);
				AddMixAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Emission"),
					EmissionTexture,
					EmissionBatches);
			}
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();
	auto ReadPixels = [ExpectedPixelCount](
		UTextureRenderTarget2D& RenderTarget,
		TArray<FColor>& OutPixels) -> bool
	{
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		return RenderTarget.GameThread_GetRenderTargetResource()->ReadPixels(OutPixels, ReadFlags)
			&& OutPixels.Num() == static_cast<int32>(ExpectedPixelCount);
	};
	const bool bReadSourceTriangleId = ReadPixels(
		*SourceTriangleIdTarget.Get(),
		OutResult.SourceTriangleIdAndDepth);
	const bool bReadBaseColor = !Request.bBakeBaseColor
		|| ReadPixels(*BaseColorTarget.Get(), OutResult.BaseColor);
	const bool bReadObjectSpaceNormal = !Request.bBakeObjectSpaceNormal
		|| ReadPixels(*ObjectSpaceNormalTarget.Get(), OutResult.ObjectSpaceNormal);
	TArray<FColor> AmbientOcclusionPixels;
	TArray<FColor> RoughnessPixels;
	TArray<FColor> MetallicPixels;
	TArray<FColor> EmissionPixels;
	const bool bReadPackedMix = !Request.bBakePackedMix
		|| (ReadPixels(*AmbientOcclusionTarget.Get(), AmbientOcclusionPixels)
			&& ReadPixels(*RoughnessTarget.Get(), RoughnessPixels)
			&& ReadPixels(*MetallicTarget.Get(), MetallicPixels)
			&& ReadPixels(*EmissionTarget.Get(), EmissionPixels));
	ReleaseResources();
	if (!bReadSourceTriangleId || !bReadBaseColor || !bReadObjectSpaceNormal || !bReadPackedMix)
	{
		OutResult = FFoliageBakerDepthCorrectTileResult();
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile GPU readback failed or returned an invalid size."));
		return false;
	}
	if (Request.bBakePackedMix)
	{
		OutResult.PackedMix.SetNumUninitialized(static_cast<int32>(ExpectedPixelCount));
		for (int32 PixelIndex = 0; PixelIndex < OutResult.PackedMix.Num(); ++PixelIndex)
		{
			const FColor& Emission = EmissionPixels[PixelIndex];
			OutResult.PackedMix[PixelIndex] = FColor(
				AmbientOcclusionPixels[PixelIndex].R,
				RoughnessPixels[PixelIndex].R,
				MetallicPixels[PixelIndex].R,
				FMath::Max3(Emission.R, Emission.G, Emission.B));
		}
	}
	return true;
}

int32 FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
	const FColor& EncodedTriangleId)
{
	const uint32 OneBasedTriangleId =
		(static_cast<uint32>(EncodedTriangleId.R) << 16)
		| (static_cast<uint32>(EncodedTriangleId.G) << 8)
		| static_cast<uint32>(EncodedTriangleId.B);
	return OneBasedTriangleId > 0
		? static_cast<int32>(OneBasedTriangleId - 1)
		: INDEX_NONE;
}
