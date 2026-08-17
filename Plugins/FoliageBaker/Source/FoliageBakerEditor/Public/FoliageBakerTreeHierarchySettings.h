#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerTreeHierarchySettings.generated.h"

class UStaticMesh;

UCLASS(Transient, PrioritizeCategories = ("Mesh"), meta = (DisplayName = "Tree Hierarchy Data Bake"))
class FOLIAGEBAKEREDITOR_API UFoliageBakerTreeHierarchySettings final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (DisplayName = "Source Static Mesh", ToolTip = "The single Static Mesh whose selected source LOD is analyzed and edited in Data Bake."))
	TObjectPtr<UStaticMesh> SourceStaticMesh;

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for hierarchy analysis and UV0 leaf ownership resolution."))
	int32 SourceLODIndex = 0;
};
