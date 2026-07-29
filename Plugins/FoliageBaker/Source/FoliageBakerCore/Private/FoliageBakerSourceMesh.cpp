#include "FoliageBakerSourceMesh.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

bool FFoliageBakerSourceMeshReader::Read(
	const UStaticMesh& StaticMesh,
	const int32 SourceLODIndex,
	const bool bOverrideBakeStaticSwitch,
	const TConstArrayView<FFoliageBakerBakeStaticSwitchOverride> BakeStaticSwitchOverrides,
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

	TSet<int32> ReferencedMaterialSet;
	for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle :
		OutData.Triangles)
	{
		ReferencedMaterialSet.Add(Triangle.MaterialIndex);
	}
	TArray<int32> ReferencedMaterialIndices = ReferencedMaterialSet.Array();
	ReferencedMaterialIndices.Sort();
	if (!OutData.BakeMaterialOverrides.Build(
			StaticMesh,
			ReferencedMaterialIndices,
			bOverrideBakeStaticSwitch,
			BakeStaticSwitchOverrides,
			OutError))
	{
		return false;
	}

	FFoliageBakerFixedFrameWPOResult FixedFrameWPO;
	if (!FFoliageBakerMaskedMaterialBaker::EvaluateFixedFrameWorldPositionOffset(
			StaticMesh,
			OutData.SourceLODBounds,
			OutData.Triangles,
			OutData.BakeMaterialOverrides,
			FixedFrameWPO,
			&OutError))
	{
		return false;
	}
	OutData.FixedFrameWPOTriangles = MoveTemp(FixedFrameWPO.Triangles);
	OutData.FixedFrameWPOBounds = FixedFrameWPO.Bounds;

	const int32 EvaluatedVertexCount = OutData.FixedFrameWPOTriangles.Num() * 3;
	double MaximumDisplacement = 0.0;
	for (int32 TriangleIndex = 0;
		TriangleIndex < OutData.FixedFrameWPOTriangles.Num();
		++TriangleIndex)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			MaximumDisplacement = FMath::Max(
				MaximumDisplacement,
				FVector::Distance(
					OutData.Triangles[TriangleIndex].Vertices[Corner],
					OutData.FixedFrameWPOTriangles[TriangleIndex].Vertices[Corner]));
		}
	}
	OutData.WorldPositionOffsetStats.EvaluatedVertexCount =
		EvaluatedVertexCount;
	OutData.WorldPositionOffsetStats.MaximumDisplacement =
		MaximumDisplacement;
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
