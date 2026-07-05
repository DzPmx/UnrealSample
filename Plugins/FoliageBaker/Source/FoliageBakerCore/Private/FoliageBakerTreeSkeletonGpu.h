#pragma once

#include "CoreMinimal.h"

struct FFoliageBakerTreeSkeletonTriangle;

struct FFoliageBakerGpuTreeSkeletonResult
{
	bool bSucceeded = false;
	FIntVector Dimensions = FIntVector::ZeroValue;
	FVector GridOrigin = FVector::ZeroVector;
	double CellSize = 0.0;
	int32 OccupiedVoxelCount = 0;
	TArray<int32> OccupiedVoxelIndices;
	TArray<float> OccupiedVoxelRadii;
	TArray<int32> SkeletonVoxelIndices;
	TArray<float> SkeletonVoxelRadii;
	FString Report;
};

class FFoliageBakerTreeSkeletonGpu final
{
public:
	static FFoliageBakerGpuTreeSkeletonResult Build(
		const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
		const FBox& SourceBounds,
		const FVector& Pivot,
		int32 TargetResolution);
};
