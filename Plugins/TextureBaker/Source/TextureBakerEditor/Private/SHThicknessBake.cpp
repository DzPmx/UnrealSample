#include "SHThicknessBakeCore.h"

#include "Algo/Sort.h"
#include "Containers/BitArray.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "GlobalShader.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Image/BCSplineFilter.h"
#include "Image/ImageOccupancyMap.h"
#include "Image/ImageTile.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIFeatureLevel.h"
#include "RHIGlobals.h"
#include "RHIGPUReadback.h"
#include "Sampling/MeshMapBaker.h"
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
		SHADER_PARAMETER(float, ThicknessScale)
		SHADER_PARAMETER(float, NormalOffset)
		SHADER_PARAMETER(float, NearClip)
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

constexpr double GPUOperationTimeoutSeconds = 60.0;
constexpr int32 GPUImageTileSize = 32;
constexpr int32 GPUFilterTilePadding = 2;
constexpr float GPUBSplineFilterRadius = 0.769f;
constexpr int32 BVHLeafTriangleCount = 4;

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

struct FGPUSession
{
	TRefCountPtr<FRDGPooledBuffer> TrianglePositions;
	TRefCountPtr<FRDGPooledBuffer> TriangleTangents;
	TRefCountPtr<FRDGPooledBuffer> TriangleNormals;
	TRefCountPtr<FRDGPooledBuffer> BVHNodes;
	TRefCountPtr<FRDGPooledBuffer> BVHTriangleIndices;
	TRefCountPtr<FRDGPooledBuffer> Directions;
	uint32 DirectionCount = 0;

	bool IsValid() const
	{
		return TrianglePositions.IsValid()
			&& TriangleTangents.IsValid()
			&& TriangleNormals.IsValid()
			&& BVHNodes.IsValid()
			&& BVHTriangleIndices.IsValid()
			&& Directions.IsValid()
			&& DirectionCount > 0;
	}
};

struct FGPUUploadContext
{
	~FGPUUploadContext()
	{
		if (CompletionEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		}
	}

	FEvent* CompletionEvent = nullptr;
	TSharedPtr<const FGPUBakeData, ESPMode::ThreadSafe> CommonData;
	TSharedPtr<FGPUSession, ESPMode::ThreadSafe> Session;
	FString Error;
};

struct FGPUDispatchContext
{
	~FGPUDispatchContext()
	{
		if (CompletionEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		}
	}

	FEvent* CompletionEvent = nullptr;
	TSharedPtr<const FGPUSession, ESPMode::ThreadSafe> Session;
	TArray<FVector4f> Samples;
	TArray<FVector4f> Output;
	TUniquePtr<FRHIGPUBufferReadback> Readback;
	uint32 OutputBytes = 0;
	ECoefficientSpace CoefficientSpace =
		ECoefficientSpace::Tangent;
	float ThicknessScale = 0.0f;
	bool bReadbackComplete = false;
	FString Error;
};

struct FGPUTileSample
{
	FVector2d FilterUV = FVector2d::Zero();
	FVector2i ImageCoords = FVector2i::Zero();
	int32 UVChart = INDEX_NONE;
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

enum class EGPUWaitResult : uint8
{
	Completed,
	Cancelled,
	TimedOut
};

EGPUWaitResult WaitForGPUEvent(
	FEvent& Event,
	const std::atomic<bool>& CancelRequested,
	const double DeadlineSeconds)
{
	while (!Event.Wait(50))
	{
		if (CancelRequested.load(std::memory_order_relaxed))
		{
			return EGPUWaitResult::Cancelled;
		}
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			return EGPUWaitResult::TimedOut;
		}
	}
	return EGPUWaitResult::Completed;
}

bool InitializeGPUSession(
	const TSharedPtr<const FGPUBakeData, ESPMode::ThreadSafe>& CommonData,
	const std::atomic<bool>& CancelRequested,
	TSharedPtr<FGPUSession, ESPMode::ThreadSafe>& OutSession,
	FString& OutError)
{
	if (!CommonData.IsValid()
		|| CommonData->TrianglePositions.IsEmpty()
		|| CommonData->TriangleTangents.IsEmpty()
		|| CommonData->TriangleNormals.IsEmpty()
		|| CommonData->BVHNodes.IsEmpty()
		|| CommonData->BVHTriangleIndices.IsEmpty()
		|| CommonData->Directions.IsEmpty())
	{
		OutError = TEXT("The GPU bake session data is empty or invalid.");
		return false;
	}

	const TSharedRef<FGPUUploadContext, ESPMode::ThreadSafe> Context =
		MakeShared<FGPUUploadContext, ESPMode::ThreadSafe>();
	Context->CompletionEvent =
		FPlatformProcess::GetSynchEventFromPool(false);
	Context->CommonData = CommonData;
	Context->Session =
		MakeShared<FGPUSession, ESPMode::ThreadSafe>();
	Context->Session->DirectionCount =
		static_cast<uint32>(CommonData->Directions.Num());

	ENQUEUE_RENDER_COMMAND(TextureBakerUploadSHThicknessGPU)(
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

			FRDGBuilder GraphBuilder(RHICmdList);
			const FRDGBufferRef TrianglePositions =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TrianglePositions"),
					Context->CommonData->TrianglePositions);
			const FRDGBufferRef TriangleTangents =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TriangleTangents"),
					Context->CommonData->TriangleTangents);
			const FRDGBufferRef TriangleNormals =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.TriangleNormals"),
					Context->CommonData->TriangleNormals);
			const FRDGBufferRef BVHNodes =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.BVHNodes"),
					Context->CommonData->BVHNodes);
			const FRDGBufferRef BVHTriangleIndices =
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
			const FRDGBufferRef Directions =
				CreateFloat4UploadBuffer(
					GraphBuilder,
					TEXT("TextureBaker.Directions"),
					Context->CommonData->Directions);

			GraphBuilder.QueueBufferExtraction(
				TrianglePositions,
				&Context->Session->TrianglePositions);
			GraphBuilder.QueueBufferExtraction(
				TriangleTangents,
				&Context->Session->TriangleTangents);
			GraphBuilder.QueueBufferExtraction(
				TriangleNormals,
				&Context->Session->TriangleNormals);
			GraphBuilder.QueueBufferExtraction(
				BVHNodes,
				&Context->Session->BVHNodes);
			GraphBuilder.QueueBufferExtraction(
				BVHTriangleIndices,
				&Context->Session->BVHTriangleIndices);
			GraphBuilder.QueueBufferExtraction(
				Directions,
				&Context->Session->Directions);
			GraphBuilder.Execute();
		});

	const EGPUWaitResult WaitResult = WaitForGPUEvent(
		*Context->CompletionEvent,
		CancelRequested,
		FPlatformTime::Seconds() + GPUOperationTimeoutSeconds);
	if (WaitResult == EGPUWaitResult::Cancelled)
	{
		OutError = TEXT("GPU Bake was cancelled while uploading common data.");
		return false;
	}
	if (WaitResult == EGPUWaitResult::TimedOut)
	{
		OutError =
			TEXT("GPU Bake timed out while uploading common data.");
		return false;
	}
	if (!Context->Error.IsEmpty())
	{
		OutError = MoveTemp(Context->Error);
		return false;
	}
	if (!Context->Session->IsValid())
	{
		OutError =
			TEXT("GPU Bake failed to create persistent common buffers.");
		return false;
	}

	OutSession = Context->Session;
	return true;
}

bool DispatchGPUComputeBatch(
	const TSharedPtr<const FGPUSession, ESPMode::ThreadSafe>& Session,
	const TConstArrayView<FVector4f> Samples,
	const float ThicknessScale,
	const ECoefficientSpace CoefficientSpace,
	const std::atomic<bool>& CancelRequested,
	TArray<FVector4f>& OutCoefficients,
	FString& OutError)
{
	if (!Session.IsValid() || !Session->IsValid() || Samples.IsEmpty())
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
	Context->Session = Session;
	Context->Samples.Append(Samples.GetData(), Samples.Num());
	Context->OutputBytes =
		static_cast<uint32>(Samples.Num()) * sizeof(FVector4f);
	Context->CoefficientSpace = CoefficientSpace;
	Context->ThicknessScale = ThicknessScale;

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
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->TrianglePositions);
			const FRDGBufferRef TriangleTangentsBuffer =
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->TriangleTangents);
			const FRDGBufferRef TriangleNormalsBuffer =
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->TriangleNormals);
			const FRDGBufferRef BVHNodesBuffer =
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->BVHNodes);
			const FRDGBufferRef BVHTriangleIndicesBuffer =
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->BVHTriangleIndices);
			const FRDGBufferRef DirectionsBuffer =
				GraphBuilder.RegisterExternalBuffer(
					Context->Session->Directions);
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
			Parameters->DirectionCount =
				Context->Session->DirectionCount;
			Parameters->DispatchGroupsX = DispatchGroupsX;
			Parameters->CoefficientSpace = static_cast<uint32>(
				Context->CoefficientSpace);
			Parameters->ThicknessScale =
				Context->ThicknessScale;
			Parameters->NormalOffset =
				static_cast<float>(NormalizedRayNormalOffset);
			Parameters->NearClip =
				static_cast<float>(NormalizedRayNearClip);
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

	const double DeadlineSeconds =
		FPlatformTime::Seconds() + GPUOperationTimeoutSeconds;
	EGPUWaitResult WaitResult = WaitForGPUEvent(
		*Context->CompletionEvent,
		CancelRequested,
		DeadlineSeconds);
	if (WaitResult == EGPUWaitResult::Cancelled)
	{
		OutError = TEXT("GPU Bake was cancelled while dispatching a tile.");
		return false;
	}
	if (WaitResult == EGPUWaitResult::TimedOut)
	{
		OutError =
			TEXT("GPU Bake tile dispatch/readback timed out after 60 seconds.");
		return false;
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

		WaitResult = WaitForGPUEvent(
			*Context->CompletionEvent,
			CancelRequested,
			DeadlineSeconds);
		if (WaitResult == EGPUWaitResult::Cancelled)
		{
			OutError =
				TEXT("GPU Bake was cancelled while reading back a tile.");
			return false;
		}
		if (WaitResult == EGPUWaitResult::TimedOut)
		{
			OutError =
				TEXT("GPU Bake tile dispatch/readback timed out after 60 seconds.");
			return false;
		}
		if (!Context->bReadbackComplete
			&& Context->Error.IsEmpty())
		{
			FPlatformProcess::SleepNoStats(0.005f);
		}
	}

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

void ComputeGPUTileGutterTexels(
	FImageOccupancyMap& OccupancyMap,
	const FImageTile& Tile,
	const FImageDimensions& Dimensions,
	const FBSplineFilter& Filter)
{
	const int32 SamplesPerTexel = OccupancyMap.PixelSampler.Num();
	TBitArray<> CoveredTexels(
		false,
		static_cast<int32>(Tile.Num()));

	for (int64 PaddedTexelIndex = 0;
		PaddedTexelIndex < OccupancyMap.Tile.Num();
		++PaddedTexelIndex)
	{
		const FVector2i TexelCoords =
			OccupancyMap.Tile.GetSourceCoords(PaddedTexelIndex);
		for (int32 SampleIndex = 0;
			SampleIndex < SamplesPerTexel;
			++SampleIndex)
		{
			const int64 PaddedSampleIndex =
				PaddedTexelIndex * SamplesPerTexel + SampleIndex;
			if (!OccupancyMap.IsInterior(PaddedSampleIndex))
			{
				continue;
			}

			const FVector2i KernelStart(
				FMath::Clamp(
					TexelCoords.X - GPUFilterTilePadding,
					0,
					Dimensions.GetWidth()),
				FMath::Clamp(
					TexelCoords.Y - GPUFilterTilePadding,
					0,
					Dimensions.GetHeight()));
			const FVector2i KernelEnd(
				FMath::Clamp(
					TexelCoords.X + GPUFilterTilePadding + 1,
					0,
					Dimensions.GetWidth()),
				FMath::Clamp(
					TexelCoords.Y + GPUFilterTilePadding + 1,
					0,
					Dimensions.GetHeight()));
			const FImageTile KernelTile(KernelStart, KernelEnd);

			const FVector2d TexelSize = Dimensions.GetTexelSize();
			const FVector2d TexelCenterUV =
				Dimensions.GetTexelUV(TexelCoords);
			const FVector2d SampleUVInTexel =
				OccupancyMap.PixelSampler.Sample(SampleIndex);
			const FVector2d SampleUV =
				TexelCenterUV - 0.5 * TexelSize
				+ SampleUVInTexel * TexelSize;
			const int32 SampleUVChart =
				OccupancyMap.TexelQueryUVChart[PaddedTexelIndex];

			for (int64 KernelIndex = 0;
				KernelIndex < KernelTile.Num();
				++KernelIndex)
			{
				const FVector2i NeighborCoords =
					KernelTile.GetSourceCoords(KernelIndex);
				if (!Tile.Contains(
					NeighborCoords.X,
					NeighborCoords.Y))
				{
					continue;
				}

				const int64 NeighborPaddedIndex =
					OccupancyMap.Tile.GetIndexFromSourceCoords(
						NeighborCoords);
				if (SampleUVChart
					!= OccupancyMap.TexelQueryUVChart[
						NeighborPaddedIndex])
				{
					continue;
				}

				const FVector2d NeighborCenterUV =
					Dimensions.GetTexelUV(NeighborCoords);
				const FVector2d TexelDistance =
					Dimensions.GetTexelDistance(
						NeighborCenterUV,
						SampleUV);
				if (Filter.IsInFilterRegion(TexelDistance))
				{
					CoveredTexels[
						static_cast<int32>(
							Tile.GetIndexFromSourceCoords(
								NeighborCoords))] = true;
				}
			}
		}
	}

	for (int64 TileTexelIndex = 0;
		TileTexelIndex < Tile.Num();
		++TileTexelIndex)
	{
		if (CoveredTexels[static_cast<int32>(TileTexelIndex)])
		{
			continue;
		}

		const FVector2i GutterCoords =
			Tile.GetSourceCoords(TileTexelIndex);
		const int64 PaddedTexelIndex =
			OccupancyMap.Tile.GetIndexFromSourceCoords(GutterCoords);
		for (int32 SampleIndex = 0;
			SampleIndex < SamplesPerTexel;
			++SampleIndex)
		{
			const int64 PaddedSampleIndex =
				PaddedTexelIndex * SamplesPerTexel + SampleIndex;
			if (OccupancyMap.TexelType[PaddedSampleIndex]
				!= FImageOccupancyMap::GutterTexel)
			{
				continue;
			}

			const FVector2d NearestUV =
				static_cast<FVector2d>(
					OccupancyMap.TexelQueryUV[PaddedSampleIndex]);
			const FVector2i SourceCoords =
				Dimensions.UVToCoords(NearestUV);
			OccupancyMap.GutterTexels.Emplace(
				Dimensions.GetIndex(GutterCoords),
				Dimensions.GetIndex(SourceCoords));
			break;
		}
	}
}

bool BakeGPUTarget(
	FBakeJob& Job,
	const FBakeTargetPreparation& Target,
	const TSharedPtr<const FGPUSession, ESPMode::ThreadSafe>& Session,
	TArray64<FVector4f>& OutCoefficientImage)
{
	if (!Target.DynamicMesh
		|| !Target.DynamicMesh->HasAttributes())
	{
		Job.Error = FString::Printf(
			TEXT("GPU target mesh data is invalid for %s."),
			*GetNameSafe(Target.SourceMesh.Get()));
		return false;
	}

	FDynamicMesh3& SurfaceMesh = *Target.DynamicMesh;
	const FDynamicMeshUVOverlay* UVOverlay =
		SurfaceMesh.Attributes()->GetUVLayer(
			Job.Preparation.Settings.BakeUVChannel);
	if (UVOverlay == nullptr)
	{
		Job.Error = FString::Printf(
			TEXT("GPU target mesh has no UV%d for %s."),
			Job.Preparation.Settings.BakeUVChannel,
			*GetNameSafe(Target.SourceMesh.Get()));
		return false;
	}

	FDynamicMesh3 FlatMesh(EMeshComponents::FaceGroups);
	for (const int32 TriangleID : SurfaceMesh.TriangleIndicesItr())
	{
		if (!UVOverlay->IsSetTriangle(TriangleID))
		{
			continue;
		}

		FVector2f A;
		FVector2f B;
		FVector2f C;
		UVOverlay->GetTriElements(TriangleID, A, B, C);
		const int32 VertexA =
			FlatMesh.AppendVertex(FVector3d(A.X, A.Y, 0.0));
		const int32 VertexB =
			FlatMesh.AppendVertex(FVector3d(B.X, B.Y, 0.0));
		const int32 VertexC =
			FlatMesh.AppendVertex(FVector3d(C.X, C.Y, 0.0));
		FlatMesh.AppendTriangle(
			VertexA,
			VertexB,
			VertexC,
			TriangleID);
	}
	if (FlatMesh.TriangleCount() == 0)
	{
		Job.Error = FString::Printf(
			TEXT("GPU target mesh has no UV triangles for %s."),
			*GetNameSafe(Target.SourceMesh.Get()));
		return false;
	}

	FDynamicMeshAABBTree3 FlatSpatial(&FlatMesh, true);
	TArray<int32> UVCharts;
	FMeshMapBaker::ComputeUVCharts(SurfaceMesh, UVCharts);
	FMeshSurfaceUVSampler UVSampler;
	UVSampler.Initialize(
		&SurfaceMesh,
		UVOverlay,
		EMeshSurfaceSamplerQueryType::TriangleAndUV);

	const int32 Resolution = GetTextureResolution(
		Job.Preparation.Settings.TextureResolution);
	const FImageDimensions Dimensions(Resolution, Resolution);
	const int64 PixelCount =
		static_cast<int64>(Resolution) * Resolution;
	TArray64<FVector4f> AccumulatedCoefficients;
	AccumulatedCoefficients.SetNumZeroed(PixelCount);
	TArray64<float> AccumulatedWeights;
	AccumulatedWeights.SetNumZeroed(PixelCount);
	const FBSplineFilter Filter(GPUBSplineFilterRadius);
	const FImageTiling Tiles(
		Dimensions,
		GPUImageTileSize,
		GPUImageTileSize);

	for (int32 TileIndex = 0;
		TileIndex < Tiles.Num();
		++TileIndex)
	{
		if (Job.bCancelRequested.load(std::memory_order_relaxed))
		{
			return false;
		}

		const FImageTile Tile = Tiles.GetTile(TileIndex);
		const FImageTile PaddedTile =
			Tiles.GetTile(TileIndex, GPUFilterTilePadding);
		FImageOccupancyMap OccupancyMap;
		OccupancyMap.GutterSize =
			FMath::Max(1, Job.Preparation.Settings.PaddingSize);
		OccupancyMap.Initialize(
			Dimensions,
			PaddedTile,
			Job.Preparation.Settings.SamplesPerPixel);
		const auto GetTriangleID =
			[&FlatMesh](const int32 TriangleID)
			{
				return FlatMesh.GetTriangleGroup(TriangleID);
			};
		OccupancyMap.ClassifySamplesFromUVSpaceMesh(
			FlatMesh,
			FlatSpatial,
			GetTriangleID,
			&UVCharts);

		TArray<FVector4f> TileGPUInputs;
		TArray<FGPUTileSample> TileSamples;
		const int32 MaximumTileSamples =
			static_cast<int32>(Tile.Num())
			* Job.Preparation.Settings.SamplesPerPixel;
		TileGPUInputs.Reserve(MaximumTileSamples);
		TileSamples.Reserve(MaximumTileSamples);

		const int32 SamplesPerTexel =
			OccupancyMap.PixelSampler.Num();
		for (int64 TileTexelIndex = 0;
			TileTexelIndex < Tile.Num();
			++TileTexelIndex)
		{
			const FVector2i ImageCoords =
				Tile.GetSourceCoords(TileTexelIndex);
			const int64 PaddedTexelIndex =
				OccupancyMap.Tile.GetIndexFromSourceCoords(
					ImageCoords);
			if (OccupancyMap.TexelNumSamples(PaddedTexelIndex) == 0)
			{
				continue;
			}

			for (int32 SampleIndex = 0;
				SampleIndex < SamplesPerTexel;
				++SampleIndex)
			{
				const int64 PaddedSampleIndex =
					PaddedTexelIndex * SamplesPerTexel + SampleIndex;
				if (!OccupancyMap.IsInterior(PaddedSampleIndex))
				{
					continue;
				}

				const FVector2d QueryUV =
					static_cast<FVector2d>(
						OccupancyMap.TexelQueryUV[
							PaddedSampleIndex]);
				const int32 UVTriangleID =
					OccupancyMap.TexelQueryTriangle[
						PaddedSampleIndex];
				FMeshUVSampleInfo SampleInfo;
				if (!UVSampler.QuerySampleInfo(
					UVTriangleID,
					QueryUV,
					SampleInfo))
				{
					Job.Error = FString::Printf(
						TEXT("GPU UV sample lookup failed for %s."),
						*GetNameSafe(Target.SourceMesh.Get()));
					return false;
				}

				const int32 SurfaceTriangleID =
					SampleInfo.TriangleIndex;
				if (!Target.CombinedTriangleIDs.IsValidIndex(
						SurfaceTriangleID)
					|| Target.CombinedTriangleIDs[
						SurfaceTriangleID] == INDEX_NONE)
				{
					Job.Error = FString::Printf(
						TEXT("GPU triangle mapping is invalid for %s."),
						*GetNameSafe(Target.SourceMesh.Get()));
					return false;
				}

				TileGPUInputs.Emplace(
					static_cast<float>(
						SampleInfo.BaryCoords.X),
					static_cast<float>(
						SampleInfo.BaryCoords.Y),
					static_cast<float>(
						SampleInfo.BaryCoords.Z),
					FPlatformMath::AsFloat(
						static_cast<uint32>(
							Target.CombinedTriangleIDs[
								SurfaceTriangleID])));

				FGPUTileSample& TileSample =
					TileSamples.AddDefaulted_GetRef();
				const FVector2d TexelSize =
					Dimensions.GetTexelSize();
				const FVector2d TexelCenterUV =
					Dimensions.GetTexelUV(ImageCoords);
				TileSample.FilterUV =
					TexelCenterUV - 0.5 * TexelSize
					+ OccupancyMap.PixelSampler.Sample(SampleIndex)
						* TexelSize;
				TileSample.ImageCoords = ImageCoords;
				TileSample.UVChart =
					OccupancyMap.TexelQueryUVChart[
						PaddedTexelIndex];
			}
		}

		if (!TileGPUInputs.IsEmpty())
		{
			TArray<FVector4f> TileResults;
			if (!DispatchGPUComputeBatch(
				Session,
				TileGPUInputs,
				static_cast<float>(
					Job.Preparation.ThicknessScale),
				Job.Preparation.Settings.CoefficientSpace,
				Job.bCancelRequested,
				TileResults,
				Job.Error))
			{
				return false;
			}
			if (TileResults.Num() != TileSamples.Num())
			{
				Job.Error =
					TEXT("GPU tile returned an unexpected sample count.");
				return false;
			}

			for (int32 SampleIndex = 0;
				SampleIndex < TileSamples.Num();
				++SampleIndex)
			{
				const FGPUTileSample& Sample =
					TileSamples[SampleIndex];
				const FVector2i KernelStart(
					FMath::Clamp(
						Sample.ImageCoords.X
							- GPUFilterTilePadding,
						0,
						Dimensions.GetWidth()),
					FMath::Clamp(
						Sample.ImageCoords.Y
							- GPUFilterTilePadding,
						0,
						Dimensions.GetHeight()));
				const FVector2i KernelEnd(
					FMath::Clamp(
						Sample.ImageCoords.X
							+ GPUFilterTilePadding + 1,
						0,
						Dimensions.GetWidth()),
					FMath::Clamp(
						Sample.ImageCoords.Y
							+ GPUFilterTilePadding + 1,
						0,
						Dimensions.GetHeight()));
				const FImageTile KernelTile(KernelStart, KernelEnd);

				for (int64 KernelIndex = 0;
					KernelIndex < KernelTile.Num();
					++KernelIndex)
				{
					const FVector2i NeighborCoords =
						KernelTile.GetSourceCoords(KernelIndex);
					const int64 NeighborPaddedIndex =
						OccupancyMap.Tile
							.GetIndexFromSourceCoords(
								NeighborCoords);
					if (Sample.UVChart
						!= OccupancyMap.TexelQueryUVChart[
							NeighborPaddedIndex])
					{
						continue;
					}

					const FVector2d NeighborCenterUV =
						Dimensions.GetTexelUV(NeighborCoords);
					const float Weight = Filter.GetWeight(
						Dimensions.GetTexelDistance(
							NeighborCenterUV,
							Sample.FilterUV));
					if (Weight == 0.0f)
					{
						continue;
					}

					const int64 PixelIndex =
						Dimensions.GetIndex(NeighborCoords);
					AccumulatedCoefficients[PixelIndex] +=
						TileResults[SampleIndex] * Weight;
					AccumulatedWeights[PixelIndex] += Weight;
				}
			}
		}

		Job.ProcessedSurfaceSamples.fetch_add(
			1,
			std::memory_order_relaxed);
	}

	Job.Stage.store(EJobStage::Filtering);
	for (int64 PixelIndex = 0;
		PixelIndex < PixelCount;
		++PixelIndex)
	{
		if (AccumulatedWeights[PixelIndex] > 0.0f)
		{
			AccumulatedCoefficients[PixelIndex] /=
				AccumulatedWeights[PixelIndex];
		}
	}
	OutCoefficientImage = MoveTemp(AccumulatedCoefficients);
	AccumulatedWeights.Reset();

	if (Job.Preparation.Settings.PaddingSize > 0)
	{
		for (int32 TileIndex = 0;
			TileIndex < Tiles.Num();
			++TileIndex)
		{
			if (Job.bCancelRequested.load(
				std::memory_order_relaxed))
			{
				return false;
			}

			const FImageTile Tile = Tiles.GetTile(TileIndex);
			const FImageTile PaddedTile =
				Tiles.GetTile(
					TileIndex,
					GPUFilterTilePadding);
			FImageOccupancyMap OccupancyMap;
			OccupancyMap.GutterSize =
				Job.Preparation.Settings.PaddingSize;
			OccupancyMap.Initialize(
				Dimensions,
				PaddedTile,
				Job.Preparation.Settings.SamplesPerPixel);
			const auto GetTriangleID =
				[&FlatMesh](const int32 TriangleID)
				{
					return FlatMesh.GetTriangleGroup(TriangleID);
				};
			OccupancyMap.ClassifySamplesFromUVSpaceMesh(
				FlatMesh,
				FlatSpatial,
				GetTriangleID,
				&UVCharts);
			ComputeGPUTileGutterTexels(
				OccupancyMap,
				Tile,
				Dimensions,
				Filter);

			for (const TTuple<int64, int64>& Gutter :
				OccupancyMap.GutterTexels)
			{
				int64 DestinationPixel;
				int64 SourcePixel;
				Tie(DestinationPixel, SourcePixel) = Gutter;
				if (OutCoefficientImage.IsValidIndex(
						DestinationPixel)
					&& OutCoefficientImage.IsValidIndex(
						SourcePixel))
				{
					OutCoefficientImage[DestinationPixel] =
						OutCoefficientImage[SourcePixel];
				}
			}
		}
	}

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
	TSharedPtr<FGPUBakeData, ESPMode::ThreadSafe> GPUData =
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

	TSharedPtr<FGPUSession, ESPMode::ThreadSafe> GPUSession;
	if (!InitializeGPUSession(
		GPUData,
		Job.bCancelRequested,
		GPUSession,
		Job.Error))
	{
		Job.Stage.store(
			Job.bCancelRequested.load(std::memory_order_relaxed)
				? EJobStage::Cancelled
				: EJobStage::Failed);
		return;
	}
	GPUData.Reset();

	TArray<TArray64<FVector4f>> CoefficientImages;
	CoefficientImages.Reserve(Job.Preparation.Targets.Num());
	const int32 Resolution = GetTextureResolution(
		Job.Preparation.Settings.TextureResolution);
	const FImageTiling Tiles(
		FImageDimensions(Resolution, Resolution),
		GPUImageTileSize,
		GPUImageTileSize);
	Job.TotalSurfaceSamples.store(
		static_cast<int64>(Tiles.Num())
			* Job.Preparation.Targets.Num());
	Job.ProcessedSurfaceSamples.store(0);
	Job.Stage.store(EJobStage::GPUComputing);

	for (const FBakeTargetPreparation& Target :
		Job.Preparation.Targets)
	{
		TArray64<FVector4f>& CoefficientImage =
			CoefficientImages.AddDefaulted_GetRef();
		if (!BakeGPUTarget(
			Job,
			Target,
			GPUSession,
			CoefficientImage))
		{
			Job.Stage.store(
				Job.bCancelRequested.load(
					std::memory_order_relaxed)
					? EJobStage::Cancelled
					: EJobStage::Failed);
			return;
		}
		Job.Stage.store(EJobStage::GPUComputing);
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
