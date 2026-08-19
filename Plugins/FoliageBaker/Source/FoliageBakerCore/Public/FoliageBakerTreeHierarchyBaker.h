#pragma once

#include "CoreMinimal.h"
class UStaticMesh;

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewCylinder
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	double Radius = 1.0;
};

enum class EFoliageBakerTreeHierarchyPreviewNodeKind : uint8
{
	Root,
	Fork,
	Tip
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewNode
{
	int32 NodeID = INDEX_NONE;
	int32 ParentNodeID = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
	double Radius = 1.0;
	EFoliageBakerTreeHierarchyPreviewNodeKind Kind =
		EFoliageBakerTreeHierarchyPreviewNodeKind::Tip;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewEdge
{
	int32 EdgeID = INDEX_NONE;
	int32 StartNodeID = INDEX_NONE;
	int32 EndNodeID = INDEX_NONE;
	int32 BranchID = INDEX_NONE;
	int32 ParentBranchID = INDEX_NONE;
	bool bTrunk = false;
	TArray<FVector> Polyline;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewJoint
{
	FVector Position = FVector::ZeroVector;
	double Radius = 1.0;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyBoneRecord
{
	int32 BoneID = INDEX_NONE;
	int32 ParentBoneID = INDEX_NONE;
	FVector PivotPosition = FVector::ZeroVector;
	FVector Axis = FVector::ZeroVector;
	double MinimumAxisProjection = 0.0;
	double PositiveAxisExtent = 0.0;
	double AxisExtent = 0.0;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewBranch
{
	int32 BranchID = INDEX_NONE;
	int32 ParentBranchID = INDEX_NONE;
	FFoliageBakerTreeHierarchyBoneRecord BoneRecord;
	FVector StartPosition = FVector::ZeroVector;
	FVector EndPosition = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	FString Label;
	FVector LabelPosition = FVector::ZeroVector;
	TArray<FFoliageBakerTreeHierarchyPreviewCylinder> Cylinders;
	TArray<FFoliageBakerTreeHierarchyPreviewJoint> Joints;
	TArray<int32> SourceTriangleIDs;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerLeafTemplateAnnotation
{
	FString UVIslandSignature;
	TSet<FString> TriangleSignatures;
	FVector2f PivotUV = FVector2f::ZeroVector;
	FVector2f TipUV = FVector2f::ZeroVector;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerResolvedLeafCluster
{
	FString UVIslandSignature;
	TArray<int32> SourceTriangleIDs;
	FVector PivotPosition = FVector::ZeroVector;
	FVector TipPosition = FVector::ZeroVector;
	int32 ParentBranchID = INDEX_NONE;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewData
{
	FString AssetName;
	FBox Bounds = FBox(EForceInit::ForceInit);
	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;
	int32 SourceLODIndex = 0;
	int32 RootNodeID = INDEX_NONE;
	double SkeletonCellSize = 0.0;
	int32 WoodVolumeComponentCount = 0;
	TArray<FFoliageBakerTreeHierarchyPreviewNode> SkeletonNodes;
	TArray<FFoliageBakerTreeHierarchyPreviewEdge> SkeletonEdges;
	TArray<FFoliageBakerTreeHierarchyPreviewBranch> Branches;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyAnalysisResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	int32 BranchCount = 0;
	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> PreviewData;
	FString Report;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerLeafOwnershipResolveResult
{
	bool bSucceeded = false;
	int32 PhysicalLeafClusterCount = 0;
	int32 ResolvedLeafClusterCount = 0;
	int32 UnresolvedLeafClusterCount = 0;
	TArray<FFoliageBakerResolvedLeafCluster> ResolvedLeafClusters;
	FString Report;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerWindDataBakeResult
{
	bool bSucceeded = false;
	int32 BoneRecordCount = 0;
	int32 BranchBoneCount = 0;
	int32 LeafBoneCount = 0;
	int32 UnassignedTriangleCount = 0;
	FString PivotPositionTexturePath;
	FString PivotAxisTexturePath;
	FString Report;
};

class FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyBaker final
{
public:
	static FFoliageBakerTreeHierarchyAnalysisResult Analyze(
		UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		int32 LeafMaterialIndex);

	static FFoliageBakerLeafOwnershipResolveResult ResolveLeafOwnership(
		const UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		int32 LeafMaterialIndex,
		const FFoliageBakerTreeHierarchyPreviewData& AnalysisData,
		const TArray<FFoliageBakerLeafTemplateAnnotation>& Annotations);

	static FFoliageBakerWindDataBakeResult BakeWindData(
		UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		const FFoliageBakerTreeHierarchyPreviewData& AnalysisData,
		const TArray<FFoliageBakerResolvedLeafCluster>& ResolvedLeafClusters);
};
