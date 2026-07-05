#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"

namespace UE::FoliageBaker::BillboardClouds
{
	struct FKMeansPlaneCoverSettings
	{
		int32 PlaneCount = 150;
		int32 MaxIterations = 64;
	};

	struct FKMeansPlaneCoverResult : PlaneCover::FPlaneProxySet
	{
		int32 IterationCount = 0;
		double TotalSeconds = 0.0;
		double InitializationSeconds = 0.0;
		double AssignmentSeconds = 0.0;
		double PlaneRefitSeconds = 0.0;
	};

	FKMeansPlaneCoverResult BuildKMeansPlaneCover(
		const TArray<PlaneCover::FSourceTriangle>& Triangles,
		const FKMeansPlaneCoverSettings& Settings);

	FString SummarizeKMeansPlaneCover(
		const FString& MeshName,
		const FKMeansPlaneCoverSettings& KMeansSettings,
		const PlaneCover::FPlaneProxySettings& ProxySettings,
		const FKMeansPlaneCoverResult& Result);
}
