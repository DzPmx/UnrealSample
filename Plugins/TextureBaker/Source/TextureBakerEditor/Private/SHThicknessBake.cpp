#include "SHThicknessBakeCore.h"

#include "Algo/Sort.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "GlobalShader.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Image/ImageBuilder.h"
#include "Misc/App.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIFeatureLevel.h"
#include "RHIGlobals.h"
#include "RHIGPUReadback.h"
#include "Sampling/MeshBakerCommon.h"
#include "Sampling/MeshMapBaker.h"
#include "Sampling/MeshMapEvaluator.h"
#include "ShaderParameterStruct.h"

#include <cfloat>

class FSHThicknessBakeCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSHThicknessBakeCS);
	SHADER_USE_PARAMETER_STRUCT(FSHThicknessBakeCS, FGlobalShader);

public:
	static constexpr uint32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SampleCount)
		SHADER_PARAMETER(uint32, DirectionCount)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, CoefficientSpace)
		SHADER_PARAMETER(float, ThicknessScaleCm)
		SHADER_PARAMETER(float, NormalOffsetCm)
		SHADER_PARAMETER(float, NearClipCm)
		SHADER_PARAMETER(float, FarClipCm)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Samples)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<float4>,
			TrianglePositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<float4>,
			TriangleTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<float4>,
			TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<float4>,
			BVHNodes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<uint>,
			BVHTriangleIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(
			StructuredBuffer<float4>,
			Directions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(
			RWStructuredBuffer<float4>,
			OutputCoefficients)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(
			Parameters.Platform,
			ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(
			Parameters,
			OutEnvironment);
		OutEnvironment.SetDefine(
			TEXT("THREADGROUP_SIZE"),
			ThreadGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(
	FSHThicknessBakeCS,
	"/Plugin/TextureBaker/Private/SHThicknessBake.usf",
	"MainCS",
	SF_Compute);

namespace SHThicknessBaker
{
namespace
{

using namespace UE::Geometry;

constexpr float UnityCameraNormalOffsetCm = 1.1f;
constexpr float UnityCameraNearClipCm = 1.0f;
constexpr float UnityCameraFarClipCm = 30000.0f;
constexpr int32 GPUSampleBatchSize = 4096;
constexpr int32 BVHLeafTriangleCount = 4;

struct FSampleKey
{
	int32 TriangleID = INDEX_NONE;
	FVector3d BaryCoords = FVector3d::Zero();

	bool operator==(const FSampleKey& Other) const
	{
		return TriangleID == Other.TriangleID
			&& BaryCoords == Other.BaryCoords;
	}

	friend uint32 GetTypeHash(const FSampleKey& Key)
	{
		uint32 Hash = GetTypeHash(Key.TriangleID);
		Hash = HashCombineFast(Hash, GetTypeHash(Key.BaryCoords.X));
		Hash = HashCombineFast(Hash, GetTypeHash(Key.BaryCoords.Y));
		return HashCombineFast(Hash, GetTypeHash(Key.BaryCoords.Z));
	}
};

struct FGPUBVHNode
{
	FVector3f Min = FVector3f::Zero();
	FVector3f Max = FVector3f::Zero();
	uint32 LeftFirst = 0;
	uint32 TriangleCount = 0;
};

struct FGPUBakeData
{
	TArray<FVector4f> TrianglePositions;
	TArray<FVector4f> TriangleTangents;
	TArray<FVector4f> TriangleNormals;
	TArray<FVector4f> BVHNodes;
	TArray<uint32> BVHTriangleIndices;
	TArray<FVector4f> Directions;
};

struct FGPUDispatchContext
{
	FEvent* CompletionEvent = nullptr;
	TSharedPtr<const FGPUBakeData, ESPMode::ThreadSafe> CommonData;
	TArray<FVector4f> Samples;
	TArray<FVector4f> Output;
	TUniquePtr<FRHIGPUBufferReadback> Readback;
	uint32 OutputBytes = 0;
	ECoefficientSpace CoefficientSpace =
		ECoefficientSpace::Tangent;
	float ThicknessScaleCm = 0.0f;
	bool bReadbackComplete = false;
	FString Error;
};

TArray<FVector4f> BuildDirectionSet(const int32 DirectionCount)
{
	const int32 HalfCount = DirectionCount / 2;
	TArray<FVector4f> Result;
	Result.Reserve(DirectionCount);
	const double GoldenAngle =
		UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));

	for (int32 Index = 0; Index < HalfCount; ++Index)
	{
		const double Z =
			(static_cast<double>(Index) + 0.5) / HalfCount;
		const double Radius =
			FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		const double Phi = GoldenAngle * Index;
		const FVector3d Direction(
			Radius * FMath::Cos(Phi),
			Radius * FMath::Sin(Phi),
			Z);

		for (const double Sign : { 1.0, -1.0 })
		{
			const FVector3d SignedDirection = Direction * Sign;
			Result.Emplace(
				static_cast<float>(SignedDirection.X),
				static_cast<float>(SignedDirection.Y),
				static_cast<float>(SignedDirection.Z),
				0.0f);
		}
	}
	return Result;
}

void ConfigureMapBaker(
	FMeshMapBaker& Baker,
	FDynamicMesh3& Mesh,
	FMeshBakerDynamicMeshSampler& DetailSampler,
	const FBakeSettings& Settings)
{
	Baker.SetTargetMesh(&Mesh);
	Baker.SetTargetMeshUVLayer(Settings.BakeUVChannel);
	Baker.SetDetailSampler(&DetailSampler);
	Baker.SetCorrespondenceStrategy(
		FMeshBaseBaker::ECorrespondenceStrategy::Identity);
	const int32 TextureResolution =
		GetTextureResolution(Settings.TextureResolution);
	Baker.SetDimensions(FImageDimensions(
		TextureResolution,
		TextureResolution));
	Baker.SetSamplesPerPixel(Settings.SamplesPerPixel);
	Baker.SetFilter(FMeshMapBaker::EBakeFilterType::BSpline);
	Baker.SetGutterEnabled(Settings.PaddingSize > 0);
	Baker.SetGutterSize(Settings.PaddingSize);
}

class FGPUCollectEvaluator final : public FMeshMapEvaluator
{
public:
	FGPUCollectEvaluator(
		std::atomic<bool>& InCancelRequested,
		std::atomic<int64>& InProcessedSurfaceSamples)
		: CancelRequested(InCancelRequested)
		, ProcessedSurfaceSamples(InProcessedSurfaceSamples)
	{
	}

	virtual void Setup(
		const FMeshBaseBaker& Baker,
		FEvaluationContext& Context) override
	{
		Context.Evaluate = &EvaluateSample;
		Context.EvaluateDefault = &EvaluateDefault;
		Context.EvaluateColor = &EvaluateColor;
		Context.EvalData = this;
		Context.AccumulateMode = EAccumulateMode::Add;
		Context.DataLayout = DataLayout();
	}

	virtual const TArray<EComponents>& DataLayout() const override
	{
		static const TArray<EComponents> Layout{ EComponents::Float4 };
		return Layout;
	}

	virtual EMeshMapEvaluatorType Type() const override
	{
		return EMeshMapEvaluatorType::Property;
	}

	const TArray<FVector4f>& GetSamples() const
	{
		return Samples;
	}

	const TMap<FSampleKey, int32>& GetSampleLookup() const
	{
		return SampleLookup;
	}

private:
	FVector4f CollectSample(const FCorrespondenceSample& Sample)
	{
		if (CancelRequested.load(std::memory_order_relaxed))
		{
			return FVector4f::Zero();
		}

		ProcessedSurfaceSamples.fetch_add(
			1,
			std::memory_order_relaxed);
		const FSampleKey Key{
			Sample.BaseSample.TriangleIndex,
			Sample.BaseSample.BaryCoords
		};

		FScopeLock Lock(&SamplesCriticalSection);
		if (!SampleLookup.Contains(Key))
		{
			const int32 SampleIndex = Samples.Add(FVector4f(
				static_cast<float>(Key.BaryCoords.X),
				static_cast<float>(Key.BaryCoords.Y),
				static_cast<float>(Key.BaryCoords.Z),
				FPlatformMath::AsFloat(
					static_cast<uint32>(Key.TriangleID))));
			SampleLookup.Add(Key, SampleIndex);
		}
		return FVector4f::Zero();
	}

	static void EvaluateSample(
		float*& Out,
		const FCorrespondenceSample& Sample,
		void* EvalData)
	{
		FGPUCollectEvaluator* Evaluator =
			static_cast<FGPUCollectEvaluator*>(EvalData);
		WriteToBuffer(Out, Evaluator->CollectSample(Sample));
	}

	static void EvaluateDefault(float*& Out, void* EvalData)
	{
		WriteToBuffer(Out, FVector4f::Zero());
	}

	static void EvaluateColor(
		const int DataIndex,
		float*& In,
		FVector4f& Out,
		void* EvalData)
	{
		Out = FVector4f(In[0], In[1], In[2], In[3]);
		In += 4;
	}

	std::atomic<bool>& CancelRequested;
	std::atomic<int64>& ProcessedSurfaceSamples;
	FCriticalSection SamplesCriticalSection;
	TArray<FVector4f> Samples;
	TMap<FSampleKey, int32> SampleLookup;
};

class FGPUResultEvaluator final : public FMeshMapEvaluator
{
public:
	FGPUResultEvaluator(
		const TMap<FSampleKey, int32>& InSampleLookup,
		const TArray<FVector4f>& InResults,
		std::atomic<int64>& InInvalidSampleCount)
		: SampleLookup(InSampleLookup)
		, Results(InResults)
		, InvalidSampleCount(InInvalidSampleCount)
	{
	}

	virtual void Setup(
		const FMeshBaseBaker& Baker,
		FEvaluationContext& Context) override
	{
		Context.Evaluate = &EvaluateSample;
		Context.EvaluateDefault = &EvaluateDefault;
		Context.EvaluateColor = &EvaluateColor;
		Context.EvalData = this;
		Context.AccumulateMode = EAccumulateMode::Add;
		Context.DataLayout = DataLayout();
	}

	virtual const TArray<EComponents>& DataLayout() const override
	{
		static const TArray<EComponents> Layout{ EComponents::Float4 };
		return Layout;
	}

	virtual EMeshMapEvaluatorType Type() const override
	{
		return EMeshMapEvaluatorType::Property;
	}

private:
	FVector4f FindResult(const FCorrespondenceSample& Sample) const
	{
		const FSampleKey Key{
			Sample.BaseSample.TriangleIndex,
			Sample.BaseSample.BaryCoords
		};
		const int32* SampleIndex = SampleLookup.Find(Key);
		if (SampleIndex == nullptr
			|| !Results.IsValidIndex(*SampleIndex))
		{
			InvalidSampleCount.fetch_add(
				1,
				std::memory_order_relaxed);
			return FVector4f::Zero();
		}
		return Results[*SampleIndex];
	}

	static void EvaluateSample(
		float*& Out,
		const FCorrespondenceSample& Sample,
		void* EvalData)
	{
		const FGPUResultEvaluator* Evaluator =
			static_cast<const FGPUResultEvaluator*>(EvalData);
		WriteToBuffer(Out, Evaluator->FindResult(Sample));
	}

	static void EvaluateDefault(float*& Out, void* EvalData)
	{
		WriteToBuffer(Out, FVector4f::Zero());
	}

	static void EvaluateColor(
		const int DataIndex,
		float*& In,
		FVector4f& Out,
		void* EvalData)
	{
		Out = FVector4f(In[0], In[1], In[2], In[3]);
		In += 4;
	}

	const TMap<FSampleKey, int32>& SampleLookup;
	const TArray<FVector4f>& Results;
	std::atomic<int64>& InvalidSampleCount;
};

bool BuildGPUBakeData(
	const FDynamicMesh3& Mesh,
	const int32 DirectionCount,
	const ECoefficientSpace CoefficientSpace,
	FGPUBakeData& OutData,
	FString& OutError)
{
	if (!Mesh.HasAttributes()
		|| Mesh.Attributes()->PrimaryNormals() == nullptr)
	{
		OutError =
			TEXT("The GPU bake mesh has no normal attributes.");
		return false;
	}
	if (CoefficientSpace == ECoefficientSpace::Tangent
		&& !Mesh.Attributes()->HasTangentSpace())
	{
		OutError =
			TEXT("The GPU bake mesh has no tangent-space attributes.");
		return false;
	}

	const int32 MaxTriangleID = Mesh.MaxTriangleID();
	if (MaxTriangleID <= 0
		|| MaxTriangleID > MAX_int32 / 3)
	{
		OutError =
			TEXT("The GPU bake mesh triangle index range is unsupported.");
		return false;
	}

	const FDynamicMeshNormalOverlay* Normals =
		Mesh.Attributes()->PrimaryNormals();
	const FDynamicMeshNormalOverlay* Tangents =
		Mesh.Attributes()->PrimaryTangents();
	const FDynamicMeshNormalOverlay* Bitangents =
		Mesh.Attributes()->PrimaryBiTangents();
	check(Normals);
	check(
		CoefficientSpace != ECoefficientSpace::Tangent
			|| (Tangents && Bitangents));

	const int32 TriangleVectorCount = MaxTriangleID * 3;
	OutData.TrianglePositions.SetNumZeroed(TriangleVectorCount);
	OutData.TriangleTangents.SetNumZeroed(TriangleVectorCount);
	OutData.TriangleNormals.SetNumZeroed(TriangleVectorCount);

	TArray<FVector3f> TriangleBoundsMin;
	TArray<FVector3f> TriangleBoundsMax;
	TArray<FVector3f> TriangleCentroids;
	TriangleBoundsMin.SetNumZeroed(MaxTriangleID);
	TriangleBoundsMax.SetNumZeroed(MaxTriangleID);
	TriangleCentroids.SetNumZeroed(MaxTriangleID);

	TArray<int32> SortedTriangleIDs;
	SortedTriangleIDs.Reserve(Mesh.TriangleCount());
	for (const int32 TriangleID : Mesh.TriangleIndicesItr())
	{
		FVector3d PositionA;
		FVector3d PositionB;
		FVector3d PositionC;
		Mesh.GetTriVertices(
			TriangleID,
			PositionA,
			PositionB,
			PositionC);
		const FVector3f A(PositionA);
		const FVector3f B(PositionB);
		const FVector3f C(PositionC);
		OutData.TrianglePositions[TriangleID * 3 + 0] =
			FVector4f(A, 0.0f);
		OutData.TrianglePositions[TriangleID * 3 + 1] =
			FVector4f(B - A, 0.0f);
		OutData.TrianglePositions[TriangleID * 3 + 2] =
			FVector4f(C - A, 0.0f);

		TriangleBoundsMin[TriangleID] = FVector3f(
			FMath::Min3(A.X, B.X, C.X),
			FMath::Min3(A.Y, B.Y, C.Y),
			FMath::Min3(A.Z, B.Z, C.Z));
		TriangleBoundsMax[TriangleID] = FVector3f(
			FMath::Max3(A.X, B.X, C.X),
			FMath::Max3(A.Y, B.Y, C.Y),
			FMath::Max3(A.Z, B.Z, C.Z));
		TriangleCentroids[TriangleID] = (A + B + C) / 3.0f;

		for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
		{
			FVector3f Normal;
			Normals->GetTriElement(
				TriangleID,
				CornerIndex,
				Normal);
			Normal.Normalize();
			OutData.TriangleNormals[
				TriangleID * 3 + CornerIndex] =
				FVector4f(Normal, 0.0f);
			if (CoefficientSpace == ECoefficientSpace::Tangent)
			{
				FVector3f Tangent;
				FVector3f ReferenceBitangent;
				Tangents->GetTriElement(
					TriangleID,
					CornerIndex,
					Tangent);
				Bitangents->GetTriElement(
					TriangleID,
					CornerIndex,
					ReferenceBitangent);
				Tangent.Normalize();
				ReferenceBitangent.Normalize();
				const float BinormalSign =
					FVector3f::DotProduct(
						Tangent,
						FVector3f::CrossProduct(
							ReferenceBitangent,
							Normal)) < 0.0f
						? -1.0f
						: 1.0f;
				const FVector3f ShaderBitangent =
					FVector3f::CrossProduct(Normal, Tangent)
						* BinormalSign;
				const FVector3f ShaderTangent =
					FVector3f::CrossProduct(
						ShaderBitangent,
						Normal)
						* BinormalSign;

				OutData.TriangleTangents[
					TriangleID * 3 + CornerIndex] =
						FVector4f(
							ShaderTangent,
							BinormalSign);
			}
		}
		SortedTriangleIDs.Add(TriangleID);
	}

	if (SortedTriangleIDs.IsEmpty())
	{
		OutError = TEXT("The GPU bake mesh has no triangles.");
		return false;
	}

	TArray<FGPUBVHNode> Nodes;
	Nodes.AddDefaulted();
	TFunction<void(int32, int32, int32)> BuildNode =
		[&](
			const int32 NodeIndex,
			const int32 FirstTriangle,
			const int32 TriangleCount)
		{
			FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			FVector3f CentroidMin(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector3f CentroidMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			for (int32 Index = 0; Index < TriangleCount; ++Index)
			{
				const int32 TriangleID =
					SortedTriangleIDs[FirstTriangle + Index];
				const FVector3f& TriangleMin =
					TriangleBoundsMin[TriangleID];
				const FVector3f& TriangleMax =
					TriangleBoundsMax[TriangleID];
				const FVector3f& Centroid =
					TriangleCentroids[TriangleID];
				BoundsMin.X = FMath::Min(BoundsMin.X, TriangleMin.X);
				BoundsMin.Y = FMath::Min(BoundsMin.Y, TriangleMin.Y);
				BoundsMin.Z = FMath::Min(BoundsMin.Z, TriangleMin.Z);
				BoundsMax.X = FMath::Max(BoundsMax.X, TriangleMax.X);
				BoundsMax.Y = FMath::Max(BoundsMax.Y, TriangleMax.Y);
				BoundsMax.Z = FMath::Max(BoundsMax.Z, TriangleMax.Z);
				CentroidMin.X = FMath::Min(CentroidMin.X, Centroid.X);
				CentroidMin.Y = FMath::Min(CentroidMin.Y, Centroid.Y);
				CentroidMin.Z = FMath::Min(CentroidMin.Z, Centroid.Z);
				CentroidMax.X = FMath::Max(CentroidMax.X, Centroid.X);
				CentroidMax.Y = FMath::Max(CentroidMax.Y, Centroid.Y);
				CentroidMax.Z = FMath::Max(CentroidMax.Z, Centroid.Z);
			}

			Nodes[NodeIndex].Min = BoundsMin;
			Nodes[NodeIndex].Max = BoundsMax;
			if (TriangleCount <= BVHLeafTriangleCount)
			{
				Nodes[NodeIndex].LeftFirst =
					static_cast<uint32>(FirstTriangle);
				Nodes[NodeIndex].TriangleCount =
					static_cast<uint32>(TriangleCount);
				return;
			}

			const FVector3f CentroidExtent =
				CentroidMax - CentroidMin;
			int32 SplitAxis = 0;
			if (CentroidExtent.Y > CentroidExtent.X)
			{
				SplitAxis = 1;
			}
			if (CentroidExtent.Z > CentroidExtent[SplitAxis])
			{
				SplitAxis = 2;
			}
			TArrayView<int32> TriangleRange(
				SortedTriangleIDs.GetData() + FirstTriangle,
				TriangleCount);
			TriangleRange.Sort(
				[&TriangleCentroids, SplitAxis](
					const int32 Left,
					const int32 Right)
				{
					return TriangleCentroids[Left][SplitAxis]
						< TriangleCentroids[Right][SplitAxis];
				});

			const int32 LeftCount = TriangleCount / 2;
			const int32 LeftChildIndex = Nodes.AddDefaulted();
			const int32 RightChildIndex = Nodes.AddDefaulted();
			check(RightChildIndex == LeftChildIndex + 1);
			Nodes[NodeIndex].LeftFirst =
				static_cast<uint32>(LeftChildIndex);
			Nodes[NodeIndex].TriangleCount = 0;
			BuildNode(
				LeftChildIndex,
				FirstTriangle,
				LeftCount);
			BuildNode(
				RightChildIndex,
				FirstTriangle + LeftCount,
				TriangleCount - LeftCount);
		};
	BuildNode(0, 0, SortedTriangleIDs.Num());

	OutData.BVHNodes.Reserve(Nodes.Num() * 2);
	for (const FGPUBVHNode& Node : Nodes)
	{
		OutData.BVHNodes.Emplace(
			Node.Min.X,
			Node.Min.Y,
			Node.Min.Z,
			FPlatformMath::AsFloat(Node.LeftFirst));
		OutData.BVHNodes.Emplace(
			Node.Max.X,
			Node.Max.Y,
			Node.Max.Z,
			FPlatformMath::AsFloat(Node.TriangleCount));
	}

	OutData.BVHTriangleIndices.Reserve(
		SortedTriangleIDs.Num());
	for (const int32 TriangleID : SortedTriangleIDs)
	{
		OutData.BVHTriangleIndices.Add(
			static_cast<uint32>(TriangleID));
	}
	OutData.Directions = BuildDirectionSet(DirectionCount);
	return true;
}

FRDGBufferRef CreateFloat4UploadBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const TArray<FVector4f>& Data)
{
	check(!Data.IsEmpty());
	return CreateStructuredBuffer(
		GraphBuilder,
		Name,
		sizeof(FVector4f),
		static_cast<uint32>(Data.Num()),
		Data.GetData(),
		static_cast<uint64>(Data.Num()) * sizeof(FVector4f));
}

bool DispatchGPUComputeBatch(
	const TSharedPtr<const FGPUBakeData, ESPMode::ThreadSafe>& CommonData,
	const TConstArrayView<FVector4f> Samples,
	const float ThicknessScaleCm,
	const ECoefficientSpace CoefficientSpace,
	TArray<FVector4f>& OutCoefficients,
	FString& OutError)
{
	if (!CommonData.IsValid() || Samples.IsEmpty())
	{
		OutError = TEXT("The GPU bake batch is empty or invalid.");
		return false;
	}
	if (!FApp::CanEverRender() || GUsingNullRHI)
	{
		OutError =
			TEXT("GPU Bake requires a rendering RHI; NullRHI is active.");
		return false;
	}

	const TSharedRef<FGPUDispatchContext, ESPMode::ThreadSafe> Context =
		MakeShared<FGPUDispatchContext, ESPMode::ThreadSafe>();
	Context->CompletionEvent =
		FPlatformProcess::GetSynchEventFromPool(false);
	Context->CommonData = CommonData;
	Context->Samples.Append(Samples.GetData(), Samples.Num());
	Context->OutputBytes =
		static_cast<uint32>(Samples.Num()) * sizeof(FVector4f);
	Context->CoefficientSpace = CoefficientSpace;
	Context->ThicknessScaleCm = ThicknessScaleCm;

	ENQUEUE_RENDER_COMMAND(TextureBakerSHThicknessGPU)(
		[Context](FRHICommandListImmediate& RHICmdList)
		{
			ON_SCOPE_EXIT
			{
				Context->CompletionEvent->Trigger();
			};

			if (GUsingNullRHI
				|| GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
			{
				Context->Error =
					TEXT("GPU Bake requires an SM5-or-newer rendering RHI.");
				return;
			}

			const uint32 SampleCount =
				static_cast<uint32>(Context->Samples.Num());
			const uint32 MaxGroupsX = static_cast<uint32>(
				GRHIMaxDispatchThreadGroupsPerDimension.X);
			const uint32 DispatchGroupsX =
				FMath::Min(SampleCount, FMath::Min(1024u, MaxGroupsX));
			const uint32 DispatchGroupsY =
				FMath::DivideAndRoundUp(
					SampleCount,
					DispatchGroupsX);
			if (DispatchGroupsY
				> static_cast<uint32>(
					GRHIMaxDispatchThreadGroupsPerDimension.Y))
			{
				Context->Error =
					TEXT("The GPU bake batch exceeds the RHI dispatch dimensions.");
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			const FRDGBufferRef SamplesBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.Samples"),
					Context->Samples);
			const FRDGBufferRef TrianglePositionsBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TrianglePositions"),
					Context->CommonData->TrianglePositions);
			const FRDGBufferRef TriangleTangentsBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TriangleTangents"),
					Context->CommonData->TriangleTangents);
			const FRDGBufferRef TriangleNormalsBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TriangleNormals"),
					Context->CommonData->TriangleNormals);
			const FRDGBufferRef BVHNodesBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.BVHNodes"),
					Context->CommonData->BVHNodes);
			const FRDGBufferRef BVHTriangleIndicesBuffer =
				CreateStructuredBuffer(
					GraphBuilder,
					TEXT("TextureBaker.BVHTriangleIndices"),
					sizeof(uint32),
					static_cast<uint32>(
						Context->CommonData
							->BVHTriangleIndices.Num()),
					Context->CommonData
						->BVHTriangleIndices.GetData(),
					static_cast<uint64>(
						Context->CommonData
							->BVHTriangleIndices.Num())
						* sizeof(uint32));
			const FRDGBufferRef DirectionsBuffer =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.Directions"),
					Context->CommonData->Directions);
			const FRDGBufferRef OutputBuffer =
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(
						sizeof(FVector4f),
						SampleCount),
					TEXT("TextureBaker.OutputCoefficients"));

			FSHThicknessBakeCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<
					FSHThicknessBakeCS::FParameters>();
			Parameters->SampleCount = SampleCount;
			Parameters->DirectionCount = static_cast<uint32>(
				Context->CommonData->Directions.Num());
			Parameters->DispatchGroupsX = DispatchGroupsX;
			Parameters->CoefficientSpace = static_cast<uint32>(
				Context->CoefficientSpace);
			Parameters->ThicknessScaleCm =
				Context->ThicknessScaleCm;
			Parameters->NormalOffsetCm =
				UnityCameraNormalOffsetCm;
			Parameters->NearClipCm = UnityCameraNearClipCm;
			Parameters->FarClipCm = UnityCameraFarClipCm;
			Parameters->Samples =
				GraphBuilder.CreateSRV(SamplesBuffer);
			Parameters->TrianglePositions =
				GraphBuilder.CreateSRV(TrianglePositionsBuffer);
			Parameters->TriangleTangents =
				GraphBuilder.CreateSRV(TriangleTangentsBuffer);
			Parameters->TriangleNormals =
				GraphBuilder.CreateSRV(TriangleNormalsBuffer);
			Parameters->BVHNodes =
				GraphBuilder.CreateSRV(BVHNodesBuffer);
			Parameters->BVHTriangleIndices =
				GraphBuilder.CreateSRV(
					BVHTriangleIndicesBuffer);
			Parameters->Directions =
				GraphBuilder.CreateSRV(DirectionsBuffer);
			Parameters->OutputCoefficients =
				GraphBuilder.CreateUAV(OutputBuffer);

			const TShaderMapRef<FSHThicknessBakeCS> ComputeShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME(
					"TextureBaker SH Thickness (%u samples)",
					SampleCount),
				ComputeShader,
				Parameters,
				FIntVector(
					static_cast<int32>(DispatchGroupsX),
					static_cast<int32>(DispatchGroupsY),
					1));

			Context->Readback =
				MakeUnique<FRHIGPUBufferReadback>(
					TEXT("TextureBaker.SHThicknessReadback"));
			AddEnqueueCopyPass(
				GraphBuilder,
				Context->Readback.Get(),
				OutputBuffer,
				Context->OutputBytes);
			GraphBuilder.Execute();
		});

	while (!Context->CompletionEvent->Wait(50))
	{
	}
	while (Context->Error.IsEmpty()
		&& !Context->bReadbackComplete)
	{
		ENQUEUE_RENDER_COMMAND(TextureBakerPollSHThicknessGPU)(
			[Context](FRHICommandListImmediate& RHICmdList)
			{
				ON_SCOPE_EXIT
				{
					Context->CompletionEvent->Trigger();
				};

				if (!Context->Readback)
				{
					Context->Error =
						TEXT("The GPU coefficient readback was not created.");
					return;
				}
				if (!Context->Readback->IsReady(
					RHICmdList.GetGPUMask()))
				{
					return;
				}

				const FVector4f* ReadbackData =
					static_cast<const FVector4f*>(
						Context->Readback->Lock(
							Context->OutputBytes));
				if (ReadbackData == nullptr)
				{
					Context->Error =
						TEXT("Failed to lock the GPU coefficient readback.");
					Context->Readback.Reset();
					return;
				}
				Context->Output.Append(
					ReadbackData,
					Context->Samples.Num());
				Context->Readback->Unlock();
				Context->Readback.Reset();
				Context->bReadbackComplete = true;
			});

		while (!Context->CompletionEvent->Wait(50))
		{
		}
		if (!Context->bReadbackComplete
			&& Context->Error.IsEmpty())
		{
			FPlatformProcess::SleepNoStats(0.005f);
		}
	}
	FPlatformProcess::ReturnSynchEventToPool(
		Context->CompletionEvent);
	Context->CompletionEvent = nullptr;

	if (!Context->Error.IsEmpty())
	{
		OutError = MoveTemp(Context->Error);
		return false;
	}
	if (Context->Output.Num() != Samples.Num())
	{
		OutError =
			TEXT("The GPU coefficient readback has an unexpected size.");
		return false;
	}
	for (const FVector4f& Coefficient : Context->Output)
	{
		if (!FMath::IsFinite(Coefficient.X)
			|| !FMath::IsFinite(Coefficient.Y)
			|| !FMath::IsFinite(Coefficient.Z)
			|| !FMath::IsFinite(Coefficient.W))
		{
			OutError =
				TEXT("The GPU coefficient readback contains a non-finite value.");
			return false;
		}
	}
	OutCoefficients = MoveTemp(Context->Output);
	return true;
}

void RunGPUBake(FBakeJob& Job)
{
	if (Job.bCancelRequested.load(std::memory_order_relaxed))
	{
		Job.Stage.store(EJobStage::Cancelled);
		return;
	}
	if (!FApp::CanEverRender() || GUsingNullRHI)
	{
		Job.Error =
			TEXT("GPU Bake requires an active rendering RHI.");
		Job.Stage.store(EJobStage::Failed);
		return;
	}

	check(Job.Preparation.CombinedDynamicMesh);
	check(!Job.Preparation.Targets.IsEmpty());
	FDynamicMesh3& RayMesh =
		*Job.Preparation.CombinedDynamicMesh;
	const TSharedRef<FGPUBakeData, ESPMode::ThreadSafe> GPUData =
		MakeShared<FGPUBakeData, ESPMode::ThreadSafe>();
	if (!BuildGPUBakeData(
			RayMesh,
			Job.Preparation.Settings.DirectionCount,
			Job.Preparation.Settings.CoefficientSpace,
			*GPUData,
			Job.Error))
	{
		Job.Stage.store(EJobStage::Failed);
		return;
	}

	TArray<TArray64<FVector4f>> CoefficientImages;
	CoefficientImages.Reserve(Job.Preparation.Targets.Num());

	for (const FBakeTargetPreparation& Target :
		Job.Preparation.Targets)
	{
		check(Target.DynamicMesh);
		FDynamicMesh3& SurfaceMesh = *Target.DynamicMesh;
		const FDynamicMeshAABBTree3 SurfaceSpatial(
			&SurfaceMesh,
			true);
		FMeshBakerDynamicMeshSampler DetailSampler(
			&SurfaceMesh,
			&SurfaceSpatial);

		Job.ProcessedSurfaceSamples.store(0);
		Job.Stage.store(EJobStage::CollectingSamples);
		FMeshMapBaker CollectBaker;
		ConfigureMapBaker(
			CollectBaker,
			SurfaceMesh,
			DetailSampler,
			Job.Preparation.Settings);
		CollectBaker.CancelF = [&Job]()
		{
			return Job.bCancelRequested.load(
				std::memory_order_relaxed);
		};
		const TSharedPtr<FGPUCollectEvaluator, ESPMode::ThreadSafe>
			Collector =
				MakeShared<FGPUCollectEvaluator, ESPMode::ThreadSafe>(
					Job.bCancelRequested,
					Job.ProcessedSurfaceSamples);
		CollectBaker.AddEvaluator(Collector);
		CollectBaker.Bake();

		if (Job.bCancelRequested.load(std::memory_order_relaxed))
		{
			Job.Stage.store(EJobStage::Cancelled);
			return;
		}

		const TArray<FVector4f>& SurfaceSamples =
			Collector->GetSamples();
		TArray<FVector4f> GPUSamples = SurfaceSamples;
		for (FVector4f& Sample : GPUSamples)
		{
			const int32 SurfaceTriangleID =
				static_cast<int32>(FPlatformMath::AsUInt(Sample.W));
			if (!Target.CombinedTriangleIDs.IsValidIndex(
					SurfaceTriangleID)
				|| Target.CombinedTriangleIDs[SurfaceTriangleID]
					== INDEX_NONE)
			{
				Job.Error = FString::Printf(
					TEXT("GPU triangle mapping is invalid for %s."),
					*GetNameSafe(Target.SourceMesh.Get()));
				Job.Stage.store(EJobStage::Failed);
				return;
			}
			Sample.W = FPlatformMath::AsFloat(
				static_cast<uint32>(
					Target.CombinedTriangleIDs[
						SurfaceTriangleID]));
		}

		TArray<FVector4f> GPUResults;
		GPUResults.SetNumUninitialized(GPUSamples.Num());
		Job.TotalSurfaceSamples.store(GPUSamples.Num());
		Job.ProcessedSurfaceSamples.store(0);

		Job.Stage.store(EJobStage::GPUComputing);
		for (int32 FirstSample = 0;
			FirstSample < GPUSamples.Num();
			FirstSample += GPUSampleBatchSize)
		{
			if (Job.bCancelRequested.load(
				std::memory_order_relaxed))
			{
				Job.Stage.store(EJobStage::Cancelled);
				return;
			}

			const int32 BatchCount = FMath::Min(
				GPUSampleBatchSize,
				GPUSamples.Num() - FirstSample);
			TArray<FVector4f> BatchResults;
			if (!DispatchGPUComputeBatch(
				GPUData,
				MakeArrayView(
					GPUSamples.GetData() + FirstSample,
					BatchCount),
				static_cast<float>(
					Job.Preparation.ThicknessScaleCm),
				Job.Preparation.Settings.CoefficientSpace,
				BatchResults,
				Job.Error))
			{
				Job.Stage.store(EJobStage::Failed);
				return;
			}
			check(BatchResults.Num() == BatchCount);
			FMemory::Memcpy(
				GPUResults.GetData() + FirstSample,
				BatchResults.GetData(),
				static_cast<SIZE_T>(BatchCount)
					* sizeof(FVector4f));
			Job.ProcessedSurfaceSamples.fetch_add(
				BatchCount,
				std::memory_order_relaxed);
		}

		Job.Stage.store(EJobStage::Filtering);
		std::atomic<int64> InvalidSampleCount{ 0 };
		FMeshMapBaker ResultBaker;
		ConfigureMapBaker(
			ResultBaker,
			SurfaceMesh,
			DetailSampler,
			Job.Preparation.Settings);
		ResultBaker.CancelF = [&Job, &InvalidSampleCount]()
		{
			return Job.bCancelRequested.load(
					std::memory_order_relaxed)
				|| InvalidSampleCount.load(
					std::memory_order_relaxed) > 0;
		};
		const TSharedPtr<FGPUResultEvaluator, ESPMode::ThreadSafe>
			ResultEvaluator =
				MakeShared<FGPUResultEvaluator, ESPMode::ThreadSafe>(
					Collector->GetSampleLookup(),
					GPUResults,
					InvalidSampleCount);
		const int32 EvaluatorIndex =
			ResultBaker.AddEvaluator(ResultEvaluator);
		ResultBaker.Bake();

		if (Job.bCancelRequested.load(std::memory_order_relaxed))
		{
			Job.Stage.store(EJobStage::Cancelled);
			return;
		}
		if (InvalidSampleCount.load(std::memory_order_relaxed) > 0)
		{
			Job.Error = FString::Printf(
				TEXT("GPU sample lookup changed between collection and filtering for %s."),
				*GetNameSafe(Target.SourceMesh.Get()));
			Job.Stage.store(EJobStage::Failed);
			return;
		}

		const TArrayView<TUniquePtr<TImageBuilder<FVector4f>>>
			Results =
				ResultBaker.GetBakeResults(EvaluatorIndex);
		if (Results.Num() != 1 || !Results[0])
		{
			Job.Error = FString::Printf(
				TEXT("FMeshMapBaker returned no GPU coefficient image for %s."),
				*GetNameSafe(Target.SourceMesh.Get()));
			Job.Stage.store(EJobStage::Failed);
			return;
		}

		const TConstArrayView64<FVector4f> ImageBuffer =
			Results[0]->GetImageBuffer();
		TArray64<FVector4f>& CoefficientImage =
			CoefficientImages.AddDefaulted_GetRef();
		CoefficientImage.Append(
			ImageBuffer.GetData(),
			ImageBuffer.Num());
	}

	Job.Stage.store(EJobStage::Encoding);
	if (!EncodeCoefficientImagesRGBA8(
		CoefficientImages,
		Job.Preparation.Settings.bRemapCoefficientRange,
		Job.EncodedRGBA))
	{
		Job.Error =
			TEXT("A filtered GPU coefficient image contains a non-finite value.");
		Job.Stage.store(EJobStage::Failed);
		return;
	}
	if (Job.bCancelRequested.load(std::memory_order_relaxed))
	{
		Job.EncodedRGBA.Reset();
		Job.Stage.store(EJobStage::Cancelled);
		return;
	}
	Job.Stage.store(EJobStage::Succeeded);
}

void RunCPUBake(FBakeJob& Job)
{
	Job.RunCPU();
}

} // namespace

void FBakeJob::Run()
{
	switch (Preparation.Settings.BakeMode)
	{
	case EBakeMode::CPU:
		RunCPUBake(*this);
		break;
	case EBakeMode::GPU:
		RunGPUBake(*this);
		break;
	default:
		checkNoEntry();
		Error = TEXT("Unknown SH thickness bake mode.");
		Stage.store(EJobStage::Failed);
		break;
	}
}

} // namespace SHThicknessBaker
