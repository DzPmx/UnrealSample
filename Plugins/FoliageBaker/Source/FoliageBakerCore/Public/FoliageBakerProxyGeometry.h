#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"
#include "MeshDescription.h"

struct FOLIAGEBAKERCORE_API FFoliageBakerProxyGeometry
{
	FMeshDescription MeshDescription;
	UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats Stats;
	TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> PlaneInfos;
};
