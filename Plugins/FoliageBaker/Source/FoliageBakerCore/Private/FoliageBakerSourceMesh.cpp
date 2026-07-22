#include "FoliageBakerSourceMesh.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

bool FFoliageBakerSourceMeshReader::Read(
	const UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	FFoliageBakerSourceMeshData& OutData,
	FString& OutError)
{
	OutData = FFoliageBakerSourceMeshData();
	OutData.SourceLODIndex = SourceLODIndex;
	if (SourceLODIndex < 0 || SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		OutError = FString::Printf(
			TEXT("Source LOD index %d is outside the supported range 0-%d."),
			SourceLODIndex,
			MAX_STATIC_MESH_LODS - 1);
		return false;
	}
	if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			SourceLODIndex,
			OutData.Triangles,
			OutError))
	{
		return false;
	}
	if (!ComputeBounds(OutData.Triangles, OutData.SourceLODBounds))
	{
		OutError = FString::Printf(
			TEXT("Source LOD %d has no valid bounds."),
			SourceLODIndex);
		return false;
	}
	return true;
}

bool FFoliageBakerSourceMeshReader::ComputeBounds(
	const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
	FBoxSphereBounds& OutBounds)
{
	FBox Bounds(ForceInit);
	for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
	{
		for (const FVector& Vertex : Triangle.Vertices)
		{
			Bounds += Vertex;
		}
	}
	if (!Bounds.IsValid)
	{
		OutBounds = FBoxSphereBounds(ForceInitToZero);
		return false;
	}

	OutBounds = FBoxSphereBounds(Bounds);
	return true;
}
