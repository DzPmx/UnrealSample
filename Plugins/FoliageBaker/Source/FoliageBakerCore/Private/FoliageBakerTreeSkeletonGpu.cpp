#include "FoliageBakerTreeSkeletonGpu.h"

#include "FoliageBakerTreeSkeleton.h"

#include "Algo/Sort.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderCommandFence.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerTreeSkeletonGpu, Log, All);

namespace
{
	constexpr int32 BvhLeafTriangleCount = 8;
	constexpr uint32 DistanceBucketCount = 2048;

	struct alignas(16) FGpuBvhNode
	{
		FVector3f BoundsMin = FVector3f::ZeroVector;
		uint32 LeftChild = 0;
		FVector3f BoundsMax = FVector3f::ZeroVector;
		uint32 RightChild = 0;
		uint32 FirstTriangle = 0;
		uint32 TriangleCount = 0;
		uint32 Padding0 = 0;
		uint32 Padding1 = 0;
	};

	struct alignas(16) FGpuBvhTriangle
	{
		FVector3f A = FVector3f::ZeroVector;
		float PaddingA = 0.0f;
		FVector3f B = FVector3f::ZeroVector;
		float PaddingB = 0.0f;
		FVector3f C = FVector3f::ZeroVector;
		float PaddingC = 0.0f;
	};

	class FBvhBuilder final
	{
	public:
		explicit FBvhBuilder(
			const TArray<FFoliageBakerTreeSkeletonTriangle>& InTriangles)
			: Triangles(InTriangles)
		{
			TriangleIndices.Reserve(Triangles.Num());
			for (int32 TriangleIndex = 0;
				TriangleIndex < Triangles.Num();
				++TriangleIndex)
			{
				const FFoliageBakerTreeSkeletonTriangle& Triangle =
					Triangles[TriangleIndex];
				if (!Triangle.A.ContainsNaN()
					&& !Triangle.B.ContainsNaN()
					&& !Triangle.C.ContainsNaN())
				{
					TriangleIndices.Add(TriangleIndex);
				}
			}
		}

		bool Build(TArray<FGpuBvhNode>& OutNodes, TArray<FGpuBvhTriangle>& OutTriangles)
		{
			if (TriangleIndices.IsEmpty())
			{
				return false;
			}
			Nodes.Reset();
			OrderedTriangles.Reset();
			BuildNode(0, TriangleIndices.Num());
			OutNodes = MoveTemp(Nodes);
			OutTriangles = MoveTemp(OrderedTriangles);
			return !OutNodes.IsEmpty() && !OutTriangles.IsEmpty();
		}

	private:
		int32 BuildNode(const int32 BeginIndex, const int32 EndIndex)
		{
			const int32 NodeIndex = Nodes.AddDefaulted();
			FBox Bounds(ForceInit);
			FBox CentroidBounds(ForceInit);
			for (int32 OrderIndex = BeginIndex; OrderIndex < EndIndex; ++OrderIndex)
			{
				const FFoliageBakerTreeSkeletonTriangle& Triangle =
					Triangles[TriangleIndices[OrderIndex]];
				Bounds += Triangle.A;
				Bounds += Triangle.B;
				Bounds += Triangle.C;
				CentroidBounds += (Triangle.A + Triangle.B + Triangle.C) / 3.0;
			}
			Nodes[NodeIndex].BoundsMin = FVector3f(Bounds.Min);
			Nodes[NodeIndex].BoundsMax = FVector3f(Bounds.Max);
			const int32 Count = EndIndex - BeginIndex;
			if (Count <= BvhLeafTriangleCount)
			{
				Nodes[NodeIndex].FirstTriangle = OrderedTriangles.Num();
				Nodes[NodeIndex].TriangleCount = Count;
				for (int32 OrderIndex = BeginIndex; OrderIndex < EndIndex; ++OrderIndex)
				{
					const FFoliageBakerTreeSkeletonTriangle& Triangle =
						Triangles[TriangleIndices[OrderIndex]];
					FGpuBvhTriangle& GpuTriangle = OrderedTriangles.AddDefaulted_GetRef();
					GpuTriangle.A = FVector3f(Triangle.A);
					GpuTriangle.B = FVector3f(Triangle.B);
					GpuTriangle.C = FVector3f(Triangle.C);
				}
				return NodeIndex;
			}

			const FVector CentroidSize = CentroidBounds.GetSize();
			const int32 Axis = CentroidSize.X >= CentroidSize.Y
				&& CentroidSize.X >= CentroidSize.Z
					? 0
					: CentroidSize.Y >= CentroidSize.Z ? 1 : 2;
			TArrayView<int32> Range(
				TriangleIndices.GetData() + BeginIndex,
				Count);
			Algo::Sort(
				Range,
				[this, Axis](const int32 FirstIndex, const int32 SecondIndex)
				{
					const FFoliageBakerTreeSkeletonTriangle& First = Triangles[FirstIndex];
					const FFoliageBakerTreeSkeletonTriangle& Second = Triangles[SecondIndex];
					const double FirstCoordinate = (First.A[Axis] + First.B[Axis] + First.C[Axis]) / 3.0;
					const double SecondCoordinate = (Second.A[Axis] + Second.B[Axis] + Second.C[Axis]) / 3.0;
					return FirstCoordinate == SecondCoordinate
						? FirstIndex < SecondIndex
						: FirstCoordinate < SecondCoordinate;
				});
			const int32 MiddleIndex = BeginIndex + Count / 2;
			const int32 LeftChild = BuildNode(BeginIndex, MiddleIndex);
			const int32 RightChild = BuildNode(MiddleIndex, EndIndex);
			Nodes[NodeIndex].LeftChild = LeftChild;
			Nodes[NodeIndex].RightChild = RightChild;
			return NodeIndex;
		}

		const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles;
		TArray<int32> TriangleIndices;
		TArray<FGpuBvhNode> Nodes;
		TArray<FGpuBvhTriangle> OrderedTriangles;
	};

	class FFoliageBakerTreeSkeletonShader : public FGlobalShader
	{
	public:
		FFoliageBakerTreeSkeletonShader() = default;
		explicit FFoliageBakerTreeSkeletonShader(
			const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer)
		{
		}

		static bool ShouldCompilePermutation(
			const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
		}
	};

	class FFoliageBakerClassifyVoxelsCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerClassifyVoxelsCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerClassifyVoxelsCS, FFoliageBakerTreeSkeletonShader);
		class FClassificationAxis : SHADER_PERMUTATION_INT("FOLIAGE_BAKER_CLASSIFICATION_AXIS", 3);
		using FPermutationDomain = TShaderPermutationDomain<FClassificationAxis>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FBvhTriangle>, BvhTriangles)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, IntersectionEvents)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Statistics)
			SHADER_PARAMETER(FIntVector, Dimensions)
			SHADER_PARAMETER(FVector3f, GridOrigin)
			SHADER_PARAMETER(float, CellSize)
			SHADER_PARAMETER(uint32, TriangleCount)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_CLASSIFY"), 1);
		}
	};

	class FFoliageBakerFillVoxelsCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerFillVoxelsCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerFillVoxelsCS, FFoliageBakerTreeSkeletonShader);
		class FScanAxis : SHADER_PERMUTATION_INT("FOLIAGE_BAKER_SCAN_AXIS", 3);
		using FPermutationDomain = TShaderPermutationDomain<FScanAxis>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IntersectionEvents)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER(FIntVector, Dimensions)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_FILL"), 1);
		}
	};

	class FFoliageBakerFinalizeVoxelsCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerFinalizeVoxelsCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerFinalizeVoxelsCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Statistics)
			SHADER_PARAMETER(FIntVector, Dimensions)
			SHADER_PARAMETER(FVector3f, GridOrigin)
			SHADER_PARAMETER(float, CellSize)
			SHADER_PARAMETER(FVector3f, Pivot)
			SHADER_PARAMETER(float, AnchorRadiusSquared)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_FINALIZE"), 1);
		}
	};

	class FFoliageBakerInitializeDistanceCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerInitializeDistanceCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerInitializeDistanceCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OccupiedVoxels)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DistanceCandidates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DistanceCandidateCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DistanceStatistics)
			SHADER_PARAMETER(FIntVector, Dimensions)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_DISTANCE_INITIALIZE"), 1);
		}
	};

	class FFoliageBakerPropagateDistanceCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerPropagateDistanceCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerPropagateDistanceCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurrentDistanceCandidates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurrentDistanceCandidateCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NextDistanceCandidates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NextDistanceCandidateCount)
			SHADER_PARAMETER(FIntVector, Dimensions)
			SHADER_PARAMETER(uint32, CurrentDistanceQueueBit)
			SHADER_PARAMETER(uint32, NextDistanceQueueBit)
			RDG_BUFFER_ACCESS(DistanceIndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_DISTANCE_PROPAGATE"), 1);
		}
	};

	class FFoliageBakerBucketDistanceCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerBucketDistanceCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerBucketDistanceCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BucketOccupiedVoxels)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DistanceHistogram)
			SHADER_PARAMETER(uint32, BucketOccupiedVoxelCount)
			SHADER_PARAMETER(uint32, DistanceBucketCount)
			SHADER_PARAMETER(uint32, DistanceBucketRowWidth)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_DISTANCE_BUCKET"), 1);
		}
	};

	class FFoliageBakerThinCandidatesCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerThinCandidatesCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerThinCandidatesCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurrentCandidates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CurrentCandidateCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NextCandidates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NextCandidateCount)
			SHADER_PARAMETER(FIntVector, Dimensions)
			SHADER_PARAMETER(uint32, VoxelColor)
			SHADER_PARAMETER(uint32, CurrentQueueBit)
			SHADER_PARAMETER(uint32, NextQueueBit)
			SHADER_PARAMETER(uint32, CurrentDistanceLayer)
			SHADER_PARAMETER(uint32, QueueNextCandidates)
			SHADER_PARAMETER(uint32, CandidateStartOffset)
			SHADER_PARAMETER(uint32, CandidateCountOffset)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_THIN"), 1);
		}
	};

	class FFoliageBakerScatterDistanceBucketsCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerScatterDistanceBucketsCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerScatterDistanceBucketsCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, UnsortedOccupiedVoxels)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SortedOccupiedVoxels)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DistanceBucketCursors)
			SHADER_PARAMETER(uint32, OccupiedVoxelCount)
			SHADER_PARAMETER(uint32, ScatterRowWidth)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_SCATTER"), 1);
		}
	};

	class FFoliageBakerCompactSkeletonCS final : public FFoliageBakerTreeSkeletonShader
	{
		DECLARE_GLOBAL_SHADER(FFoliageBakerCompactSkeletonCS);
		SHADER_USE_PARAMETER_STRUCT(FFoliageBakerCompactSkeletonCS, FFoliageBakerTreeSkeletonShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, VoxelStates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SkeletonVoxels)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SkeletonDistanceBuckets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Statistics)
			SHADER_PARAMETER(FIntVector, Dimensions)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(
			const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FFoliageBakerTreeSkeletonShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("FOLIAGE_BAKER_COMPACT"), 1);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerClassifyVoxelsCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"ClassifyVoxelsCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerFillVoxelsCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"FillVoxelsCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerFinalizeVoxelsCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"FinalizeVoxelsCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerInitializeDistanceCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"InitializeDistanceCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerPropagateDistanceCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"PropagateDistanceCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerBucketDistanceCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"BucketDistanceCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerScatterDistanceBucketsCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"ScatterDistanceBucketsCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerThinCandidatesCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"ThinCandidatesCS",
		SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(
		FFoliageBakerCompactSkeletonCS,
		"/Plugin/FoliageBaker/Private/FoliageBakerTreeSkeleton.usf",
		"CompactSkeletonCS",
		SF_Compute);

	struct FGpuReadbackPayload
	{
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ClassificationStatisticsReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> DistanceHistogramReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> OccupiedVoxelReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> SkeletonStatisticsReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> SkeletonReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> SkeletonDistanceReadback;
		TRefCountPtr<FRDGPooledBuffer> VoxelStateBuffer;
		TRefCountPtr<FRDGPooledBuffer> OccupiedVoxelBuffer;
		TRefCountPtr<FRDGPooledBuffer> SortedOccupiedVoxelBuffer;
		TRefCountPtr<FRDGPooledBuffer> SkeletonBuffer;
		TRefCountPtr<FRDGPooledBuffer> SkeletonDistanceBuffer;
		uint32 OccupiedVoxelCount = 0;
		uint32 SkeletonVoxelCount = 0;
		uint32 IntersectionEventCount = 0;
		TArray<uint32> DistanceBucketCounts;
		TArray<int32> OccupiedVoxelIndices;
		TArray<float> OccupiedVoxelRadii;
		TArray<int32> SkeletonVoxelIndices;
		TArray<float> SkeletonVoxelRadii;
	};
}

FFoliageBakerGpuTreeSkeletonResult FFoliageBakerTreeSkeletonGpu::Build(
	const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
	const FBox& SourceBounds,
	const FVector& Pivot,
	const int32 TargetResolution)
{
	FFoliageBakerGpuTreeSkeletonResult Result;
	if (Triangles.IsEmpty()
		|| !SourceBounds.IsValid
		|| TargetResolution <= 0
		|| GMaxRHIFeatureLevel < ERHIFeatureLevel::SM6)
	{
		Result.Report = TEXT("GPU skeletonization requires finite triangles and an SM6 RHI.");
		return Result;
	}

	TArray<FGpuBvhNode> BvhNodes;
	TArray<FGpuBvhTriangle> BvhTriangles;
	FBvhBuilder BvhBuilder(Triangles);
	if (!BvhBuilder.Build(BvhNodes, BvhTriangles))
	{
		Result.Report = TEXT("GPU skeletonization could not build a finite source BVH.");
		return Result;
	}

	const double CellSize = SourceBounds.GetSize().GetMax() / static_cast<double>(TargetResolution);
	if (!FMath::IsFinite(CellSize) || CellSize <= UE_DOUBLE_SMALL_NUMBER)
	{
		Result.Report = TEXT("GPU skeletonization could not derive a finite cell size.");
		return Result;
	}
	const FVector GridOrigin = SourceBounds.Min - FVector(CellSize * 2.0);
	const FVector GridSize = SourceBounds.GetSize() + FVector(CellSize * 4.0);
	const FIntVector Dimensions(
		FMath::CeilToInt(GridSize.X / CellSize) + 1,
		FMath::CeilToInt(GridSize.Y / CellSize) + 1,
		FMath::CeilToInt(GridSize.Z / CellSize) + 1);
	const uint64 GridCellCount64 = static_cast<uint64>(Dimensions.X)
		* static_cast<uint64>(Dimensions.Y)
		* static_cast<uint64>(Dimensions.Z);
	if (GridCellCount64 == 0 || GridCellCount64 > static_cast<uint64>(MAX_int32))
	{
		Result.Report = TEXT("The 1000-cell global grid exceeds the 32-bit sparse skeleton index range.");
		return Result;
	}
	const uint32 GridCellCount = static_cast<uint32>(GridCellCount64);
	Result.Dimensions = Dimensions;
	Result.GridOrigin = GridOrigin;
	Result.CellSize = CellSize;
	const double ClassificationBufferMegabytes = static_cast<double>(GridCellCount)
		* sizeof(uint32)
		* 2.0
		/ (1024.0 * 1024.0);
	UE_LOG(
		LogFoliageBakerTreeSkeletonGpu,
		Display,
		TEXT("GPU global grid %dx%dx%d (%u cells, %.3f MiB classification working set), %.4f cm per cell."),
		Dimensions.X,
		Dimensions.Y,
		Dimensions.Z,
		GridCellCount,
		ClassificationBufferMegabytes,
		CellSize);

	const TSharedRef<FGpuReadbackPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FGpuReadbackPayload, ESPMode::ThreadSafe>();
	Payload->ClassificationStatisticsReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonClassificationStatistics"));
	Payload->DistanceHistogramReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonDistanceHistogram"));
	Payload->OccupiedVoxelReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonOccupiedVoxels"));
	Payload->SkeletonStatisticsReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonThinningStatistics"));
	Payload->SkeletonReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonVoxels"));
	Payload->SkeletonDistanceReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(
		TEXT("FoliageBakerTreeSkeletonDistances"));

	ENQUEUE_RENDER_COMMAND(FoliageBakerClassifyGpuTreeVolume)(
		[BvhTriangles = MoveTemp(BvhTriangles),
		 Dimensions,
		 GridOrigin = FVector3f(GridOrigin),
		 CellSize = static_cast<float>(CellSize),
		 Pivot = FVector3f(Pivot),
		 GridCellCount,
		 TargetResolution,
		 Payload](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef BvhTriangleBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("FoliageBaker.TreeSkeleton.BvhTriangles"),
				MakeArrayView(BvhTriangles));
			FRDGBufferRef VoxelStates = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GridCellCount),
				TEXT("FoliageBaker.TreeSkeleton.VoxelStates"));
			FRDGBufferRef Statistics = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
				TEXT("FoliageBaker.TreeSkeleton.Statistics"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VoxelStates), 0u);
			FRDGBufferRef IntersectionEvents = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), GridCellCount),
				TEXT("FoliageBaker.TreeSkeleton.IntersectionEvents"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(IntersectionEvents), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Statistics), 0u);

			TShaderMapRef<FFoliageBakerFinalizeVoxelsCS> FinalizeShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (uint32 ScanAxis = 0; ScanAxis < 3; ++ScanAxis)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(IntersectionEvents), 0u);
				FFoliageBakerClassifyVoxelsCS::FPermutationDomain ClassifyPermutation;
				ClassifyPermutation.Set<FFoliageBakerClassifyVoxelsCS::FClassificationAxis>(ScanAxis);
				TShaderMapRef<FFoliageBakerClassifyVoxelsCS> ClassifyShader(
					GetGlobalShaderMap(GMaxRHIFeatureLevel),
					ClassifyPermutation);
				FFoliageBakerClassifyVoxelsCS::FParameters* ClassifyParameters =
					GraphBuilder.AllocParameters<FFoliageBakerClassifyVoxelsCS::FParameters>();
				ClassifyParameters->BvhTriangles = GraphBuilder.CreateSRV(BvhTriangleBuffer);
				ClassifyParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
				ClassifyParameters->IntersectionEvents = GraphBuilder.CreateUAV(IntersectionEvents);
				ClassifyParameters->Statistics = GraphBuilder.CreateUAV(Statistics);
				ClassifyParameters->Dimensions = Dimensions;
				ClassifyParameters->GridOrigin = GridOrigin;
				ClassifyParameters->CellSize = CellSize;
				ClassifyParameters->TriangleCount = BvhTriangles.Num();
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.ClassifyAxis(Axis=%u)", ScanAxis),
					ClassifyShader,
					ClassifyParameters,
					FIntVector(FMath::DivideAndRoundUp(BvhTriangles.Num(), 64), 1, 1));

				FFoliageBakerFillVoxelsCS::FPermutationDomain FillPermutation;
				FillPermutation.Set<FFoliageBakerFillVoxelsCS::FScanAxis>(ScanAxis);
				TShaderMapRef<FFoliageBakerFillVoxelsCS> FillShader(
					GetGlobalShaderMap(GMaxRHIFeatureLevel),
					FillPermutation);
				const int32 FirstTransverseAxis = ScanAxis == 0 ? 1 : 0;
				const int32 SecondTransverseAxis = ScanAxis == 2 ? 1 : 2;
				FFoliageBakerFillVoxelsCS::FParameters* FillParameters =
					GraphBuilder.AllocParameters<FFoliageBakerFillVoxelsCS::FParameters>();
				FillParameters->IntersectionEvents = GraphBuilder.CreateSRV(IntersectionEvents);
				FillParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
				FillParameters->Dimensions = Dimensions;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.FillAxis(Axis=%u)", ScanAxis),
					FillShader,
					FillParameters,
					FIntVector(
						FMath::DivideAndRoundUp(Dimensions[FirstTransverseAxis], 8),
						FMath::DivideAndRoundUp(Dimensions[SecondTransverseAxis], 8),
						1));
			}

			FFoliageBakerFinalizeVoxelsCS::FParameters* FinalizeParameters =
				GraphBuilder.AllocParameters<FFoliageBakerFinalizeVoxelsCS::FParameters>();
			FinalizeParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
			FinalizeParameters->Statistics = GraphBuilder.CreateUAV(Statistics);
			FinalizeParameters->Dimensions = Dimensions;
			FinalizeParameters->GridOrigin = GridOrigin;
			FinalizeParameters->CellSize = CellSize;
			FinalizeParameters->Pivot = Pivot;
			FinalizeParameters->AnchorRadiusSquared = 0.0f;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.Finalize1000"),
				FinalizeShader,
				FinalizeParameters,
				FIntVector(
					FMath::DivideAndRoundUp(Dimensions.X, 4),
					FMath::DivideAndRoundUp(Dimensions.Y, 4),
					FMath::DivideAndRoundUp(Dimensions.Z, 4)));

			AddEnqueueCopyPass(
				GraphBuilder,
				Payload->ClassificationStatisticsReadback.Get(),
				Statistics,
				2 * sizeof(uint32));
			GraphBuilder.QueueBufferExtraction(
				VoxelStates,
				&Payload->VoxelStateBuffer);
			GraphBuilder.Execute();
		});
	FRenderCommandFence ClassificationFence;
	ClassificationFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
	ClassificationFence.Wait(true);
	ENQUEUE_RENDER_COMMAND(FoliageBakerReadGpuTreeClassificationStatistics)(
		[Payload](FRHICommandListImmediate& RHICmdList)
		{
			Payload->ClassificationStatisticsReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
			const TArrayView<const uint32> StatisticsView(
				static_cast<const uint32*>(Payload->ClassificationStatisticsReadback->Lock(2 * sizeof(uint32))),
				2);
			Payload->OccupiedVoxelCount = StatisticsView[0];
			Payload->IntersectionEventCount = StatisticsView[1];
			Payload->ClassificationStatisticsReadback->Unlock();
		});
	FRenderCommandFence ClassificationStatisticsFence;
	ClassificationStatisticsFence.BeginFence();
	ClassificationStatisticsFence.Wait(true);
	if (Payload->OccupiedVoxelCount == 0 || !Payload->VoxelStateBuffer.IsValid())
	{
		Result.Report = TEXT("GPU global voxelization produced no occupied voxels.");
		return Result;
	}
	const uint32 SparseCapacity = Payload->OccupiedVoxelCount;
	UE_LOG(
		LogFoliageBakerTreeSkeletonGpu,
		Display,
		TEXT("GPU classification produced %u occupied voxels and %u triangle-column intersections; thinning sparse capacity is %u."),
		Payload->OccupiedVoxelCount,
		Payload->IntersectionEventCount,
		SparseCapacity);

	ENQUEUE_RENDER_COMMAND(FoliageBakerMeasureGpuTreeDistances)(
		[Dimensions,
		 GridCellCount,
		 SparseCapacity,
		 Payload](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef VoxelStates = GraphBuilder.RegisterExternalBuffer(Payload->VoxelStateBuffer);
			FRDGBufferRef DistanceHistogram = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DistanceBucketCount),
				TEXT("FoliageBaker.TreeSkeleton.DistanceHistogram"));
			FRDGBufferRef OccupiedVoxels = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
				TEXT("FoliageBaker.TreeSkeleton.UnsortedOccupiedVoxels"));
			FRDGBufferRef DistanceStatistics = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
				TEXT("FoliageBaker.TreeSkeleton.DistanceStatistics"));
			FRDGBufferRef DistanceCandidateBuffers[2]{
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
					TEXT("FoliageBaker.TreeSkeleton.DistanceCandidatesA")),
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
					TEXT("FoliageBaker.TreeSkeleton.DistanceCandidatesB"))};
			FRDGBufferRef DistanceCandidateCounts[2]{
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
					TEXT("FoliageBaker.TreeSkeleton.DistanceCandidateCountA")),
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
					TEXT("FoliageBaker.TreeSkeleton.DistanceCandidateCountB"))};
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DistanceHistogram), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DistanceStatistics), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DistanceCandidateCounts[0]), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DistanceCandidateCounts[1]), 0u);

			TShaderMapRef<FFoliageBakerInitializeDistanceCS> InitializeDistanceShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FFoliageBakerInitializeDistanceCS::FParameters* InitializeDistanceParameters =
				GraphBuilder.AllocParameters<FFoliageBakerInitializeDistanceCS::FParameters>();
			InitializeDistanceParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
			InitializeDistanceParameters->OccupiedVoxels = GraphBuilder.CreateUAV(OccupiedVoxels);
			InitializeDistanceParameters->DistanceCandidates =
				GraphBuilder.CreateUAV(DistanceCandidateBuffers[0]);
			InitializeDistanceParameters->DistanceCandidateCount =
				GraphBuilder.CreateUAV(DistanceCandidateCounts[0]);
			InitializeDistanceParameters->DistanceStatistics = GraphBuilder.CreateUAV(DistanceStatistics);
			InitializeDistanceParameters->Dimensions = Dimensions;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.InitializeUnionDistance"),
				InitializeDistanceShader,
				InitializeDistanceParameters,
				FIntVector(
					FMath::DivideAndRoundUp(Dimensions.X, 4),
					FMath::DivideAndRoundUp(Dimensions.Y, 4),
					FMath::DivideAndRoundUp(Dimensions.Z, 4)));

			TShaderMapRef<FFoliageBakerPropagateDistanceCS> PropagateDistanceShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			int32 CurrentQueueIndex = 0;
			int32 NextQueueIndex = 1;
			uint32 CurrentQueueBit = 16u;
			uint32 NextQueueBit = 32u;
			constexpr int32 MaximumDistancePropagationPassCount = 512;
			for (int32 PassIndex = 0;
				PassIndex < MaximumDistancePropagationPassCount;
				++PassIndex)
			{
				FRDGBufferRef DistanceIndirectArgs = FComputeShaderUtils::AddIndirectArgsSetupCsPass1D(
					GraphBuilder,
					GMaxRHIFeatureLevel,
					DistanceCandidateCounts[CurrentQueueIndex],
					TEXT("FoliageBaker.TreeSkeleton.DistancePropagationIndirectArgs"),
					256,
					0u);
				AddClearUAVPass(
					GraphBuilder,
					GraphBuilder.CreateUAV(DistanceCandidateCounts[NextQueueIndex]),
					0u);
				FFoliageBakerPropagateDistanceCS::FParameters* PropagateParameters =
					GraphBuilder.AllocParameters<FFoliageBakerPropagateDistanceCS::FParameters>();
				PropagateParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
				PropagateParameters->CurrentDistanceCandidates =
					GraphBuilder.CreateSRV(DistanceCandidateBuffers[CurrentQueueIndex]);
				PropagateParameters->CurrentDistanceCandidateCount =
					GraphBuilder.CreateSRV(DistanceCandidateCounts[CurrentQueueIndex]);
				PropagateParameters->NextDistanceCandidates =
					GraphBuilder.CreateUAV(DistanceCandidateBuffers[NextQueueIndex]);
				PropagateParameters->NextDistanceCandidateCount =
					GraphBuilder.CreateUAV(DistanceCandidateCounts[NextQueueIndex]);
				PropagateParameters->Dimensions = Dimensions;
				PropagateParameters->CurrentDistanceQueueBit = CurrentQueueBit;
				PropagateParameters->NextDistanceQueueBit = NextQueueBit;
				PropagateParameters->DistanceIndirectArgs = DistanceIndirectArgs;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME(
						"FoliageBaker.TreeSkeleton.PropagateUnionDistance(Pass=%d)",
						PassIndex),
					PropagateDistanceShader,
					PropagateParameters,
					DistanceIndirectArgs,
					0);
				Swap(CurrentQueueIndex, NextQueueIndex);
				Swap(CurrentQueueBit, NextQueueBit);
			}

			TShaderMapRef<FFoliageBakerBucketDistanceCS> BucketDistanceShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FFoliageBakerBucketDistanceCS::FParameters* BucketParameters =
				GraphBuilder.AllocParameters<FFoliageBakerBucketDistanceCS::FParameters>();
			BucketParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
			BucketParameters->BucketOccupiedVoxels = GraphBuilder.CreateSRV(OccupiedVoxels);
			BucketParameters->DistanceHistogram = GraphBuilder.CreateUAV(DistanceHistogram);
			BucketParameters->BucketOccupiedVoxelCount = SparseCapacity;
			BucketParameters->DistanceBucketCount = DistanceBucketCount;
			constexpr uint32 DistanceBucketRowWidth = 8192u;
			const uint32 DistanceBucketRowCount = FMath::DivideAndRoundUp(
				SparseCapacity,
				DistanceBucketRowWidth);
			BucketParameters->DistanceBucketRowWidth = DistanceBucketRowWidth;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.BucketUnionDistance"),
				BucketDistanceShader,
				BucketParameters,
				FIntVector(
					FMath::DivideAndRoundUp(DistanceBucketRowWidth, 8u),
					FMath::DivideAndRoundUp(DistanceBucketRowCount, 8u),
					1));
			AddEnqueueCopyPass(
				GraphBuilder,
				Payload->DistanceHistogramReadback.Get(),
				DistanceHistogram,
				DistanceBucketCount * sizeof(uint32));
			GraphBuilder.QueueBufferExtraction(OccupiedVoxels, &Payload->OccupiedVoxelBuffer);
			GraphBuilder.Execute();
		});
	FRenderCommandFence DistanceFence;
	DistanceFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
	DistanceFence.Wait(true);
	ENQUEUE_RENDER_COMMAND(FoliageBakerReadGpuTreeDistanceHistogram)(
		[Payload](FRHICommandListImmediate& RHICmdList)
		{
			Payload->DistanceHistogramReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
			const TArrayView<const uint32> HistogramView(
				static_cast<const uint32*>(Payload->DistanceHistogramReadback->Lock(
					DistanceBucketCount * sizeof(uint32))),
				DistanceBucketCount);
			Payload->DistanceBucketCounts.Append(HistogramView);
			Payload->DistanceHistogramReadback->Unlock();
		});
	FRenderCommandFence DistanceReadbackFence;
	DistanceReadbackFence.BeginFence();
	DistanceReadbackFence.Wait(true);
	if (Payload->DistanceBucketCounts.Num() != static_cast<int32>(DistanceBucketCount)
		|| !Payload->OccupiedVoxelBuffer.IsValid())
	{
		Result.Report = TEXT("GPU Euclidean distance bucketing did not produce a sparse volume.");
		return Result;
	}
	TArray<uint32> DistanceBucketOffsets;
	DistanceBucketOffsets.SetNumUninitialized(DistanceBucketCount);
	uint32 RunningDistanceOffset = 0u;
	int32 LastOccupiedDistanceBucket = INDEX_NONE;
	for (uint32 BucketIndex = 0u; BucketIndex < DistanceBucketCount; ++BucketIndex)
	{
		DistanceBucketOffsets[BucketIndex] = RunningDistanceOffset;
		if (Payload->DistanceBucketCounts[BucketIndex] > 0u)
		{
			LastOccupiedDistanceBucket = static_cast<int32>(BucketIndex);
		}
		RunningDistanceOffset += Payload->DistanceBucketCounts[BucketIndex];
	}

	ENQUEUE_RENDER_COMMAND(FoliageBakerThinGpuTreeSkeleton)(
		[Dimensions,
		 GridCellCount,
		 SparseCapacity,
		 DistanceBucketOffsets = MoveTemp(DistanceBucketOffsets),
		 LastOccupiedDistanceBucket,
		 Payload](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef VoxelStates = GraphBuilder.RegisterExternalBuffer(
				Payload->VoxelStateBuffer);
			FRDGBufferRef UnsortedOccupiedVoxels = GraphBuilder.RegisterExternalBuffer(
				Payload->OccupiedVoxelBuffer);
			FRDGBufferRef SortedOccupiedVoxels = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
				TEXT("FoliageBaker.TreeSkeleton.SortedOccupiedVoxels"));
			FRDGBufferRef DistanceBucketCounts = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("FoliageBaker.TreeSkeleton.DistanceBucketCounts"),
				TArrayView<const uint32>(Payload->DistanceBucketCounts));
			FRDGBufferRef DistanceBucketCursors = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("FoliageBaker.TreeSkeleton.DistanceBucketCursors"),
				TArrayView<const uint32>(DistanceBucketOffsets));
			FRDGBufferRef ThinCandidateBuffers[2]{
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
					TEXT("FoliageBaker.TreeSkeleton.ThinCandidatesA")),
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
					TEXT("FoliageBaker.TreeSkeleton.ThinCandidatesB"))};
			FRDGBufferRef ThinCandidateCounts[2]{
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
					TEXT("FoliageBaker.TreeSkeleton.ThinCandidateCountA")),
				GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
					TEXT("FoliageBaker.TreeSkeleton.ThinCandidateCountB"))};
			FRDGBufferRef Statistics = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
				TEXT("FoliageBaker.TreeSkeleton.ThinningStatistics"));
			FRDGBufferRef SkeletonVoxels = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
				TEXT("FoliageBaker.TreeSkeleton.CompactedVoxels"));
			FRDGBufferRef SkeletonDistanceBuckets = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SparseCapacity),
				TEXT("FoliageBaker.TreeSkeleton.CompactedDistanceBuckets"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ThinCandidateCounts[0]), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ThinCandidateCounts[1]), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Statistics), 0u);

			TShaderMapRef<FFoliageBakerScatterDistanceBucketsCS> ScatterShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FFoliageBakerScatterDistanceBucketsCS::FParameters* ScatterParameters =
				GraphBuilder.AllocParameters<FFoliageBakerScatterDistanceBucketsCS::FParameters>();
			ScatterParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
			ScatterParameters->UnsortedOccupiedVoxels = GraphBuilder.CreateSRV(UnsortedOccupiedVoxels);
			ScatterParameters->SortedOccupiedVoxels = GraphBuilder.CreateUAV(SortedOccupiedVoxels);
			ScatterParameters->DistanceBucketCursors = GraphBuilder.CreateUAV(DistanceBucketCursors);
			ScatterParameters->OccupiedVoxelCount = SparseCapacity;
			constexpr uint32 ScatterRowWidth = 8192u;
			const uint32 ScatterRowCount = FMath::DivideAndRoundUp(
				SparseCapacity,
				ScatterRowWidth);
			ScatterParameters->ScatterRowWidth = ScatterRowWidth;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.ScatterDistanceBuckets"),
				ScatterShader,
				ScatterParameters,
				FIntVector(
					FMath::DivideAndRoundUp(ScatterRowWidth, 8u),
					FMath::DivideAndRoundUp(ScatterRowCount, 8u),
					1));

			TShaderMapRef<FFoliageBakerThinCandidatesCS> ThinShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			constexpr int32 SameBucketRelaxationCount = 64;
			for (int32 BucketIndex = 0;
				BucketIndex <= LastOccupiedDistanceBucket;
				++BucketIndex)
			{
				int32 CurrentThinQueueIndex = 0;
				int32 NextThinQueueIndex = 1;
				uint32 CurrentThinQueueBit = 32u;
				uint32 NextThinQueueBit = 16u;
				for (int32 RelaxationIndex = 0;
					RelaxationIndex < SameBucketRelaxationCount;
					++RelaxationIndex)
				{
					FRDGBufferRef CurrentThinCandidates = RelaxationIndex == 0
						? SortedOccupiedVoxels
						: ThinCandidateBuffers[CurrentThinQueueIndex];
					FRDGBufferRef CurrentThinCandidateCount = RelaxationIndex == 0
						? DistanceBucketCounts
						: ThinCandidateCounts[CurrentThinQueueIndex];
					FRDGBufferRef ThinIndirectArgs = FComputeShaderUtils::AddIndirectArgsSetupCsPass1D(
						GraphBuilder,
						GMaxRHIFeatureLevel,
						CurrentThinCandidateCount,
						TEXT("FoliageBaker.TreeSkeleton.ThinDistanceBucketIndirectArgs"),
						64,
						RelaxationIndex == 0 ? static_cast<uint32>(BucketIndex) : 0u);
					AddClearUAVPass(
						GraphBuilder,
						GraphBuilder.CreateUAV(ThinCandidateCounts[NextThinQueueIndex]),
						0u);
					for (uint32 Color = 0; Color < 27; ++Color)
					{
						FFoliageBakerThinCandidatesCS::FParameters* ThinParameters =
							GraphBuilder.AllocParameters<FFoliageBakerThinCandidatesCS::FParameters>();
						ThinParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
						ThinParameters->CurrentCandidates = GraphBuilder.CreateSRV(CurrentThinCandidates);
						ThinParameters->CurrentCandidateCount = GraphBuilder.CreateSRV(CurrentThinCandidateCount);
						ThinParameters->NextCandidates = GraphBuilder.CreateUAV(ThinCandidateBuffers[NextThinQueueIndex]);
						ThinParameters->NextCandidateCount = GraphBuilder.CreateUAV(ThinCandidateCounts[NextThinQueueIndex]);
						ThinParameters->Dimensions = Dimensions;
						ThinParameters->VoxelColor = Color;
						ThinParameters->CurrentQueueBit = RelaxationIndex == 0
							? 0u
							: CurrentThinQueueBit;
						ThinParameters->NextQueueBit = NextThinQueueBit;
						ThinParameters->CurrentDistanceLayer = static_cast<uint32>(BucketIndex);
						ThinParameters->QueueNextCandidates =
							RelaxationIndex + 1 < SameBucketRelaxationCount ? 1u : 0u;
						ThinParameters->CandidateStartOffset = RelaxationIndex == 0
							? DistanceBucketOffsets[BucketIndex]
							: 0u;
						ThinParameters->CandidateCountOffset = RelaxationIndex == 0
							? static_cast<uint32>(BucketIndex)
							: 0u;
						ThinParameters->IndirectArgs = ThinIndirectArgs;
						FComputeShaderUtils::AddPass(
							GraphBuilder,
							RDG_EVENT_NAME(
								"FoliageBaker.TreeSkeleton.ThinDistanceBucket(Bucket=%d Relaxation=%d Color=%u)",
								BucketIndex,
								RelaxationIndex,
								Color),
							ThinShader,
							ThinParameters,
							ThinIndirectArgs,
							0);
					}
					Swap(CurrentThinQueueIndex, NextThinQueueIndex);
					Swap(CurrentThinQueueBit, NextThinQueueBit);
				}
			}

			TShaderMapRef<FFoliageBakerCompactSkeletonCS> CompactShader(
				GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FFoliageBakerCompactSkeletonCS::FParameters* CompactParameters =
				GraphBuilder.AllocParameters<FFoliageBakerCompactSkeletonCS::FParameters>();
			CompactParameters->VoxelStates = GraphBuilder.CreateUAV(VoxelStates);
			CompactParameters->SkeletonVoxels = GraphBuilder.CreateUAV(SkeletonVoxels);
			CompactParameters->SkeletonDistanceBuckets =
				GraphBuilder.CreateUAV(SkeletonDistanceBuckets);
			CompactParameters->Statistics = GraphBuilder.CreateUAV(Statistics);
			CompactParameters->Dimensions = Dimensions;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("FoliageBaker.TreeSkeleton.Compact"),
				CompactShader,
				CompactParameters,
				FIntVector(
					FMath::DivideAndRoundUp(Dimensions.X, 4),
					FMath::DivideAndRoundUp(Dimensions.Y, 4),
					FMath::DivideAndRoundUp(Dimensions.Z, 4)));
			AddEnqueueCopyPass(
				GraphBuilder,
				Payload->SkeletonStatisticsReadback.Get(),
				Statistics,
				3 * sizeof(uint32));
			GraphBuilder.QueueBufferExtraction(
				SortedOccupiedVoxels,
				&Payload->SortedOccupiedVoxelBuffer,
				ERHIAccess::CopySrc);
			GraphBuilder.QueueBufferExtraction(
				SkeletonVoxels,
				&Payload->SkeletonBuffer,
				ERHIAccess::CopySrc);
			GraphBuilder.QueueBufferExtraction(
				SkeletonDistanceBuckets,
				&Payload->SkeletonDistanceBuffer,
				ERHIAccess::CopySrc);
			GraphBuilder.Execute();
		});
	FRenderCommandFence ThinningFence;
	ThinningFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
	ThinningFence.Wait(true);
	if (Payload->SortedOccupiedVoxelBuffer.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(FoliageBakerCopyGpuTreeOccupiedVoxels)(
			[SparseCapacity, Payload](FRHICommandListImmediate& RHICmdList)
			{
				Payload->OccupiedVoxelReadback->EnqueueCopy(
					RHICmdList,
					Payload->SortedOccupiedVoxelBuffer->GetRHI(),
					SparseCapacity * sizeof(uint32));
			});
		FRenderCommandFence OccupiedCopyFence;
		OccupiedCopyFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
		OccupiedCopyFence.Wait(true);
		ENQUEUE_RENDER_COMMAND(FoliageBakerReadGpuTreeOccupiedVoxels)(
			[CellSize, SparseCapacity, Payload](FRHICommandListImmediate& RHICmdList)
			{
				const uint32 ReadbackBytes = SparseCapacity * sizeof(uint32);
				Payload->OccupiedVoxelReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
				const TArrayView<const uint32> OccupiedView(
					static_cast<const uint32*>(Payload->OccupiedVoxelReadback->Lock(ReadbackBytes)),
					SparseCapacity);
				Payload->OccupiedVoxelIndices.Reserve(SparseCapacity);
				Payload->OccupiedVoxelRadii.Reserve(SparseCapacity);
				for (uint32 OccupiedIndex = 0; OccupiedIndex < SparseCapacity; ++OccupiedIndex)
				{
					Payload->OccupiedVoxelIndices.Add(
						static_cast<int32>(OccupiedView[OccupiedIndex]));
				}
				Payload->OccupiedVoxelReadback->Unlock();
				for (uint32 BucketIndex = 0; BucketIndex < DistanceBucketCount; ++BucketIndex)
				{
					const float Radius = (static_cast<float>(BucketIndex) * 0.25f + 0.5f)
						* static_cast<float>(CellSize);
					for (uint32 BucketOffset = 0;
						BucketOffset < Payload->DistanceBucketCounts[BucketIndex];
						++BucketOffset)
					{
						Payload->OccupiedVoxelRadii.Add(Radius);
					}
				}
			});
		FRenderCommandFence OccupiedReadbackFence;
		OccupiedReadbackFence.BeginFence();
		OccupiedReadbackFence.Wait(true);
	}
	ENQUEUE_RENDER_COMMAND(FoliageBakerReadGpuTreeThinningStatistics)(
		[SparseCapacity, Payload](FRHICommandListImmediate& RHICmdList)
		{
			Payload->SkeletonStatisticsReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
			const TArrayView<const uint32> StatisticsView(
				static_cast<const uint32*>(Payload->SkeletonStatisticsReadback->Lock(3 * sizeof(uint32))),
				3);
			Payload->SkeletonVoxelCount = FMath::Min(StatisticsView[2], SparseCapacity);
			Payload->SkeletonStatisticsReadback->Unlock();
		});
	FRenderCommandFence ThinningStatisticsFence;
	ThinningStatisticsFence.BeginFence();
	ThinningStatisticsFence.Wait(true);
	if (Payload->SkeletonVoxelCount > 0
		&& Payload->SkeletonBuffer.IsValid()
		&& Payload->SkeletonDistanceBuffer.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(FoliageBakerCopyGpuTreeSkeleton)(
			[Payload](FRHICommandListImmediate& RHICmdList)
			{
				const uint32 ReadbackBytes = Payload->SkeletonVoxelCount * sizeof(uint32);
				Payload->SkeletonReadback->EnqueueCopy(
					RHICmdList,
					Payload->SkeletonBuffer->GetRHI(),
					ReadbackBytes);
				Payload->SkeletonDistanceReadback->EnqueueCopy(
					RHICmdList,
					Payload->SkeletonDistanceBuffer->GetRHI(),
					ReadbackBytes);
			});
		FRenderCommandFence SkeletonCopyFence;
		SkeletonCopyFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
		SkeletonCopyFence.Wait(true);

		ENQUEUE_RENDER_COMMAND(FoliageBakerReadGpuTreeSkeleton)(
			[CellSize, Payload](FRHICommandListImmediate& RHICmdList)
			{
				const uint32 ReadbackBytes = Payload->SkeletonVoxelCount * sizeof(uint32);
				Payload->SkeletonReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
				const TArrayView<const uint32> SkeletonView(
					static_cast<const uint32*>(Payload->SkeletonReadback->Lock(ReadbackBytes)),
					Payload->SkeletonVoxelCount);
				Payload->SkeletonVoxelIndices.Reserve(Payload->SkeletonVoxelCount);
				Payload->SkeletonVoxelRadii.Reserve(Payload->SkeletonVoxelCount);
				Payload->SkeletonDistanceReadback->Wait(RHICmdList, RHICmdList.GetGPUMask());
				const TArrayView<const uint32> DistanceView(
					static_cast<const uint32*>(Payload->SkeletonDistanceReadback->Lock(ReadbackBytes)),
					Payload->SkeletonVoxelCount);
				for (uint32 SkeletonIndex = 0;
					SkeletonIndex < Payload->SkeletonVoxelCount;
					++SkeletonIndex)
				{
					Payload->SkeletonVoxelIndices.Add(static_cast<int32>(SkeletonView[SkeletonIndex]));
					Payload->SkeletonVoxelRadii.Add(
						(static_cast<float>(DistanceView[SkeletonIndex]) * 0.25f + 0.5f)
						* static_cast<float>(CellSize));
				}
				Payload->SkeletonDistanceReadback->Unlock();
				Payload->SkeletonReadback->Unlock();
			});
		FRenderCommandFence ReadbackFence;
		ReadbackFence.BeginFence();
		ReadbackFence.Wait(true);
	}

	Result.OccupiedVoxelCount = static_cast<int32>(
		FMath::Min<uint32>(Payload->OccupiedVoxelCount, MAX_int32));
	Result.OccupiedVoxelIndices = MoveTemp(Payload->OccupiedVoxelIndices);
	Result.OccupiedVoxelRadii = MoveTemp(Payload->OccupiedVoxelRadii);
	Result.SkeletonVoxelIndices = MoveTemp(Payload->SkeletonVoxelIndices);
	Result.SkeletonVoxelRadii = MoveTemp(Payload->SkeletonVoxelRadii);
	Result.bSucceeded = !Result.SkeletonVoxelIndices.IsEmpty();
	Result.Report = Result.bSucceeded
		? FString::Printf(
			TEXT("GPU global voxel skeleton: %dx%dx%d, %.4f cm cells, %d occupied voxels, %d skeleton voxels, %u triangle-column intersections."),
			Dimensions.X,
			Dimensions.Y,
			Dimensions.Z,
			CellSize,
			Result.OccupiedVoxelCount,
			Result.SkeletonVoxelIndices.Num(),
			Payload->IntersectionEventCount)
		: TEXT("GPU thinning produced no skeleton voxels.");
	return Result;
}
