#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerPlaneCover.h"

class UStaticMesh;

struct FOLIAGEBAKERCORE_API FFoliageBakerWorldPositionOffsetStats
{
	int32 EvaluatedVertexCount = 0;
	int32 NonFiniteCulledTriangleCount = 0;
	double MaximumDisplacement = 0.0;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerSourceMeshData
{
	int32 SourceLODIndex = INDEX_NONE;
	// Original source geometry used to evaluate the source material exactly once.
	FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
	TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
	// Geometry after evaluating WPO with Time and RealTime fixed at zero.
	FBoxSphereBounds FixedFrameWPOBounds = FBoxSphereBounds(ForceInitToZero);
	TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> FixedFrameWPOTriangles;
	FFoliageBakerBakeMaterialOverrideSet BakeMaterialOverrides;
	FFoliageBakerWorldPositionOffsetStats WorldPositionOffsetStats;
};

class FOLIAGEBAKERCORE_API FFoliageBakerSourceMeshReader final
{
public:
	static bool ValidateSourceLOD(
		const UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		FString& OutError);

	static bool Read(
		const UStaticMesh& StaticMesh,
		int32 SourceLODIndex,
		bool bOverrideBakeStaticSwitch,
		TConstArrayView<FFoliageBakerBakeStaticSwitchOverride> BakeStaticSwitchOverrides,
		FFoliageBakerSourceMeshData& OutData,
		FString& OutError);

	static bool ComputeBounds(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		FBoxSphereBounds& OutBounds);
};
