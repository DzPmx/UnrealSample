#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerTreeHierarchySettings.generated.h"

class UStaticMesh;

UCLASS(Transient, PrioritizeCategories = ("Mesh"), meta = (DisplayName = "Data Bake"))
class FOLIAGEBAKEREDITOR_API UFoliageBakerTreeHierarchySettings final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (DisplayName = "Source Static Mesh", ToolTip = "The single Static Mesh whose selected source LOD is analyzed and edited in Data Bake."))
	TObjectPtr<UStaticMesh> SourceStaticMesh;

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for hierarchy analysis and UV0 leaf ownership resolution."))
	int32 SourceLODIndex = 0;

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ClampMin = "800", ClampMax = "1000", DisplayName = "Voxel Resolution", ToolTip = "Cells along the longest wood bounds dimension. Used by solidification, GPU skeletonization, and CPU fallback. Higher values increase detail and memory use."))
	int32 VoxelResolution = 800;
};
