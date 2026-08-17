#pragma once

#include "CoreMinimal.h"

struct FFoliageBakerTreeSkeletonTriangle
{
	int32 SourceComponentID = INDEX_NONE;
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;
	FVector C = FVector::ZeroVector;
};

enum class EFoliageBakerTreeSkeletonNodeKind : uint8
{
	Root,
	Fork,
	Tip
};

struct FFoliageBakerTreeSkeletonNode
{
	int32 NodeID = INDEX_NONE;
	int32 ParentNodeID = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
	double Radius = 1.0;
	EFoliageBakerTreeSkeletonNodeKind Kind = EFoliageBakerTreeSkeletonNodeKind::Tip;
};

struct FFoliageBakerTreeSkeletonEdge
{
	int32 EdgeID = INDEX_NONE;
	int32 StartNodeID = INDEX_NONE;
	int32 EndNodeID = INDEX_NONE;
	int32 BranchID = INDEX_NONE;
	int32 ParentBranchID = INDEX_NONE;
	bool bTrunk = false;
	double Radius = 1.0;
	TArray<FVector> Polyline;
};

struct FFoliageBakerTreeSkeletonResult
{
	bool bSucceeded = false;
	int32 RootNodeID = INDEX_NONE;
	int32 OccupiedVoxelCount = 0;
	int32 OccupiedComponentCount = 0;
	int32 ConnectedComponentCount = 0;
	int32 ConnectedInteriorVoxelCount = 0;
	int32 UncoveredWoodTriangleCount = 0;
	double CellSize = 0.0;
	double MaximumWoodCoverageRatio = 0.0;
	TArray<FFoliageBakerTreeSkeletonNode> Nodes;
	TArray<FFoliageBakerTreeSkeletonEdge> Edges;
	FString Report;
};

class FFoliageBakerTreeSkeleton final
{
public:
	static FFoliageBakerTreeSkeletonResult Build(
		const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
		const FVector& Pivot);
};
