#include "FoliageBakerMaskedMaterialBaker.h"

#include "CanvasRender.h"
#include "CanvasTypes.h"
#include "DynamicMeshBuilder.h"
#include "EngineModule.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ExportMaterialProxy.h"
#include "HAL/IConsoleManager.h"
#include "MaterialBakingStructures.h"
#include "MaterialRenderItem.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "MeshPassProcessor.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RendererInterface.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	enum class EFoliageBakerMaskedOutput : uint8
	{
		BaseColor,
		FinalCoverage,
		ObjectSpaceNormal,
		SourceTriangleId,
	};

	const TCHAR* GetMaskedOutputName(const EFoliageBakerMaskedOutput Output)
	{
		switch (Output)
		{
		case EFoliageBakerMaskedOutput::BaseColor: return TEXT("BaseColor");
		case EFoliageBakerMaskedOutput::FinalCoverage: return TEXT("FinalCoverage");
		case EFoliageBakerMaskedOutput::SourceTriangleId: return TEXT("SourceTriangleId");
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
		case EFoliageBakerMaskedOutput::FinalCoverage:
			return EMaterialShaderMapUsage::MaterialExportOpacityMask;
		case EFoliageBakerMaskedOutput::SourceTriangleId:
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
		if (Output == EFoliageBakerMaskedOutput::FinalCoverage)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424335u, // "FBC5"
				SourceMaterialId.B ^ 0x46494E41u, // "FINA"
				SourceMaterialId.C ^ 0x4C434F56u, // "LCOV"
				SourceMaterialId.D ^ 0x45524147u); // "ERAG"
		}
		if (Output == EFoliageBakerMaskedOutput::ObjectSpaceNormal)
		{
			return FGuid(
				SourceMaterialId.A ^ 0x46424E32u, // "FBN2"
				SourceMaterialId.B ^ 0x4F424A45u, // "OBJE"
				SourceMaterialId.C ^ 0x43544E4Fu, // "CTNO"
				SourceMaterialId.D ^ 0x524D414Cu); // "RMAL"
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
				if (Output == EFoliageBakerMaskedOutput::FinalCoverage)
				{
					Compiler->SetSubstrateMaterialExportType(
						SME_OpacityMask,
						ESubstrateMaterialExportContext::SMEC_Opaque,
						BLEND_Opaque);
					return MaterialInterface->CompileProperty(
						&ProxyCompiler,
						MP_OpacityMask,
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
		EFoliageBakerMaskedOutput Output = EFoliageBakerMaskedOutput::FinalCoverage;
		bool bUseSourceMaskedClip = false;
		bool bShaderCacheSucceeded = false;
	};

	class FFoliageBakerMaskedMaterialRenderItem final : public FCanvasBaseRenderItem
	{
	public:
		FFoliageBakerMaskedMaterialRenderItem(
			const FIntPoint& InTextureSize,
			const FMeshData& InMeshSettings,
			FMaterialRenderProxy& InMaterialRenderProxy,
			const TArray<int32>* InRasterSourceTriangleIndices)
			: MeshSettings(InMeshSettings)
			, TextureSize(InTextureSize)
			, MaterialRenderProxy(InMaterialRenderProxy)
			, LCI(new FMeshRenderInfo(
				InMeshSettings.LightMap,
				nullptr,
				nullptr,
				InMeshSettings.LightmapResourceCluster))
		{
			if (InRasterSourceTriangleIndices)
			{
				RasterSourceTriangleIndices = *InRasterSourceTriangleIndices;
			}
			PopulateWithMeshData();
		}

		virtual ~FFoliageBakerMaskedMaterialRenderItem() override
		{
			delete LCI;
			LCI = nullptr;
			ENQUEUE_RENDER_COMMAND(ReleaseFoliageBakerMaskedMaterialResources)(
				[ResourcesToRelease = MoveTemp(MeshBuilderResources)](FRHICommandListImmediate&) {});
		}

		virtual bool Render_RenderThread(
			FCanvasRenderContext& RenderContext,
			FMeshPassProcessorRenderState&,
			const FCanvas* Canvas) override
		{
			check(ViewFamily);

			const FRenderTarget* CanvasRenderTarget = Canvas->GetRenderTarget();
			const FIntRect ViewRect(FIntPoint::ZeroValue, CanvasRenderTarget->GetSizeXY());

			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = ViewFamily;
			ViewInitOptions.SetViewRectangle(ViewRect);
			ViewInitOptions.ViewOrigin = FVector::ZeroVector;
			ViewInitOptions.ViewRotationMatrix = FMatrix::Identity;
			ViewInitOptions.ProjectionMatrix = Canvas->GetTransformStack().Top().GetMatrix();
			ViewInitOptions.BackgroundColor = FLinearColor::Black;
			ViewInitOptions.OverlayColor = FLinearColor::White;

			FSceneView View(ViewInitOptions);
			View.FinalPostProcessSettings.bOverride_IndirectLightingIntensity = true;
			View.FinalPostProcessSettings.IndirectLightingIntensity = 0.0f;

			if (!Vertices.IsEmpty() && !Indices.IsEmpty())
			{
				FMeshPassProcessorRenderState DrawRenderState;
				DrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
				DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
				QueueMaterial(RenderContext, DrawRenderState, View);
			}
			return true;
		}

		virtual bool Render_GameThread(
			const FCanvas* Canvas,
			FCanvasRenderThreadScope& RenderScope) override
		{
			RenderScope.EnqueueRenderCommand(
				[this, Canvas](FCanvasRenderContext& RenderContext)
				{
					FMeshPassProcessorRenderState DummyRenderState;
					Render_RenderThread(RenderContext, DummyRenderState, Canvas);
				});
			return true;
		}

		FSceneViewFamily* ViewFamily = nullptr;

	private:
		void PopulateWithMeshData()
		{
			check(MeshSettings.MeshDescription);
			const FMatrix44f WorldToLocal = MeshSettings.PrimitiveData.IsSet()
				? FMatrix44f(MeshSettings.PrimitiveData->LocalToWorld.Inverse())
				: FMatrix44f::Identity;

			const FMeshDescription& RawMesh = *MeshSettings.MeshDescription;
			check(RasterSourceTriangleIndices.IsEmpty()
				|| RasterSourceTriangleIndices.Num() == RawMesh.Triangles().Num());
			FStaticMeshConstAttributes Attributes(RawMesh);
			const TArrayView<const FVector3f> VertexPositions = Attributes.GetVertexPositions().GetRawArray();
			const TArrayView<const FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals().GetRawArray();
			const TArrayView<const FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents().GetRawArray();
			const TArrayView<const float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns().GetRawArray();
			const TArrayView<const FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors().GetRawArray();
			const TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();

			const float ScaleX = TextureSize.X
				/ (MeshSettings.TextureCoordinateBox.Max.X - MeshSettings.TextureCoordinateBox.Min.X);
			const float ScaleY = TextureSize.Y
				/ (MeshSettings.TextureCoordinateBox.Max.Y - MeshSettings.TextureCoordinateBox.Min.Y);
			const float OffsetX = -MeshSettings.TextureCoordinateBox.Min.X * ScaleX;
			const float OffsetY = -MeshSettings.TextureCoordinateBox.Min.Y * ScaleY;
			constexpr int32 VertexPositionStoredUVChannel = 6;
			const int32 NumTexcoords = FMath::Min(
				VertexInstanceUVs.GetNumChannels(),
				VertexPositionStoredUVChannel);
			const bool bUseCustomUVs = !MeshSettings.CustomTextureCoordinates.IsEmpty();

			if (bUseCustomUVs)
			{
				check(MeshSettings.CustomTextureCoordinates.Num() == VertexInstanceUVs.GetNumElements());
			}

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

						FDynamicMeshVertex& Vertex = Vertices.AddDefaulted_GetRef();
						const FVector2D BakeUV = bUseCustomUVs
							? MeshSettings.CustomTextureCoordinates[SourceVertexIndex]
							: FVector2D(VertexInstanceUVs.Get(
								SourceVertexInstanceID,
								MeshSettings.TextureCoordinateIndex));
						Vertex.Position = WorldToLocal.TransformPosition(
							FVector3f(OffsetX + BakeUV.X * ScaleX, OffsetY + BakeUV.Y * ScaleY, 0.0f));

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

						Vertex.TextureCoordinate[6].X = VertexPositions[SourceVertexID].X;
						Vertex.TextureCoordinate[6].Y = VertexPositions[SourceVertexID].Y;
						Vertex.TextureCoordinate[7].X = VertexPositions[SourceVertexID].Z;
						Vertex.TextureCoordinate[7].Y = RasterSourceTriangleIndices.IsEmpty()
							? 0.0f
							: static_cast<float>(RasterSourceTriangleIndices[FaceIndex] + 1);
						Vertex.Color = FLinearColor(VertexInstanceColors[SourceVertexInstanceID]).ToFColor(true);
						Indices.Add(Indices.Num());
					}
				}
				++FaceIndex;
			}
		}

		void QueueMaterial(
			FCanvasRenderContext& RenderContext,
			FMeshPassProcessorRenderState& DrawRenderState,
			const FSceneView& View)
		{
			if (!bMeshElementInitialized)
			{
				FDynamicMeshBuilder DynamicMeshBuilder(View.GetFeatureLevel(), MAX_STATIC_TEXCOORDS, MeshSettings.LightMapIndex);
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
					&MaterialRenderProxy,
					SDPG_Foreground,
					true,
					0,
					MeshBuilderResources,
					MeshElement);
				check(MeshBuilderResources.IsValidForRendering());
				bMeshElementInitialized = true;
			}

			MeshElement.MaterialRenderProxy = &MaterialRenderProxy;
			LCI->CreatePrecomputedLightingUniformBuffer_RenderingThread(View.GetFeatureLevel());
			MeshElement.LCI = LCI;
			GetRendererModule().DrawTileMesh(
				RenderContext,
				DrawRenderState,
				View,
				MeshElement,
				false,
				FHitProxyId());
		}

		const FMeshData& MeshSettings;
		FIntPoint TextureSize;
		FMaterialRenderProxy& MaterialRenderProxy;
		TArray<int32> RasterSourceTriangleIndices;
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

	bool BakeMaskedOutput(
		UMaterialInterface& MaterialInterface,
		const FMeshData& MeshSettings,
		const FIntPoint& TextureSize,
		const FColor& BackgroundColor,
		const EFoliageBakerMaskedOutput Output,
		const TArray<int32>* RasterSourceTriangleIndices,
		TArray<FColor>& OutPixels,
		FString* OutError)
	{
		OutPixels.Reset();
		if (OutError)
		{
			OutError->Reset();
		}
		const TCHAR* OutputName = GetMaskedOutputName(Output);

		if (!IsInGameThread())
		{
			const FString Message = FString::Printf(TEXT("%s must be baked on the game thread."), OutputName);
			SetMaskedBakeError(OutError, *Message);
			return false;
		}
		if (!MeshSettings.MeshDescription || TextureSize.X <= 0 || TextureSize.Y <= 0)
		{
			const FString Message = FString::Printf(TEXT("%s received invalid mesh data or texture size."), OutputName);
			SetMaskedBakeError(OutError, *Message);
			return false;
		}
		if (Output == EFoliageBakerMaskedOutput::SourceTriangleId)
		{
			if (!RasterSourceTriangleIndices
				|| RasterSourceTriangleIndices->Num() != MeshSettings.MeshDescription->Triangles().Num())
			{
				SetMaskedBakeError(
					OutError,
					TEXT("SourceTriangleId requires one source index for every raster mesh triangle."));
				return false;
			}
			for (const int32 SourceTriangleIndex : *RasterSourceTriangleIndices)
			{
				if (SourceTriangleIndex < 0 || SourceTriangleIndex > 0xFFFFFE)
				{
					SetMaskedBakeError(
						OutError,
						TEXT("SourceTriangleId exceeds the supported 24-bit one-based range."));
					return false;
				}
			}
		}

		TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget(
			NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
		if (!RenderTarget.IsValid())
		{
			const FString Message = FString::Printf(TEXT("%s could not allocate a render target."), OutputName);
			SetMaskedBakeError(OutError, *Message);
			return false;
		}

		// MP_BaseColor is baked to an sRGB render target by Engine MaterialBaking.
		// Match that storage convention so the returned FColor can be copied into
		// the sRGB atlas without an extra decode/encode or 8-bit linear quantization.
		const bool bSrgbOutput = Output == EFoliageBakerMaskedOutput::BaseColor;
		RenderTarget->ClearColor = bSrgbOutput
			? FLinearColor(BackgroundColor)
			: BackgroundColor.ReinterpretAsLinear();
		RenderTarget->TargetGamma = 0.0f;
		RenderTarget->InitCustomFormat(TextureSize.X, TextureSize.Y, PF_B8G8R8A8, !bSrgbOutput);
		RenderTarget->UpdateResourceImmediate(true);

		FFoliageBakerMaskedMaterialProxy* MaterialProxy =
			new FFoliageBakerMaskedMaterialProxy(MaterialInterface, Output);
		MaterialProxy->FinishCompilation();
		if (!MaterialProxy->DidCacheShadersSucceed() || !MaterialProxy->GetGameThreadShaderMap())
		{
			const bool bCacheShadersSucceeded = MaterialProxy->DidCacheShadersSucceed();
			const bool bGameThreadShaderMapValid = MaterialProxy->GetGameThreadShaderMap() != nullptr;
			FString CompileErrorDetails;
#if WITH_EDITOR
			const TArray<FString>& CompileErrors = MaterialProxy->GetCompileErrors();
			if (!CompileErrors.IsEmpty())
			{
				CompileErrorDetails = FString::Printf(
					TEXT(" Details: %s"),
					*FString::Join(CompileErrors, TEXT(" | ")));
			}
#endif

			// CacheShaders enqueues SetGameThreadShaderMap commands that hold an
			// intrusive reference to this FMaterial even when compilation fails.
			FlushRenderingCommands();
			FMaterial::DeferredDelete(MaterialProxy);
			const FString CompileFailureMessage = FString::Printf(
				TEXT("%s material shader compilation failed (CacheShaders=%s, GameThreadShaderMap=%s).%s"),
				OutputName,
				bCacheShadersSucceeded ? TEXT("true") : TEXT("false"),
				bGameThreadShaderMapValid ? TEXT("valid") : TEXT("null"),
				*CompileErrorDetails);
			SetMaskedBakeError(OutError, *CompileFailureMessage);
			return false;
		}

		TUniquePtr<FFoliageBakerMaskedMaterialRenderItem> RenderItem =
			MakeUnique<FFoliageBakerMaskedMaterialRenderItem>(
				TextureSize,
				MeshSettings,
				*MaterialProxy,
				RasterSourceTriangleIndices);
		FFoliageBakerMaskedMaterialRenderItem* RenderItemPtr = RenderItem.Get();
		UTextureRenderTarget2D* RenderTargetPtr = RenderTarget.Get();
		const IConsoleVariable* VTWarmupFramesVariable =
			IConsoleManager::Get().FindConsoleVariable(TEXT("MaterialBaking.VTWarmupFrames"));
		const int32 RequestedVTWarmupFrames = FMath::Max(
			1,
			VTWarmupFramesVariable ? VTWarmupFramesVariable->GetInt() : 5);

		ENQUEUE_RENDER_COMMAND(RenderFoliageBakerMaskedMaterial)(
			[RenderItemPtr, RenderTargetPtr, MaterialProxy, RequestedVTWarmupFrames](FRHICommandListImmediate& RHICmdList)
			{
				FSceneViewFamily ViewFamily(
					FSceneViewFamily::ConstructionValues(
						RenderTargetPtr->GetRenderTargetResource(),
						nullptr,
						FEngineShowFlags(ESFIM_Game))
					.SetTime(FGameTime()));

				RenderItemPtr->ViewFamily = &ViewFamily;
				FTextureRenderTargetResource* RenderTargetResource =
					RenderTargetPtr->GetRenderTargetResource();
				FCanvas Canvas(
					RenderTargetResource,
					nullptr,
					FGameTime::GetTimeSinceAppStart(),
					GMaxRHIFeatureLevel);
				Canvas.SetAllowedModes(FCanvas::Allow_Flush);
				Canvas.SetRenderTargetRect(FIntRect(
					0,
					0,
					RenderTargetPtr->SizeX,
					RenderTargetPtr->SizeY));
				Canvas.SetBaseTransform(Canvas.CalcBaseTransform2D(
					RenderTargetPtr->SizeX,
					RenderTargetPtr->SizeY));

				int32 WarmupIterationCount = 1;
				if (UseVirtualTexturing(ViewFamily.GetShaderPlatform()))
				{
					const FMaterial& MeshMaterial = MaterialProxy->GetIncompleteMaterialWithFallback(
						ViewFamily.GetFeatureLevel());
					if (!MeshMaterial.GetUniformVirtualTextureExpressions().IsEmpty())
					{
						WarmupIterationCount = RequestedVTWarmupFrames;
					}
				}

				for (int32 WarmupIndex = 0; WarmupIndex < WarmupIterationCount; ++WarmupIndex)
				{
					FCanvas::FCanvasSortElement& SortElement =
						Canvas.GetSortElement(Canvas.TopDepthSortKey());
					SortElement.RenderBatchArray.Add(RenderItemPtr);
					Canvas.Flush_RenderThread(RHICmdList);
					SortElement.RenderBatchArray.Empty();
					RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
				}
			});

		FlushRenderingCommands();

		TArray<FColor> RawPixels;
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		const bool bReadSucceeded = RenderTarget->GameThread_GetRenderTargetResource()->ReadPixels(
			RawPixels,
			ReadFlags);
		const int64 ExpectedPixelCount = static_cast<int64>(TextureSize.X) * static_cast<int64>(TextureSize.Y);
		const bool bOutputSizeValid = ExpectedPixelCount > 0
			&& ExpectedPixelCount <= MAX_int32
			&& RawPixels.Num() == static_cast<int32>(ExpectedPixelCount);

		RenderItem.Reset();
		FMaterial::DeferredDelete(MaterialProxy);

		if (!bReadSucceeded || !bOutputSizeValid)
		{
			const FString Message = FString::Printf(
				TEXT("%s GPU readback failed or returned an invalid size."),
				OutputName);
			SetMaskedBakeError(OutError, *Message);
			return false;
		}

		if (Output != EFoliageBakerMaskedOutput::FinalCoverage)
		{
			OutPixels = MoveTemp(RawPixels);
			return true;
		}

		OutPixels.SetNumUninitialized(RawPixels.Num());
		for (int32 PixelIndex = 0; PixelIndex < RawPixels.Num(); ++PixelIndex)
		{
			OutPixels[PixelIndex] = RawPixels[PixelIndex] == BackgroundColor
				? BackgroundColor
				: FColor::White;
		}
		return true;
	}
}

bool FFoliageBakerMaskedMaterialBaker::BakeBaseColor(
	UMaterialInterface& MaterialInterface,
	const FMeshData& MeshSettings,
	const FIntPoint& TextureSize,
	const FColor& BackgroundColor,
	TArray<FColor>& OutBaseColor,
	FString* OutError)
{
	return BakeMaskedOutput(
		MaterialInterface,
		MeshSettings,
		TextureSize,
		BackgroundColor,
		EFoliageBakerMaskedOutput::BaseColor,
		nullptr,
		OutBaseColor,
		OutError);
}

bool FFoliageBakerMaskedMaterialBaker::BakeFinalCoverage(
	UMaterialInterface& MaterialInterface,
	const FMeshData& MeshSettings,
	const FIntPoint& TextureSize,
	const FColor& BackgroundColor,
	TArray<FColor>& OutCoverage,
	FString* OutError)
{
	return BakeMaskedOutput(
		MaterialInterface,
		MeshSettings,
		TextureSize,
		BackgroundColor,
		EFoliageBakerMaskedOutput::FinalCoverage,
		nullptr,
		OutCoverage,
		OutError);
}

bool FFoliageBakerMaskedMaterialBaker::BakeObjectSpaceNormal(
	UMaterialInterface& MaterialInterface,
	const FMeshData& MeshSettings,
	const FIntPoint& TextureSize,
	const FColor& BackgroundColor,
	TArray<FColor>& OutNormals,
	FString* OutError)
{
	return BakeMaskedOutput(
		MaterialInterface,
		MeshSettings,
		TextureSize,
		BackgroundColor,
		EFoliageBakerMaskedOutput::ObjectSpaceNormal,
		nullptr,
		OutNormals,
		OutError);
}

bool FFoliageBakerMaskedMaterialBaker::BakeSourceTriangleId(
	UMaterialInterface& MaterialInterface,
	const FMeshData& MeshSettings,
	const TArray<int32>& RasterSourceTriangleIndices,
	const FIntPoint& TextureSize,
	TArray<FColor>& OutTriangleIds,
	FString* OutError)
{
	return BakeMaskedOutput(
		MaterialInterface,
		MeshSettings,
		TextureSize,
		FColor::Black,
		EFoliageBakerMaskedOutput::SourceTriangleId,
		&RasterSourceTriangleIndices,
		OutTriangleIds,
		OutError);
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
