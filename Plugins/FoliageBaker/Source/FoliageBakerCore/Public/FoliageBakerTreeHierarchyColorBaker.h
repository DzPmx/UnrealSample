#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UStaticMesh;

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewCylinder
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	double Radius = 1.0;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewJoint
{
	FVector Position = FVector::ZeroVector;
	double Radius = 1.0;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewBranch
{
	FLinearColor Color = FLinearColor::White;
	FString Label;
	FVector LabelPosition = FVector::ZeroVector;
	TArray<FFoliageBakerTreeHierarchyPreviewCylinder> Cylinders;
	TArray<FFoliageBakerTreeHierarchyPreviewJoint> Joints;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyPreviewData
{
	FString AssetName;
	FBox Bounds = FBox(EForceInit::ForceInit);
	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;
	int32 SourceLODIndex = 0;
	TArray<FFoliageBakerTreeHierarchyPreviewBranch> Branches;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyColorBakeResult
{
	bool bSucceeded = false;
	bool bCancelled = false;
	int32 BranchCount = 0;
	TSharedPtr<FFoliageBakerTreeHierarchyPreviewData> PreviewData;
	TArray<TStrongObjectPtr<UObject>> CreatedAssets;
	FString Report;
};

class FOLIAGEBAKERCORE_API FFoliageBakerTreeHierarchyColorBaker final
{
public:
	static FFoliageBakerTreeHierarchyColorBakeResult Bake(
		UStaticMesh& StaticMesh,
		int32 SourceLODIndex);
};
