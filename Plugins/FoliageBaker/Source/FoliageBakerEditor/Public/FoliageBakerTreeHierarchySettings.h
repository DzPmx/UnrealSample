#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "FoliageBakerTreeHierarchySettings.generated.h"

class UStaticMesh;

UCLASS(Transient, PrioritizeCategories = ("Mesh"), meta = (DisplayName = "Tree Hierarchy Test Colors"))
class FOLIAGEBAKEREDITOR_API UFoliageBakerTreeHierarchySettings final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ToolTip = "Static Mesh assets whose selected source LOD receives tree hierarchy test vertex colors."))
	TArray<TObjectPtr<UStaticMesh>> SourceStaticMeshes;

	UPROPERTY(Transient, EditAnywhere, Category = "Mesh", meta = (ClampMin = "0", ClampMax = "7", DisplayName = "Source LOD Index", ToolTip = "Source Static Mesh LOD used for tree hierarchy recognition and vertex-color output."))
	int32 SourceLODIndex = 0;
};
