#pragma once

#include "CoreMinimal.h"

struct FFoliageBakerTreeSkeletonTriangle
{
	int32 SourceTriangleID = INDEX_NONE;
	int32 SourceComponentID = INDEX_NONE;
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;
	FVector C = FVector::ZeroVector;
};

struct FFoliageBakerTreeSkeletonGuide
{
	int32 SourceComponentID = INDEX_NONE;
	bool bRooted = false;
	double Radius = 1.0;
	TStaticArray<double, 2> EndpointRadii{1.0, 1.0};
	TArray<FVector> Polyline;
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
	int32 RecoveredGuideCount = 0;
	int32 UnresolvedGuideCount = 0;
	int32 UncoveredGuideTerminalCount = 0;
	int32 UncoveredGuideEndpointCount = 0;
	int32 UncoveredWoodTriangleCount = 0;
	double CellSize = 0.0;
	double MaximumWoodCoverageRatio = 0.0;
	TArray<FFoliageBakerTreeSkeletonNode> Nodes;
	TArray<FFoliageBakerTreeSkeletonEdge> Edges;
	TArray<int32> UnresolvedGuideSourceComponentIDs;
	TArray<int32> UncoveredWoodSourceTriangleIDs;
	TArray<FString> UnresolvedGuideDiagnostics;
	FString Report;
};

class FFoliageBakerTreeSkeleton final
{
public:
	static TArray<FFoliageBakerTreeSkeletonGuide> BuildSurfaceGuides(
		const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
		int32 SourceComponentID,
		double GeometryScale,
		const FVector& Pivot);

	static FFoliageBakerTreeSkeletonResult Build(
		const TArray<FFoliageBakerTreeSkeletonTriangle>& Triangles,
		const TArray<FFoliageBakerTreeSkeletonGuide>& Guides,
		const FVector& Pivot);

};
