#include "FoliageBakerMaskedMaterialBaker.h"

#include "DynamicMeshBuilder.h"
#include "Engine/StaticMesh.h"
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
#include "Materials/MaterialInstanceConstant.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerMaterialOverride, Log, All);

bool FFoliageBakerBakeMaterialOverrideSet::Build(
	const UStaticMesh& SourceStaticMesh,
	const TConstArrayView<int32> ReferencedMaterialIndices,
	const bool bEnableStaticSwitchOverrides,
	const TConstArrayView<FFoliageBakerBakeStaticSwitchOverride> InStaticSwitchOverrides,
	FString& OutError)
{
	OverriddenMaterialsByIndex.Reset();
	bEnabled = bEnableStaticSwitchOverrides;
	StaticSwitchOverrides.Reset();
	ReferencedMaterialCount = 0;
	OverriddenMaterialCount = 0;
	AppliedStaticSwitchOverrideCount = 0;
	MissingParameterMaterialNames.Reset();

	if (!bEnabled)
	{
		return true;
	}
	if (InStaticSwitchOverrides.IsEmpty())
	{
		UE_LOG(
			LogFoliageBakerMaterialOverride,
			Warning,
			TEXT("Bake Static Switch Override is enabled, but no overrides are configured; Bake will continue unchanged."));
		return true;
	}

	TSet<FName> SeenParameterNames;
	StaticSwitchOverrides.Reserve(InStaticSwitchOverrides.Num());
	for (int32 OverrideIndex = 0;
		OverrideIndex < InStaticSwitchOverrides.Num();
		++OverrideIndex)
	{
		const FFoliageBakerBakeStaticSwitchOverride& StaticSwitchOverride =
			InStaticSwitchOverrides[OverrideIndex];
		if (StaticSwitchOverride.ParameterName.IsNone())
		{
			OutError = FString::Printf(
				TEXT("Bake Static Switch Override entry %d has a None parameter name."),
				OverrideIndex);
			return false;
		}
		if (SeenParameterNames.Contains(StaticSwitchOverride.ParameterName))
		{
			OutError = FString::Printf(
				TEXT("Bake Static Switch Override contains duplicate parameter '%s'. Each switch may be configured only once."),
				*StaticSwitchOverride.ParameterName.ToString());
			return false;
		}

		SeenParameterNames.Add(StaticSwitchOverride.ParameterName);
		StaticSwitchOverrides.Add(StaticSwitchOverride);
	}

	struct FResolvedMaterialOverride
	{
		TStrongObjectPtr<UMaterialInterface> SourceMaterial;
		TStrongObjectPtr<UMaterialInterface> BakeMaterial;
	};
	struct FResolvedStaticSwitchOverride
	{
		FMaterialParameterInfo ParameterInfo;
		bool bValue = false;
	};

	const TArray<FStaticMaterial>& SourceMaterials =
		SourceStaticMesh.GetStaticMaterials();
	TArray<FResolvedMaterialOverride> ResolvedMaterialOverrides;
	ResolvedMaterialOverrides.Reserve(ReferencedMaterialIndices.Num());

	for (const int32 MaterialIndex : ReferencedMaterialIndices)
	{
		if (!SourceMaterials.IsValidIndex(MaterialIndex)
			|| !SourceMaterials[MaterialIndex].MaterialInterface)
		{
			continue;
		}

		const TStrongObjectPtr<UMaterialInterface> SourceMaterial(
			SourceMaterials[MaterialIndex].MaterialInterface.Get());
		const int32 ExistingOverrideIndex =
			ResolvedMaterialOverrides.IndexOfByPredicate(
				[&SourceMaterial](const FResolvedMaterialOverride& Candidate)
				{
					return Candidate.SourceMaterial == SourceMaterial;
				});
		if (ExistingOverrideIndex != INDEX_NONE)
		{
			if (ResolvedMaterialOverrides[ExistingOverrideIndex].BakeMaterial)
			{
				OverriddenMaterialsByIndex.Add(
					MaterialIndex,
					ResolvedMaterialOverrides[ExistingOverrideIndex].BakeMaterial);
			}
			continue;
		}

		++ReferencedMaterialCount;
		TArray<FMaterialParameterInfo> StaticSwitchParameterInfos;
		TArray<FGuid> StaticSwitchParameterIds;
		SourceMaterial->GetAllStaticSwitchParameterInfo(
			StaticSwitchParameterInfos,
			StaticSwitchParameterIds);

		TArray<FResolvedStaticSwitchOverride> ResolvedStaticSwitchOverrides;
		ResolvedStaticSwitchOverrides.Reserve(StaticSwitchOverrides.Num());
		for (const FFoliageBakerBakeStaticSwitchOverride& StaticSwitchOverride :
			StaticSwitchOverrides)
		{
			const int32 MatchedParameterIndex =
				StaticSwitchParameterInfos.IndexOfByPredicate(
					[&StaticSwitchOverride](
						const FMaterialParameterInfo& ParameterInfo)
					{
						return ParameterInfo.Name
								== StaticSwitchOverride.ParameterName
							&& ParameterInfo.Association
								== EMaterialParameterAssociation::GlobalParameter;
					});
			if (MatchedParameterIndex != INDEX_NONE)
			{
				FResolvedStaticSwitchOverride& ResolvedStaticSwitchOverride =
					ResolvedStaticSwitchOverrides.AddDefaulted_GetRef();
				ResolvedStaticSwitchOverride.ParameterInfo =
					StaticSwitchParameterInfos[MatchedParameterIndex];
				ResolvedStaticSwitchOverride.bValue =
					StaticSwitchOverride.bValue;
			}
			else
			{
				MissingParameterMaterialNames
					.FindOrAdd(StaticSwitchOverride.ParameterName)
					.Add(SourceMaterial->GetPathName());
			}
		}

		FResolvedMaterialOverride& ResolvedOverride =
			ResolvedMaterialOverrides.AddDefaulted_GetRef();
		ResolvedOverride.SourceMaterial = SourceMaterial;
		if (ResolvedStaticSwitchOverrides.IsEmpty())
		{
			continue;
		}

		const TStrongObjectPtr<UMaterialInstanceConstant> TemporaryMaterial(
			NewObject<UMaterialInstanceConstant>(
				GetTransientPackage(),
				NAME_None,
				RF_Transient));
		TemporaryMaterial->SetParentEditorOnly(SourceMaterial.Get());
		for (const FResolvedStaticSwitchOverride& StaticSwitchOverride :
			ResolvedStaticSwitchOverrides)
		{
			TemporaryMaterial->SetStaticSwitchParameterValueEditorOnly(
				StaticSwitchOverride.ParameterInfo,
				StaticSwitchOverride.bValue);
			++AppliedStaticSwitchOverrideCount;
		}
		TemporaryMaterial->UpdateStaticPermutation();
		TemporaryMaterial->PostEditChange();

		ResolvedOverride.BakeMaterial = TemporaryMaterial;
		OverriddenMaterialsByIndex.Add(
			MaterialIndex,
			ResolvedOverride.BakeMaterial);
		++OverriddenMaterialCount;
	}

	for (const FFoliageBakerBakeStaticSwitchOverride& StaticSwitchOverride :
		StaticSwitchOverrides)
	{
		if (MissingParameterMaterialNames.Contains(
				StaticSwitchOverride.ParameterName))
		{
			TArray<FString>& MissingMaterialNames =
				MissingParameterMaterialNames.FindChecked(
					StaticSwitchOverride.ParameterName);
			MissingMaterialNames.Sort();
			UE_LOG(
				LogFoliageBakerMaterialOverride,
				Warning,
				TEXT("Bake static switch '%s' is missing on %d selected-LOD material(s); that switch remains unchanged while any other configured overrides still apply: %s"),
				*StaticSwitchOverride.ParameterName.ToString(),
				MissingMaterialNames.Num(),
				*FString::Join(MissingMaterialNames, TEXT(", ")));
		}
	}
	if (ReferencedMaterialCount == 0)
	{
		UE_LOG(
			LogFoliageBakerMaterialOverride,
			Warning,
			TEXT("Bake static switch overrides could not be applied because no valid selected-LOD source materials were referenced; Bake will continue unchanged."));
	}
	return true;
}

TStrongObjectPtr<UMaterialInterface>
FFoliageBakerBakeMaterialOverrideSet::ResolveMaterial(
	const int32 MaterialIndex) const
{
	return OverriddenMaterialsByIndex.FindRef(MaterialIndex);
}

FString FFoliageBakerBakeMaterialOverrideSet::BuildReportDetails() const
{
	if (!bEnabled)
	{
		return TEXT("disabled");
	}
	if (StaticSwitchOverrides.IsEmpty())
	{
		return TEXT("enabled; no static switch overrides configured; source assets unchanged");
	}

	TArray<FString> OverrideDetails;
	OverrideDetails.Reserve(StaticSwitchOverrides.Num());
	TArray<FString> MissingDetails;
	for (const FFoliageBakerBakeStaticSwitchOverride& StaticSwitchOverride :
		StaticSwitchOverrides)
	{
		OverrideDetails.Add(FString::Printf(
			TEXT("%s=%s"),
			*StaticSwitchOverride.ParameterName.ToString(),
			StaticSwitchOverride.bValue ? TEXT("true") : TEXT("false")));

		if (MissingParameterMaterialNames.Contains(
				StaticSwitchOverride.ParameterName))
		{
			const TArray<FString>& MissingMaterialNames =
				MissingParameterMaterialNames.FindChecked(
					StaticSwitchOverride.ParameterName);
			MissingDetails.Add(FString::Printf(
				TEXT("%s missing on: %s"),
				*StaticSwitchOverride.ParameterName.ToString(),
				*FString::Join(MissingMaterialNames, TEXT(", "))));
		}
	}
	if (ReferencedMaterialCount == 0)
	{
		MissingDetails.Add(
			TEXT("no valid selected-LOD source materials were referenced"));
	}

	const FString WarningDetails = MissingDetails.IsEmpty()
		? FString()
		: FString::Printf(
			TEXT("; warning: %s"),
			*FString::Join(MissingDetails, TEXT("; ")));
	return FString::Printf(
		TEXT("%s; transient child MICs=%d/%d unique selected-LOD materials; applied switch assignments=%d; source assets unchanged%s"),
		*FString::Join(OverrideDetails, TEXT(", ")),
		OverriddenMaterialCount,
		ReferencedMaterialCount,
		AppliedStaticSwitchOverrideCount,
		*WarningDetails);
}

namespace
{
	// FoliageBaker bakes a StaticMesh asset rather than a particular component
	// instance, so PerInstanceCustomData expressions keep their graph defaults.
	class FFoliageBakerExportMaterialCompiler final
		: public FExportMaterialCompiler
	{
	public:
		explicit FFoliageBakerExportMaterialCompiler(
			FMaterialCompiler* InCompiler)
			: FExportMaterialCompiler(InCompiler)
		{
		}

		virtual int32 PerInstanceCustomData(
			int32,
			int32 DefaultValueIndex) override
		{
			return DefaultValueIndex;
		}

		virtual int32 PerInstanceCustomData3Vector(
			int32,
			int32 DefaultValueIndex) override
		{
			return DefaultValueIndex;
		}
	};

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

	class FFoliageBakerFixedFrameWPOShaderElementData final
		: public FMeshMaterialShaderElementData
	{
	public:
		uint32 VertexIndexOffset = 0;
	};

	class FFoliageBakerFixedFrameWPOBoundsVS final
		: public FMeshMaterialShader
	{
		DECLARE_SHADER_TYPE(
			FFoliageBakerFixedFrameWPOBoundsVS,
			MeshMaterial);

	public:
		FFoliageBakerFixedFrameWPOBoundsVS() = default;

		FFoliageBakerFixedFrameWPOBoundsVS(
			const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FMeshMaterialShader(Initializer)
		{
			VertexIndexOffsetParameter.Bind(
				Initializer.ParameterMap,
				TEXT("WPOBoundsVertexIndexOffset"));
		}

		static bool ShouldCompilePermutation(
			const FMeshMaterialShaderPermutationParameters& Parameters)
		{
			return FFoliageBakerDepthCorrectTileVS::ShouldCompilePermutation(
				Parameters);
		}

		void GetShaderBindings(
			const FScene* Scene,
			const ERHIFeatureLevel::Type FeatureLevel,
			const FPrimitiveSceneProxy* PrimitiveSceneProxy,
			const FMaterialRenderProxy& MaterialRenderProxy,
			const FMaterial& Material,
			const FMeshMaterialShaderElementData& ShaderElementData,
			FMeshDrawSingleShaderBindings& ShaderBindings) const
		{
			FMeshMaterialShader::GetShaderBindings(
				Scene,
				FeatureLevel,
				PrimitiveSceneProxy,
				MaterialRenderProxy,
				Material,
				ShaderElementData,
				ShaderBindings);
			const FFoliageBakerFixedFrameWPOShaderElementData& WPOElementData =
				static_cast<
					const FFoliageBakerFixedFrameWPOShaderElementData&>(
						ShaderElementData);
			ShaderBindings.Add(
				VertexIndexOffsetParameter,
				WPOElementData.VertexIndexOffset);
		}

	private:
		LAYOUT_FIELD(FShaderParameter, VertexIndexOffsetParameter);
	};

	class FFoliageBakerFixedFrameWPOBoundsPS final
		: public FMeshMaterialShader
	{
		DECLARE_SHADER_TYPE(
			FFoliageBakerFixedFrameWPOBoundsPS,
			MeshMaterial);

	public:
		FFoliageBakerFixedFrameWPOBoundsPS() = default;

		FFoliageBakerFixedFrameWPOBoundsPS(
			const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FMeshMaterialShader(Initializer)
		{
		}

		static bool ShouldCompilePermutation(
			const FMeshMaterialShaderPermutationParameters& Parameters)
		{
			return FFoliageBakerFixedFrameWPOBoundsVS::
				ShouldCompilePermutation(Parameters);
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

	class FFoliageBakerFixedFrameWPOBoundsMeshProcessor final
		: public FMeshPassProcessor
	{
	public:
		FFoliageBakerFixedFrameWPOBoundsMeshProcessor(
			const ERHIFeatureLevel::Type FeatureLevel,
			const FSceneView* View,
			const FMeshPassProcessorRenderState& RenderState,
			FMeshPassDrawListContext* DrawListContext)
			: FMeshPassProcessor(
				TEXT("FoliageBakerFixedFrameWPOBounds"),
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
			const FMaterialRenderProxy* MaterialRenderProxy =
				MeshBatch.MaterialRenderProxy;
			if (!MaterialRenderProxy || MeshBatch.SegmentIndex < 0)
			{
				return;
			}
			const FMaterial* Material =
				MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (!Material || !Material->GetRenderingThreadShaderMap())
			{
				return;
			}

			FMaterialShaderTypes ShaderTypes;
			ShaderTypes.AddShaderType<
				FFoliageBakerFixedFrameWPOBoundsVS>();
			ShaderTypes.AddShaderType<
				FFoliageBakerFixedFrameWPOBoundsPS>();
			FMaterialShaders Shaders;
			if (!Material->TryGetShaders(
					ShaderTypes,
					MeshBatch.VertexFactory->GetType(),
					Shaders))
			{
				return;
			}

			TMeshProcessorShaders<
				FFoliageBakerFixedFrameWPOBoundsVS,
				FFoliageBakerFixedFrameWPOBoundsPS> PassShaders;
			Shaders.TryGetVertexShader(PassShaders.VertexShader);
			Shaders.TryGetPixelShader(PassShaders.PixelShader);
			if (!PassShaders.VertexShader.IsValid()
				|| !PassShaders.PixelShader.IsValid())
			{
				return;
			}

			FFoliageBakerFixedFrameWPOShaderElementData ShaderElementData;
			ShaderElementData.VertexIndexOffset =
				static_cast<uint32>(MeshBatch.SegmentIndex);
			ShaderElementData.InitializeMeshMaterialData(
				ViewIfDynamicMeshCommand,
				PrimitiveSceneProxy,
				MeshBatch,
				StaticMeshId,
				false);
			FMeshDrawCommandSortKey SortKey =
				FMeshDrawCommandSortKey::Default;
			SortKey.Generic.VertexShaderHash = 0;
			SortKey.Generic.PixelShaderHash =
				static_cast<uint32>(MeshBatch.SegmentIndex);
			BuildMeshDrawCommands(
				MeshBatch,
				BatchElementMask,
				PrimitiveSceneProxy,
				*MaterialRenderProxy,
				*Material,
				PassDrawRenderState,
				PassShaders,
				FM_Solid,
				CM_None,
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
	IMPLEMENT_MATERIAL_SHADER_TYPE(
		,
		FFoliageBakerFixedFrameWPOBoundsVS,
		TEXT("/Plugin/FoliageBaker/Private/FoliageBakerDepthCorrectTile.usf"),
		TEXT("FixedFrameWPOBoundsVS"),
		SF_Vertex);
	IMPLEMENT_MATERIAL_SHADER_TYPE(
		,
		FFoliageBakerFixedFrameWPOBoundsPS,
		TEXT("/Plugin/FoliageBaker/Private/FoliageBakerDepthCorrectTile.usf"),
		TEXT("FixedFrameWPOBoundsPS"),
		SF_Pixel);

	enum class EFoliageBakerMaskedOutput : uint8
	{
		BaseColor,
		ObjectSpaceNormal,
		SourceTriangleId,
		AmbientOcclusion,
		Roughness,
		Specular,
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
		case EFoliageBakerMaskedOutput::Specular: return TEXT("Specular");
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
		case EFoliageBakerMaskedOutput::Specular:
			return EMaterialShaderMapUsage::MaterialExportSpecular;
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
		// v2: PerInstanceCustomData expressions compile to their graph defaults.
		// v3: source World Position Offset is compiled and evaluated at Time=0.
		constexpr uint32 CompilerSemanticVersion = 0x57504F33u; // "WPO3"

		// Each direct proxy needs a stable plugin-specific material id. Its masked
		// base-pass shader contains the source custom outputs and must not reuse an
		// ordinary MaterialBaking shader map or the other output mode's shader map.
		if (Output == EFoliageBakerMaskedOutput::BaseColor)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424231u, // "FBB1"
				SourceMaterialId.B ^ 0x42415345u, // "BASE"
				SourceMaterialId.C ^ 0x434F4C4Fu, // "COLO"
				SourceMaterialId.D ^ 0x52535247u ^ CompilerSemanticVersion); // "RSRG"
		}
		if (Output == EFoliageBakerMaskedOutput::ObjectSpaceNormal)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424E32u, // "FBN2"
				SourceMaterialId.B ^ 0x4F424A45u, // "OBJE"
				SourceMaterialId.C ^ 0x43544E4Fu, // "CTNO"
				SourceMaterialId.D ^ 0x524D414Cu ^ CompilerSemanticVersion);
		}
		if (Output == EFoliageBakerMaskedOutput::AmbientOcclusion)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642414Fu,
				SourceMaterialId.B ^ 0x414D4249u,
				SourceMaterialId.C ^ 0x454E544Fu,
				SourceMaterialId.D ^ 0x43434C55u ^ CompilerSemanticVersion);
		}
		if (Output == EFoliageBakerMaskedOutput::Roughness)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642524Fu,
				SourceMaterialId.B ^ 0x5547484Eu,
				SourceMaterialId.C ^ 0x45535330u,
				SourceMaterialId.D ^ 0x4D495831u ^ CompilerSemanticVersion);
		}
		if (Output == EFoliageBakerMaskedOutput::Specular)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46425350u,
				SourceMaterialId.B ^ 0x4543554Cu,
				SourceMaterialId.C ^ 0x41523030u,
				SourceMaterialId.D ^ 0x41564731u ^ CompilerSemanticVersion);
		}
		if (Output == EFoliageBakerMaskedOutput::Metallic)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424D45u,
				SourceMaterialId.B ^ 0x54414C4Cu,
				SourceMaterialId.C ^ 0x49433030u,
				SourceMaterialId.D ^ 0x4D495832u ^ CompilerSemanticVersion);
		}
		if (Output == EFoliageBakerMaskedOutput::Emission)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x4642454Du,
				SourceMaterialId.B ^ 0x49535349u,
				SourceMaterialId.C ^ 0x4F4E3030u,
				SourceMaterialId.D ^ 0x4D495833u ^ CompilerSemanticVersion);
		}
		return FGuid(
			SourceMaterialId.A ^ 0x46424932u, // "FBI2"
			SourceMaterialId.B ^ 0x534F5552u, // "SOUR"
			SourceMaterialId.C ^ 0x43455452u, // "CETR"
			SourceMaterialId.D ^ 0x49414E47u ^ CompilerSemanticVersion); // "IANG"
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

			MaskedOutputMaterialId = MakeMaskedOutputMaterialId(
				Material->StateId,
				Output);
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
				FFoliageBakerExportMaterialCompiler ProxyCompiler(Compiler);
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
					|| Output == EFoliageBakerMaskedOutput::Specular
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
					else if (Output == EFoliageBakerMaskedOutput::Specular)
					{
						SourceProperty = MP_Specular;
						Compiler->SetSubstrateMaterialExportType(
							SME_Specular,
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
			if (Property == MP_WorldPositionOffset)
			{
				return MaterialInterface->CompileProperty(
					Compiler,
					MP_WorldPositionOffset);
			}
			if (Property == MP_Displacement)
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
				FFoliageBakerExportMaterialCompiler ProxyCompiler(Compiler);
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
				TEXT("FoliageBakerMasked%s_v4_%s"),
				GetMaskedOutputName(Output),
				*GetNameSafe(MaterialInterface.Get()));
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
		virtual UMaterialInterface* GetMaterialInterface() const override { return MaterialInterface.Get(); }
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
		TStrongObjectPtr<UMaterialInterface> MaterialInterface;
		TStrongObjectPtr<UMaterial> Material;
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
			const FMeshData& InMeshSettings,
			const TArray<int32>& InRasterSourceTriangleIndices)
			: LightMapIndex(InMeshSettings.LightMapIndex)
			, SourceMesh(InMeshSettings.Mesh)
			, LCI(MakeUnique<FMeshRenderInfo>(
				InMeshSettings.LightMap,
				nullptr,
				nullptr,
				InMeshSettings.LightmapResourceCluster))
		{
			const FPrimitiveData DefaultPrimitiveData;
			PrimitiveData =
				InMeshSettings.PrimitiveData.Get(DefaultPrimitiveData);
			if (PrimitiveData.CustomPrimitiveData)
			{
				CustomPrimitiveData.Emplace(
					*PrimitiveData.CustomPrimitiveData);
				PrimitiveData.CustomPrimitiveData =
					&CustomPrimitiveData.GetValue();
			}
			PopulateWithMeshData(
				InMeshSettings,
				InRasterSourceTriangleIndices);
		}

		~FFoliageBakerDepthCorrectTileMesh()
		{
			LCI.Reset();
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
				LightMapIndex);
			DynamicMeshBuilder.AddVertices(Vertices);
			DynamicMeshBuilder.AddTriangles(Indices);

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
			MeshElement.LCI = LCI.Get();
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
		void PopulateWithMeshData(
			const FMeshData& MeshSettings,
			const TArray<int32>& RasterSourceTriangleIndices)
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

			constexpr int32 VertexPositionStoredUVChannel = 6;
			const int32 NumTexcoords = FMath::Min(
				VertexInstanceUVs.GetNumChannels(),
				VertexPositionStoredUVChannel);
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
						const FVertexInstanceID SourceVertexInstanceID =
							RawMesh.GetTriangleVertexInstance(TriangleID, Corner);
						const FVertexID SourceVertexID =
							RawMesh.GetVertexInstanceVertex(SourceVertexInstanceID);
						const FVector SourcePosition(VertexPositions[SourceVertexID]);

						FDynamicMeshVertex& Vertex = Vertices.AddDefaulted_GetRef();
						Vertex.Position =
							WorldToLocal.TransformPosition(
								FVector3f(SourcePosition));

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

		int32 LightMapIndex = 0;
		FPrimitiveData PrimitiveData;
		TOptional<FCustomPrimitiveData> CustomPrimitiveData;
		TStrongObjectPtr<const UStaticMesh> SourceMesh;
		TArray<FDynamicMeshVertex> Vertices;
		TArray<uint32> Indices;
		TUniquePtr<FMeshRenderInfo> LCI;
		FMeshBatch MeshElement;
		FMeshBuilderResources MeshBuilderResources;
		bool bMeshElementInitialized = false;
	};

	class FFoliageBakerFixedFrameWPOMesh final
	{
	public:
		FFoliageBakerFixedFrameWPOMesh(
			const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>&
				SourceTriangles,
			const int32 MaterialIndex,
			const FBoxSphereBounds& PrimitiveBounds)
			: PrimitiveData(PrimitiveBounds)
			, LCI(MakeUnique<FMeshRenderInfo>(
				nullptr,
				nullptr,
				nullptr,
				nullptr))
		{
			Populate(SourceTriangles, MaterialIndex);
		}

		~FFoliageBakerFixedFrameWPOMesh()
		{
			LCI.Reset();
			ENQUEUE_RENDER_COMMAND(ReleaseFoliageBakerFixedFrameWPOResources)(
				[ResourcesToRelease = MoveTemp(MeshBuilderResources)](
					FRHICommandListImmediate&) {});
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
				0);
			DynamicMeshBuilder.AddVertices(Vertices);
			DynamicMeshBuilder.AddTriangles(Indices);

			const FPrimitiveUniformShaderParameters PrimitiveParameters =
				FPrimitiveUniformShaderParametersBuilder{}
					.Defaults()
					.LocalToWorld(PrimitiveData.LocalToWorld)
					.ActorWorldPosition(PrimitiveData.ActorPosition)
					.WorldBounds(PrimitiveData.WorldBounds)
					.LocalBounds(PrimitiveData.LocalBounds)
					.PreSkinnedLocalBounds(PrimitiveData.PreSkinnedLocalBounds)
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
			LCI->CreatePrecomputedLightingUniformBuffer_RenderingThread(
				View.GetFeatureLevel());
			MeshElement.LCI = LCI.Get();
			MeshElement.Type = PT_PointList;
			for (FMeshBatchElement& BatchElement : MeshElement.Elements)
			{
				BatchElement.NumPrimitives = Vertices.Num();
				BatchElement.PrimitiveIdMode = PrimID_ForceZero;
			}
			bMeshElementInitialized = true;
		}

		FMeshBatch MakeMeshBatch(
			FMaterialRenderProxy& MaterialRenderProxy,
			const int32 VertexIndexOffset) const
		{
			FMeshBatch Result = MeshElement;
			Result.MaterialRenderProxy = &MaterialRenderProxy;
			Result.SegmentIndex = VertexIndexOffset;
			return Result;
		}

		bool HasGeometry() const
		{
			return !Vertices.IsEmpty();
		}

		const TArray<int32>& GetSourceVertexIndices() const
		{
			return SourceVertexIndices;
		}

	private:
		void Populate(
			const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>&
				SourceTriangles,
			const int32 MaterialIndex)
		{
			using UE::FoliageBaker::PlaneCover::FSourceTriangle;
			constexpr int32 VertexPositionStoredUVChannel = 6;
			for (int32 TriangleIndex = 0;
				TriangleIndex < SourceTriangles.Num();
				++TriangleIndex)
			{
				const FSourceTriangle& SourceTriangle =
					SourceTriangles[TriangleIndex];
				if (SourceTriangle.MaterialIndex != MaterialIndex)
				{
					continue;
				}

				const int32 NumTexcoords = FMath::Min(
					SourceTriangle.NumUVChannels,
					VertexPositionStoredUVChannel);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector& SourcePosition =
						SourceTriangle.Vertices[Corner];
					FDynamicMeshVertex& Vertex =
						Vertices.AddDefaulted_GetRef();
					Vertex.Position = FVector3f(SourcePosition);

					const FVector3f TangentX(
						SourceTriangle.VertexTangents[Corner]);
					const FVector3f TangentZ(
						SourceTriangle.VertexNormals[Corner]);
					const FVector3f TangentY =
						FVector3f::CrossProduct(TangentZ, TangentX)
							.GetSafeNormal()
						* SourceTriangle.BinormalSigns[Corner];
					Vertex.SetTangents(TangentX, TangentY, TangentZ);

					for (int32 UVChannel = 0;
						UVChannel < NumTexcoords;
						++UVChannel)
					{
						Vertex.TextureCoordinate[UVChannel] =
							SourceTriangle.UVChannels[UVChannel][Corner];
					}
					for (int32 UVChannel = NumTexcoords;
						UVChannel < VertexPositionStoredUVChannel;
						++UVChannel)
					{
						Vertex.TextureCoordinate[UVChannel] =
							Vertex.TextureCoordinate[
								FMath::Max(NumTexcoords - 1, 0)];
					}
					Vertex.TextureCoordinate[6] =
						FVector2f(SourcePosition.X, SourcePosition.Y);
					Vertex.TextureCoordinate[7] = FVector2f(
						SourcePosition.Z,
						static_cast<float>(TriangleIndex + 1));
					Vertex.Color = FLinearColor(
						SourceTriangle.VertexColors[Corner]).ToFColor(true);

					Indices.Add(Indices.Num());
					SourceVertexIndices.Add(TriangleIndex * 3 + Corner);
				}
			}
		}

		FPrimitiveData PrimitiveData;
		TArray<FDynamicMeshVertex> Vertices;
		TArray<uint32> Indices;
		TArray<int32> SourceVertexIndices;
		TUniquePtr<FMeshRenderInfo> LCI;
		FMeshBatch MeshElement;
		FMeshBuilderResources MeshBuilderResources;
		bool bMeshElementInitialized = false;
	};

	void SetMaskedBakeError(FString& OutError, const FString& Message)
	{
		OutError = Message;
	}

	struct FFoliageBakerMaskedMaterialProxyDeleter
	{
		// TUniquePtr's deleter contract is the boundary that supplies this pointer.
		void operator()(FFoliageBakerMaskedMaterialProxy* MaterialProxy) const
		{
			if (MaterialProxy)
			{
				FMaterial::DeferredDelete(MaterialProxy);
			}
		}
	};

	using FFoliageBakerMaskedMaterialProxyOwner =
		TUniquePtr<
			FFoliageBakerMaskedMaterialProxy,
			FFoliageBakerMaskedMaterialProxyDeleter>;

	struct FFoliageBakerDepthCorrectTileMaterialResources
	{
		TUniquePtr<FFoliageBakerDepthCorrectTileMesh> Mesh;
		FFoliageBakerMaskedMaterialProxyOwner SourceTriangleIdProxy;
		FFoliageBakerMaskedMaterialProxyOwner BaseColorProxy;
		FFoliageBakerMaskedMaterialProxyOwner ObjectSpaceNormalProxy;
		FFoliageBakerMaskedMaterialProxyOwner AmbientOcclusionProxy;
		FFoliageBakerMaskedMaterialProxyOwner RoughnessProxy;
		FFoliageBakerMaskedMaterialProxyOwner SpecularProxy;
		FFoliageBakerMaskedMaterialProxyOwner MetallicProxy;
		FFoliageBakerMaskedMaterialProxyOwner EmissionProxy;
	};

	struct FFoliageBakerFixedFrameWPOMaterialResources
	{
		TUniquePtr<FFoliageBakerFixedFrameWPOMesh> Mesh;
		FFoliageBakerMaskedMaterialProxyOwner MaterialProxy;
		int32 VertexIndexOffset = 0;
	};

	struct FFixedFrameWPORenderCommandPayload
	{
		TStrongObjectPtr<UTextureRenderTarget2D> PositionTarget;
		TArray<FFoliageBakerFixedFrameWPOMaterialResources> Resources;
	};

	struct FDepthCorrectTileRenderCommandPayload
	{
		TStrongObjectPtr<UTextureRenderTarget2D> SourceTriangleIdTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> BaseColorTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> ObjectSpaceNormalTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> AmbientOcclusionTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> RoughnessTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> SpecularTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> MetallicTarget;
		TStrongObjectPtr<UTextureRenderTarget2D> EmissionTarget;
		TArray<FFoliageBakerDepthCorrectTileMaterialResources> Resources;
	};

	TStrongObjectPtr<UTextureRenderTarget2D> CreateFixedFrameWPOPositionTarget(
		const FIntPoint& TextureSize)
	{
		TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget(
			NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
		if (!RenderTarget.IsValid())
		{
			return RenderTarget;
		}
		RenderTarget->ClearColor = FLinearColor::Transparent;
		RenderTarget->TargetGamma = 0.0f;
		RenderTarget->InitCustomFormat(
			TextureSize.X,
			TextureSize.Y,
			PF_A32B32G32R32F,
			true);
		RenderTarget->UpdateResourceImmediate(true);
		return RenderTarget;
	}

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

	FMatrix BuildDepthCorrectTileViewRotationMatrix(
		const FFoliageBakerDepthCorrectTileRequest& Request)
	{
		const FVector AxisU = Request.ProjectionAxisU.GetSafeNormal();
		const FVector AxisV = Request.ProjectionAxisV.GetSafeNormal();
		const FVector DepthAxis =
			Request.CaptureRayDirection.GetSafeNormal();
		return FMatrix(
			FPlane(AxisU.X, AxisV.X, DepthAxis.X, 0.0),
			FPlane(AxisU.Y, AxisV.Y, DepthAxis.Y, 0.0),
			FPlane(AxisU.Z, AxisV.Z, DepthAxis.Z, 0.0),
			FPlane(0.0, 0.0, 0.0, 1.0));
	}

	FVector BuildDepthCorrectTileViewOrigin(
		const FFoliageBakerDepthCorrectTileRequest& Request)
	{
		const double DepthRadius = FMath::Max(
			static_cast<double>(Request.SourceBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);
		return Request.SourceBounds.Origin
			- Request.CaptureRayDirection.GetSafeNormal()
				* (2.0 * DepthRadius);
	}

	FMatrix BuildDepthCorrectTileProjectionMatrix(
		const FFoliageBakerDepthCorrectTileRequest& Request,
		const FVector& ViewOrigin)
	{
		const FVector AxisU = Request.ProjectionAxisU.GetSafeNormal();
		const FVector AxisV = Request.ProjectionAxisV.GetSafeNormal();
		const FVector DepthAxis =
			Request.CaptureRayDirection.GetSafeNormal();
		const double UExtent = FMath::Max(
			Request.ProjectionMaxU - Request.ProjectionMinU,
			UE_DOUBLE_SMALL_NUMBER);
		const double VExtent = FMath::Max(
			Request.ProjectionMaxV - Request.ProjectionMinV,
			UE_DOUBLE_SMALL_NUMBER);
		const double ViewMinU = Request.ProjectionMinU
			- FVector::DotProduct(ViewOrigin, AxisU);
		const double ViewMinV = Request.ProjectionMinV
			- FVector::DotProduct(ViewOrigin, AxisV);
		const double DepthRadius = FMath::Max(
			static_cast<double>(Request.SourceBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);
		const double SourceCenterDepth = FVector::DotProduct(
			Request.SourceBounds.Origin - ViewOrigin,
			DepthAxis);
		const double VScale = Request.bFlipProjectionV
			? 2.0 / VExtent
			: -2.0 / VExtent;
		const double VOffset = Request.bFlipProjectionV
			? -1.0 - 2.0 * ViewMinV / VExtent
			: 1.0 + 2.0 * ViewMinV / VExtent;

		return AdjustProjectionMatrixForRHI(FMatrix(
			FPlane(2.0 / UExtent, 0.0, 0.0, 0.0),
			FPlane(0.0, VScale, 0.0, 0.0),
			FPlane(0.0, 0.0, -1.0 / (2.0 * DepthRadius), 0.0),
			FPlane(
				-1.0 - 2.0 * ViewMinU / UExtent,
				VOffset,
				0.5 + SourceCenterDepth / (2.0 * DepthRadius),
				1.0)));
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

	void AddFixedFrameWPOBoundsDrawPass(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef PositionTarget,
		const FMeshPassProcessorRenderState& RenderState,
		const FSceneView& View,
		const TRDGUniformBufferRef<FSceneUniformParameters>& SceneUniformBuffer,
		const TRDGUniformBufferRef<FInstanceCullingGlobalUniforms>&
			InstanceCullingUniformBuffer,
		const FIntRect& ViewRect,
		const TArray<FMeshBatch>& MeshBatches)
	{
		FFoliageBakerDepthCorrectTilePassParameters* PassParameters =
			GraphBuilder.AllocParameters<
				FFoliageBakerDepthCorrectTilePassParameters>();
		PassParameters->View.View = View.ViewUniformBuffer;
		PassParameters->View.InstancedView =
			View.GetInstancedViewUniformBuffer();
		PassParameters->InstanceCullingDrawParams.Scene =
			SceneUniformBuffer;
		PassParameters->InstanceCullingDrawParams.InstanceCulling =
			InstanceCullingUniformBuffer;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			PositionTarget,
			ERenderTargetLoadAction::EClear);

		AddDrawDynamicMeshPass(
			GraphBuilder,
			RDG_EVENT_NAME("FoliageBaker.FixedFrameWPOBounds"),
			PassParameters,
			View,
			ViewRect,
			[&View, RenderState, &MeshBatches](
				FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
			{
				FFoliageBakerFixedFrameWPOBoundsMeshProcessor
					PassMeshProcessor(
						View.GetFeatureLevel(),
						&View,
						RenderState,
						DynamicMeshPassContext);
				for (const FMeshBatch& MeshBatch : MeshBatches)
				{
					PassMeshProcessor.AddMeshBatch(
						MeshBatch,
						~0ull,
						nullptr);
				}
			},
			true,
			true);
	}

}

bool FFoliageBakerMaskedMaterialBaker::
	EvaluateFixedFrameWorldPositionOffset(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& PrimitiveBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>&
			SourceTriangles,
		const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
		FFoliageBakerFixedFrameWPOResult& OutResult,
		FString& OutError)
{
	using UE::FoliageBaker::PlaneCover::FSourceTriangle;
	OutResult = FFoliageBakerFixedFrameWPOResult();
	OutError.Reset();
	if (!IsInGameThread())
	{
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds evaluation must run on the game thread."));
		return false;
	}
	if (SourceTriangles.IsEmpty()
		|| PrimitiveBounds.SphereRadius <= UE_DOUBLE_SMALL_NUMBER)
	{
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds evaluation received invalid source geometry."));
		return false;
	}

	TSet<int32> ReferencedMaterialSet;
	for (const FSourceTriangle& SourceTriangle : SourceTriangles)
	{
		ReferencedMaterialSet.Add(SourceTriangle.MaterialIndex);
	}
	TArray<int32> ReferencedMaterialIndices = ReferencedMaterialSet.Array();
	ReferencedMaterialIndices.Sort();

	const TArray<FStaticMaterial>& SourceMaterials =
		SourceStaticMesh.GetStaticMaterials();
	struct FResolvedMaterial
	{
		int32 MaterialIndex = INDEX_NONE;
		TStrongObjectPtr<UMaterialInterface> MaterialInterface;
	};
	TArray<FResolvedMaterial> ResolvedMaterials;
	ResolvedMaterials.Reserve(ReferencedMaterialIndices.Num());
	for (const int32 MaterialIndex : ReferencedMaterialIndices)
	{
		if (!SourceMaterials.IsValidIndex(MaterialIndex))
		{
			const FString Message = FString::Printf(
				TEXT("Fixed-frame WPO bounds evaluation references invalid material index %d."),
				MaterialIndex);
			SetMaskedBakeError(OutError, Message);
			return false;
		}

		FResolvedMaterial& ResolvedMaterial =
			ResolvedMaterials.AddDefaulted_GetRef();
		ResolvedMaterial.MaterialIndex = MaterialIndex;
		ResolvedMaterial.MaterialInterface =
			BakeMaterialOverrides.ResolveMaterial(MaterialIndex);
		if (!ResolvedMaterial.MaterialInterface)
		{
			ResolvedMaterial.MaterialInterface.Reset(
				SourceMaterials[MaterialIndex].MaterialInterface.Get());
		}
		if (!ResolvedMaterial.MaterialInterface)
		{
			ResolvedMaterial.MaterialInterface.Reset(
				UMaterial::GetDefaultMaterial(MD_Surface));
		}
	}

	TArray<FFoliageBakerFixedFrameWPOMaterialResources> Resources;
	Resources.Reserve(ResolvedMaterials.Num());
	TArray<int32> OutputToSourceVertexIndex;
	const int64 SourceVertexCount64 =
		static_cast<int64>(SourceTriangles.Num()) * 3;
	if (SourceVertexCount64 <= 0 || SourceVertexCount64 > MAX_int32)
	{
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds source vertex count is unsupported."));
		return false;
	}
	OutputToSourceVertexIndex.Reserve(
		static_cast<int32>(SourceVertexCount64));

	auto FinishProxy = [&OutError](
		FFoliageBakerMaskedMaterialProxy& Proxy) -> bool
	{
		Proxy.FinishCompilation();
		if (Proxy.DidCacheShadersSucceed()
			&& Proxy.GetGameThreadShaderMap())
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
			TEXT("Fixed-frame WPO bounds shader compilation failed.%s"),
			*CompileErrorDetails);
		SetMaskedBakeError(OutError, Message);
		return false;
	};

	for (const FResolvedMaterial& ResolvedMaterial : ResolvedMaterials)
	{
		FFoliageBakerFixedFrameWPOMaterialResources& Resource =
			Resources.AddDefaulted_GetRef();
		Resource.Mesh = MakeUnique<FFoliageBakerFixedFrameWPOMesh>(
			SourceTriangles,
			ResolvedMaterial.MaterialIndex,
			PrimitiveBounds);
		if (!Resource.Mesh->HasGeometry())
		{
			continue;
		}
		Resource.VertexIndexOffset = OutputToSourceVertexIndex.Num();
		OutputToSourceVertexIndex.Append(
			Resource.Mesh->GetSourceVertexIndices());
		Resource.MaterialProxy = FFoliageBakerMaskedMaterialProxyOwner(
			new FFoliageBakerMaskedMaterialProxy(
				*ResolvedMaterial.MaterialInterface,
				EFoliageBakerMaskedOutput::SourceTriangleId));
		if (!FinishProxy(*Resource.MaterialProxy))
		{
			FlushRenderingCommands();
			return false;
		}
	}

	const int32 OutputVertexCount = OutputToSourceVertexIndex.Num();
	constexpr int32 MaxExactFloatOutputIndex = 1 << 24;
	if (OutputVertexCount <= 0
		|| OutputVertexCount != SourceVertexCount64
		|| OutputVertexCount > MaxExactFloatOutputIndex)
	{
		FlushRenderingCommands();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds could not map every source vertex to an exact GPU output index."));
		return false;
	}

	const int32 MaxTextureDimension =
		static_cast<int32>(GetMax2DTextureDimension());
	const int32 TextureWidth = FMath::Min(
		MaxTextureDimension,
		FMath::Max(
			1,
			FMath::CeilToInt(
				FMath::Sqrt(static_cast<double>(OutputVertexCount)))));
	const int32 TextureHeight =
		FMath::DivideAndRoundUp(OutputVertexCount, TextureWidth);
	if (TextureHeight <= 0 || TextureHeight > MaxTextureDimension)
	{
		FlushRenderingCommands();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds output exceeds the maximum 2D texture size."));
		return false;
	}

	const FIntPoint TextureSize(TextureWidth, TextureHeight);
	TStrongObjectPtr<UTextureRenderTarget2D> PositionTarget =
		CreateFixedFrameWPOPositionTarget(TextureSize);
	if (!PositionTarget.IsValid())
	{
		FlushRenderingCommands();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds could not allocate its RGBA32F render target."));
		return false;
	}

	FFoliageBakerDepthCorrectTileRequest ViewRequest;
	ViewRequest.CaptureRayDirection = FVector::ForwardVector;
	ViewRequest.ProjectionAxisU = FVector::RightVector;
	ViewRequest.ProjectionAxisV = FVector::UpVector;
	ViewRequest.SourceBounds = PrimitiveBounds;
	const FVector BoundsOrigin = PrimitiveBounds.Origin;
	const FVector BoundsExtent = PrimitiveBounds.BoxExtent;
	const double ExtentU = FMath::Max(
		static_cast<double>(BoundsExtent.Y),
		1.0);
	const double ExtentV = FMath::Max(
		static_cast<double>(BoundsExtent.Z),
		1.0);
	const double CenterU = FVector::DotProduct(
		BoundsOrigin,
		ViewRequest.ProjectionAxisU);
	const double CenterV = FVector::DotProduct(
		BoundsOrigin,
		ViewRequest.ProjectionAxisV);
	ViewRequest.ProjectionMinU = CenterU - ExtentU;
	ViewRequest.ProjectionMaxU = CenterU + ExtentU;
	ViewRequest.ProjectionMinV = CenterV - ExtentV;
	ViewRequest.ProjectionMaxV = CenterV + ExtentV;
	const FVector ViewOrigin =
		BuildDepthCorrectTileViewOrigin(ViewRequest);
	const FMatrix ViewRotationMatrix =
		BuildDepthCorrectTileViewRotationMatrix(ViewRequest);
	const FMatrix ProjectionMatrix =
		BuildDepthCorrectTileProjectionMatrix(ViewRequest, ViewOrigin);

	const TSharedRef<FFixedFrameWPORenderCommandPayload, ESPMode::ThreadSafe>
		RenderPayload =
			MakeShared<FFixedFrameWPORenderCommandPayload, ESPMode::ThreadSafe>();
	RenderPayload->PositionTarget = PositionTarget;
	RenderPayload->Resources = MoveTemp(Resources);
	ENQUEUE_RENDER_COMMAND(RenderFoliageBakerFixedFrameWPOBounds)(
		[RenderPayload,
		 TextureSize,
		 ViewOrigin,
		 ViewRotationMatrix,
		 ProjectionMatrix](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRenderTargetResource& PositionResource =
				*RenderPayload->PositionTarget->GetRenderTargetResource();
			FSceneViewFamilyContext ViewFamily(
				FSceneViewFamily::ConstructionValues(
					&PositionResource,
					nullptr,
					FEngineShowFlags(ESFIM_Game))
				.SetTime(
					FGameTime::CreateDilated(
						0.0,
						0.0f,
						0.0,
						0.0f)));
			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = &ViewFamily;
			ViewInitOptions.SetViewRectangle(
				FIntRect(FIntPoint::ZeroValue, TextureSize));
			ViewInitOptions.ViewOrigin = ViewOrigin;
			ViewInitOptions.ViewRotationMatrix = ViewRotationMatrix;
			ViewInitOptions.ProjectionMatrix = ProjectionMatrix;
			ViewInitOptions.BackgroundColor = FLinearColor::Transparent;
			GetRendererModule().CreateAndInitSingleView(
				RHICmdList,
				&ViewFamily,
				&ViewInitOptions);
			const FSceneView& View = *ViewFamily.Views[0];

			TArray<FMeshBatch> MeshBatches;
			MeshBatches.Reserve(RenderPayload->Resources.Num());
			for (FFoliageBakerFixedFrameWPOMaterialResources& Resource :
				RenderPayload->Resources)
			{
				if (!Resource.MaterialProxy
					|| !Resource.Mesh
					|| !Resource.Mesh->HasGeometry())
				{
					continue;
				}
				Resource.MaterialProxy->UpdateUniformExpressionCacheIfNeeded(
					RHICmdList,
					View.GetFeatureLevel());
				Resource.Mesh->PrepareMeshElement(
					View,
					*Resource.MaterialProxy);
				MeshBatches.Add(Resource.Mesh->MakeMeshBatch(
					*Resource.MaterialProxy,
					Resource.VertexIndexOffset));
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			GetRendererModule().InitializeSystemTextures(RHICmdList);
			FRDGSystemTextures::Create(GraphBuilder);
			FRDGTextureRef PositionTexture =
				PositionResource.GetRenderTargetTexture(GraphBuilder);
			if (!MeshBatches.IsEmpty())
			{
				FSceneUniformBuffer& SceneUniforms =
					*GetRendererModule()
						.CreateSinglePrimitiveSceneUniformBuffer(
							GraphBuilder,
							View.GetFeatureLevel(),
							MeshBatches[0]);
				const TRDGUniformBufferRef<FSceneUniformParameters>
					SceneUniformBuffer =
						SceneUniforms.GetBuffer(GraphBuilder);
				const TRDGUniformBufferRef<FInstanceCullingGlobalUniforms>
					InstanceCullingUniformBuffer =
						FInstanceCullingContext::
							CreateDummyInstanceCullingUniformBuffer(
								GraphBuilder);

				FMeshPassProcessorRenderState RenderState;
				RenderState.SetBlendState(
					TStaticBlendState<CW_RGBA>::GetRHI());
				RenderState.SetDepthStencilState(
					TStaticDepthStencilState<false, CF_Always>::GetRHI());
				AddFixedFrameWPOBoundsDrawPass(
					GraphBuilder,
					PositionTexture,
					RenderState,
					View,
					SceneUniformBuffer,
					InstanceCullingUniformBuffer,
					FIntRect(FIntPoint::ZeroValue, TextureSize),
					MeshBatches);
			}
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();
	TArray<FLinearColor> PositionPixels;
	FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
	ReadFlags.SetLinearToGamma(false);
	const int32 ExpectedPixelCount = TextureSize.X * TextureSize.Y;
	const bool bReadSucceeded =
		PositionTarget
			->GameThread_GetRenderTargetResource()
			->ReadLinearColorPixels(PositionPixels, ReadFlags)
		&& PositionPixels.Num() == ExpectedPixelCount;
	RenderPayload->Resources.Reset();
	if (!bReadSucceeded)
	{
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds GPU readback failed or returned an invalid size."));
		return false;
	}

	OutResult.Triangles = SourceTriangles;
	TBitArray<> SeenOutputVertices;
	SeenOutputVertices.Init(false, OutputVertexCount);
	TBitArray<> NonFiniteSourceTriangles;
	NonFiniteSourceTriangles.Init(false, SourceTriangles.Num());
	for (const FLinearColor& PositionPixel : PositionPixels)
	{
		if (PositionPixel.A < 0.5f)
		{
			continue;
		}
		const int32 OutputIndex =
			FMath::RoundToInt(PositionPixel.A) - 1;
		if (!SeenOutputVertices.IsValidIndex(OutputIndex)
			|| SeenOutputVertices[OutputIndex])
		{
			OutResult = FFoliageBakerFixedFrameWPOResult();
			SetMaskedBakeError(
				OutError,
				TEXT("Fixed-frame WPO bounds GPU output contained an invalid or duplicate vertex index."));
			return false;
		}

		const FVector Position(
			PositionPixel.R,
			PositionPixel.G,
			PositionPixel.B);
		if (!FMath::IsFinite(Position.X)
			|| !FMath::IsFinite(Position.Y)
			|| !FMath::IsFinite(Position.Z))
		{
			const int32 SourceVertexIndex =
				OutputToSourceVertexIndex[OutputIndex];
			const int32 SourceTriangleIndex = SourceVertexIndex / 3;
			check(NonFiniteSourceTriangles.IsValidIndex(SourceTriangleIndex));
			NonFiniteSourceTriangles[SourceTriangleIndex] = true;
			SeenOutputVertices[OutputIndex] = true;
			continue;
		}

		const int32 SourceVertexIndex =
			OutputToSourceVertexIndex[OutputIndex];
		const int32 SourceTriangleIndex = SourceVertexIndex / 3;
		const int32 SourceCorner = SourceVertexIndex % 3;
		if (!OutResult.Triangles.IsValidIndex(SourceTriangleIndex))
		{
			OutResult = FFoliageBakerFixedFrameWPOResult();
			SetMaskedBakeError(
				OutError,
				TEXT("Fixed-frame WPO output mapped outside the source triangle array."));
			return false;
		}
		OutResult.Triangles[SourceTriangleIndex].Vertices[SourceCorner] =
			Position;
		SeenOutputVertices[OutputIndex] = true;
	}

	if (SeenOutputVertices.CountSetBits() != OutputVertexCount)
	{
		OutResult = FFoliageBakerFixedFrameWPOResult();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO bounds did not receive every source vertex."));
		return false;
	}

	TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>
		RetainedTriangles;
	const int32 RetainedTriangleCount =
		SourceTriangles.Num() - NonFiniteSourceTriangles.CountSetBits();
	RetainedTriangles.Reserve(RetainedTriangleCount);
	OutResult.RetainedSourceTriangleIndices.Reserve(
		RetainedTriangleCount);
	FBox FixedFrameBounds(ForceInit);
	for (int32 SourceTriangleIndex = 0;
		SourceTriangleIndex < OutResult.Triangles.Num();
		++SourceTriangleIndex)
	{
		if (NonFiniteSourceTriangles[SourceTriangleIndex])
		{
			continue;
		}

		UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle =
			OutResult.Triangles[SourceTriangleIndex];
		for (const FVector& Position : Triangle.Vertices)
		{
			FixedFrameBounds += Position;
		}
		RetainedTriangles.Add(MoveTemp(Triangle));
		OutResult.RetainedSourceTriangleIndices.Add(SourceTriangleIndex);
	}
	OutResult.Triangles = MoveTemp(RetainedTriangles);
	if (OutResult.Triangles.IsEmpty() || !FixedFrameBounds.IsValid)
	{
		OutResult = FFoliageBakerFixedFrameWPOResult();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO culled every source triangle with non-finite vertex positions."));
		return false;
	}
	OutResult.Bounds = FBoxSphereBounds(FixedFrameBounds);
	if (OutResult.Bounds.SphereRadius <= UE_DOUBLE_SMALL_NUMBER)
	{
		OutResult = FFoliageBakerFixedFrameWPOResult();
		SetMaskedBakeError(
			OutError,
			TEXT("Fixed-frame WPO collapsed the selected LOD to a zero-size bounds."));
		return false;
	}
	return true;
}

bool FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
	const FFoliageBakerDepthCorrectTileRequest& Request,
	FFoliageBakerDepthCorrectTileResult& OutResult,
	FString& OutError)
{
	OutResult = FFoliageBakerDepthCorrectTileResult();
	OutError.Reset();
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
		|| Request.ProjectionAxisU.IsNearlyZero()
		|| Request.ProjectionAxisV.IsNearlyZero()
		|| FMath::IsNearlyEqual(
			Request.ProjectionMaxU,
			Request.ProjectionMinU)
		|| FMath::IsNearlyEqual(
			Request.ProjectionMaxV,
			Request.ProjectionMinV)
		|| Request.SourceBounds.SphereRadius <= UE_DOUBLE_SMALL_NUMBER
		|| Request.Materials.IsEmpty())
	{
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake received invalid capture settings."));
		return false;
	}
	for (const FFoliageBakerDepthCorrectTileMaterialInput& Input : Request.Materials)
	{
		if (!Input.MaterialInterface
			|| !Input.MeshDescription
			|| Input.MeshSettings.MeshDescription != Input.MeshDescription.Get()
			|| Input.RasterSourceTriangleIndices.Num()
				!= Input.MeshDescription->Triangles().Num())
		{
			SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake received invalid material geometry."));
			return false;
		}
		for (const int32 SourceTriangleIndex : Input.RasterSourceTriangleIndices)
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
	TStrongObjectPtr<UTextureRenderTarget2D> SpecularTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> MetallicTarget;
	TStrongObjectPtr<UTextureRenderTarget2D> EmissionTarget;
	const bool bBakeRoughness = Request.bBakePackedMix || Request.bBakeRoughnessSpecular;
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
		MetallicTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
		EmissionTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor::Black, false);
	}
	if (bBakeRoughness)
	{
		RoughnessTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor(0.5f, 0.5f, 0.5f), false);
	}
	if (Request.bBakeRoughnessSpecular)
	{
		SpecularTarget = CreateDepthCorrectTileRenderTarget(Request.TextureSize, FLinearColor(0.5f, 0.5f, 0.5f), false);
	}
	if (!SourceTriangleIdTarget.IsValid()
		|| (Request.bBakeBaseColor && !BaseColorTarget.IsValid())
		|| (Request.bBakeObjectSpaceNormal && !ObjectSpaceNormalTarget.IsValid())
		|| (bBakeRoughness && !RoughnessTarget.IsValid())
		|| (Request.bBakeRoughnessSpecular && !SpecularTarget.IsValid())
		|| (Request.bBakePackedMix
			&& (!AmbientOcclusionTarget.IsValid()
				|| !MetallicTarget.IsValid()
				|| !EmissionTarget.IsValid())))
	{
		SetMaskedBakeError(OutError, TEXT("Depth-correct tile bake could not allocate its render targets."));
		return false;
	}

	TArray<FFoliageBakerDepthCorrectTileMaterialResources> Resources;
	Resources.Reserve(Request.Materials.Num());
	auto FinishProxy = [&OutError](
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
		SetMaskedBakeError(OutError, Message);
		return false;
	};

	for (const FFoliageBakerDepthCorrectTileMaterialInput& Input : Request.Materials)
	{
		FFoliageBakerDepthCorrectTileMaterialResources& Resource = Resources.AddDefaulted_GetRef();
		Resource.Mesh = MakeUnique<FFoliageBakerDepthCorrectTileMesh>(
			Input.MeshSettings,
			Input.RasterSourceTriangleIndices);
		Resource.SourceTriangleIdProxy =
			FFoliageBakerMaskedMaterialProxyOwner(
				new FFoliageBakerMaskedMaterialProxy(
					*Input.MaterialInterface,
					EFoliageBakerMaskedOutput::SourceTriangleId));
		if (!FinishProxy(*Resource.SourceTriangleIdProxy, TEXT("source-triangle-id")))
		{
			FlushRenderingCommands();
			return false;
		}
		if (Request.bBakeBaseColor)
		{
			Resource.BaseColorProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::BaseColor));
			if (!FinishProxy(*Resource.BaseColorProxy, TEXT("BaseColor")))
			{
				FlushRenderingCommands();
				return false;
			}
		}
		if (Request.bBakeObjectSpaceNormal)
		{
			Resource.ObjectSpaceNormalProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::ObjectSpaceNormal));
			if (!FinishProxy(*Resource.ObjectSpaceNormalProxy, TEXT("object-space-normal")))
			{
				FlushRenderingCommands();
				return false;
			}
		}
		if (Request.bBakePackedMix)
		{
			Resource.AmbientOcclusionProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::AmbientOcclusion));
			Resource.MetallicProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::Metallic));
			Resource.EmissionProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::Emission));
			if (!FinishProxy(*Resource.AmbientOcclusionProxy, TEXT("ambient-occlusion"))
				|| !FinishProxy(*Resource.MetallicProxy, TEXT("metallic"))
				|| !FinishProxy(*Resource.EmissionProxy, TEXT("emission")))
			{
				FlushRenderingCommands();
				return false;
			}
		}
		if (bBakeRoughness)
		{
			Resource.RoughnessProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::Roughness));
			if (!FinishProxy(*Resource.RoughnessProxy, TEXT("roughness")))
			{
				FlushRenderingCommands();
				return false;
			}
		}
		if (Request.bBakeRoughnessSpecular)
		{
			Resource.SpecularProxy =
				FFoliageBakerMaskedMaterialProxyOwner(
					new FFoliageBakerMaskedMaterialProxy(
						*Input.MaterialInterface,
						EFoliageBakerMaskedOutput::Specular));
			if (!FinishProxy(*Resource.SpecularProxy, TEXT("specular")))
			{
				FlushRenderingCommands();
				return false;
			}
		}
	}

	const TSharedRef<FDepthCorrectTileRenderCommandPayload, ESPMode::ThreadSafe>
		RenderPayload =
			MakeShared<FDepthCorrectTileRenderCommandPayload, ESPMode::ThreadSafe>();
	RenderPayload->SourceTriangleIdTarget = SourceTriangleIdTarget;
	RenderPayload->BaseColorTarget = BaseColorTarget;
	RenderPayload->ObjectSpaceNormalTarget = ObjectSpaceNormalTarget;
	RenderPayload->AmbientOcclusionTarget = AmbientOcclusionTarget;
	RenderPayload->RoughnessTarget = RoughnessTarget;
	RenderPayload->SpecularTarget = SpecularTarget;
	RenderPayload->MetallicTarget = MetallicTarget;
	RenderPayload->EmissionTarget = EmissionTarget;
	RenderPayload->Resources = MoveTemp(Resources);
	const FIntPoint TextureSize = Request.TextureSize;
	const FVector ViewOrigin =
		BuildDepthCorrectTileViewOrigin(Request);
	const FMatrix ViewRotationMatrix =
		BuildDepthCorrectTileViewRotationMatrix(Request);
	const FMatrix ProjectionMatrix =
		BuildDepthCorrectTileProjectionMatrix(Request, ViewOrigin);
	ENQUEUE_RENDER_COMMAND(RenderFoliageBakerDepthCorrectTile)(
		[RenderPayload,
		 TextureSize,
		 ViewOrigin,
		 ViewRotationMatrix,
		 ProjectionMatrix](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRenderTargetResource& SourceTriangleIdResource =
				*RenderPayload
					->SourceTriangleIdTarget
					->GetRenderTargetResource();
			FSceneViewFamilyContext ViewFamily(
				FSceneViewFamily::ConstructionValues(
					&SourceTriangleIdResource,
					nullptr,
					FEngineShowFlags(ESFIM_Game))
				.SetTime(
					FGameTime::CreateDilated(
						0.0,
						0.0f,
						0.0,
						0.0f)));
			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = &ViewFamily;
			ViewInitOptions.SetViewRectangle(FIntRect(FIntPoint::ZeroValue, TextureSize));
			ViewInitOptions.ViewOrigin = ViewOrigin;
			ViewInitOptions.ViewRotationMatrix = ViewRotationMatrix;
			ViewInitOptions.ProjectionMatrix = ProjectionMatrix;
			ViewInitOptions.BackgroundColor = FLinearColor::Black;
			ViewInitOptions.OverlayColor = FLinearColor::White;
			GetRendererModule().CreateAndInitSingleView(
				RHICmdList,
				&ViewFamily,
				&ViewInitOptions);
			const FSceneView& View = *ViewFamily.Views[0];

			for (FFoliageBakerDepthCorrectTileMaterialResources& Resource :
				RenderPayload->Resources)
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
				}
				if (Resource.RoughnessProxy)
				{
					Resource.RoughnessProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
				}
				if (Resource.SpecularProxy)
				{
					Resource.SpecularProxy->UpdateUniformExpressionCacheIfNeeded(
						RHICmdList,
						View.GetFeatureLevel());
				}
				if (Resource.MetallicProxy)
				{
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
			TArray<FMeshBatch> SpecularBatches;
			TArray<FMeshBatch> MetallicBatches;
			TArray<FMeshBatch> EmissionBatches;
			SourceTriangleIdBatches.Reserve(RenderPayload->Resources.Num());
			BaseColorBatches.Reserve(RenderPayload->Resources.Num());
			ObjectSpaceNormalBatches.Reserve(RenderPayload->Resources.Num());
			AmbientOcclusionBatches.Reserve(RenderPayload->Resources.Num());
			RoughnessBatches.Reserve(RenderPayload->Resources.Num());
			SpecularBatches.Reserve(RenderPayload->Resources.Num());
			MetallicBatches.Reserve(RenderPayload->Resources.Num());
			EmissionBatches.Reserve(RenderPayload->Resources.Num());
			for (int32 ResourceIndex = 0;
				ResourceIndex < RenderPayload->Resources.Num();
				++ResourceIndex)
			{
				FFoliageBakerDepthCorrectTileMaterialResources& Resource =
					RenderPayload->Resources[ResourceIndex];
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
				}
				if (Resource.RoughnessProxy)
				{
					RoughnessBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.RoughnessProxy,
						ResourceIndex));
				}
				if (Resource.SpecularProxy)
				{
					SpecularBatches.Add(Resource.Mesh->MakeMeshBatch(
						*Resource.SpecularProxy,
						ResourceIndex));
				}
				if (Resource.MetallicProxy)
				{
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
				SourceTriangleIdResource.GetRenderTargetTexture(GraphBuilder);
			FRDGTextureRef BaseColorTexture = RenderPayload->BaseColorTarget
				? RenderPayload->BaseColorTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef ObjectSpaceNormalTexture = RenderPayload->ObjectSpaceNormalTarget
				? RenderPayload->ObjectSpaceNormalTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef AmbientOcclusionTexture = RenderPayload->AmbientOcclusionTarget
				? RenderPayload->AmbientOcclusionTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef RoughnessTexture = RenderPayload->RoughnessTarget
				? RenderPayload->RoughnessTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef SpecularTexture = RenderPayload->SpecularTarget
				? RenderPayload->SpecularTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef MetallicTexture = RenderPayload->MetallicTarget
				? RenderPayload->MetallicTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
				: nullptr;
			FRDGTextureRef EmissionTexture = RenderPayload->EmissionTarget
				? RenderPayload->EmissionTarget->GetRenderTargetResource()->GetRenderTargetTexture(GraphBuilder)
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
				FSceneUniformBuffer& SceneUniforms =
					*GetRendererModule().CreateSinglePrimitiveSceneUniformBuffer(
						GraphBuilder,
						View.GetFeatureLevel(),
						SourceTriangleIdBatches[0]);
				const TRDGUniformBufferRef<FSceneUniformParameters> SceneUniformBuffer =
					SceneUniforms.GetBuffer(GraphBuilder);
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
				auto AddAttributePass = [
					&AttributeRenderState,
					DepthTexture,
					&GraphBuilder,
					&InstanceCullingUniformBuffer,
					&SceneUniformBuffer,
					&View,
					ViewRect](
					const TCHAR* PassName,
					FRDGTextureRef Target,
					const TArray<FMeshBatch>& Batches)
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
				AddAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.AmbientOcclusion"),
					AmbientOcclusionTexture,
					AmbientOcclusionBatches);
				AddAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Roughness"),
					RoughnessTexture,
					RoughnessBatches);
				AddAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Specular"),
					SpecularTexture,
					SpecularBatches);
				AddAttributePass(
					TEXT("FoliageBaker.DepthCorrectTile.Metallic"),
					MetallicTexture,
					MetallicBatches);
				AddAttributePass(
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
	TArray<FColor> SpecularPixels;
	TArray<FColor> MetallicPixels;
	TArray<FColor> EmissionPixels;
	const bool bReadRoughness = !bBakeRoughness
		|| ReadPixels(*RoughnessTarget.Get(), RoughnessPixels);
	const bool bReadSpecular = !Request.bBakeRoughnessSpecular
		|| ReadPixels(*SpecularTarget.Get(), SpecularPixels);
	const bool bReadPackedMix = !Request.bBakePackedMix
		|| (ReadPixels(*AmbientOcclusionTarget.Get(), AmbientOcclusionPixels)
			&& ReadPixels(*MetallicTarget.Get(), MetallicPixels)
			&& ReadPixels(*EmissionTarget.Get(), EmissionPixels));
	RenderPayload->Resources.Reset();
	if (!bReadSourceTriangleId
		|| !bReadBaseColor
		|| !bReadObjectSpaceNormal
		|| !bReadRoughness
		|| !bReadSpecular
		|| !bReadPackedMix)
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
	if (Request.bBakeRoughnessSpecular)
	{
		OutResult.Roughness = MoveTemp(RoughnessPixels);
		OutResult.Specular = MoveTemp(SpecularPixels);
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
