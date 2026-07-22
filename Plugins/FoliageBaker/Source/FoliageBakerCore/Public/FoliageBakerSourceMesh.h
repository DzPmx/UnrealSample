#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

class UStaticMesh;

struct FOLIAGEBAKERCORE_API FFoliageBakerSourceMeshData
{
	int32 SourceLODIndex = INDEX_NONE;
	FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
	TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
};

class FOLIAGEBAKERCORE_API FFoliageBakerSourceMeshReader final
{
public:
	static bool Read(
		const UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		FFoliageBakerSourceMeshData& OutData,
		FString& OutError);

	static bool ComputeBounds(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		FBoxSphereBounds& OutBounds);
};
