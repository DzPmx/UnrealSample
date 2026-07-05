#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
struct FMeshDescription;

namespace UE::BillboardClouds
{
	struct FSourceTriangle
	{
		FVector Vertices[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		FVector2f UVs[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FVector Normal = FVector::UpVector;
		FVector ShadingNormal = FVector::UpVector;
		double Area = 0.0;
		int32 MaterialIndex = INDEX_NONE;
		bool bHasUVs = false;
		bool bHasSourceShadingNormal = false;
	};

	struct FCandidatePlane
	{
		FVector Normal = FVector::UpVector;
		double Rho = 0.0;
		double EstimatedDensity = 0.0;
	};

	struct FPlaneCoverPlane
	{
		FVector Normal = FVector::UpVector;
		double Rho = 0.0;
		double Score = 0.0;
		double CoveredArea = 0.0;
		TArray<int32> TriangleIndices;
	};

	struct FPlaneCoverSettings
	{
		double ErrorTolerance = 1.0;
		int32 NormalThetaSteps = 16;
		int32 NormalPhiSteps = 9;
		int32 RhoBinCount = 256;
		int32 AdaptiveRefinementDepth = 10;
		int32 TextureTileResolution = 128;
		int32 TextureTilePaddingPixels = 2;
		int32 TextureAtlasMaxResolution = 4096;
	};

	struct FPlaneCoverResult
	{
		int32 SourceTriangleCount = 0;
		int32 CandidatePlaneCount = 0;
		int32 MaxIterationCandidatePlaneCount = 0;
		int32 TotalCandidatePlaneCount = 0;
		int32 GreedyIterationCount = 0;
		int32 CoveredTriangleCount = 0;
		double SourceArea = 0.0;
		double CoveredArea = 0.0;
		double TotalSeconds = 0.0;
		double DensityBuildSeconds = 0.0;
		double CandidateSearchSeconds = 0.0;
		double CandidatePlaneBuildSeconds = 0.0;
		double CandidatePrepareSeconds = 0.0;
		double DensityUpdateSeconds = 0.0;
		TArray<FPlaneCoverPlane> Planes;
	};

	struct FPlaneProxyMeshStats
	{
		int32 PlaneCount = 0;
		int32 QuadCount = 0;
		int32 TriangleCount = 0;
		int32 AtlasWidth = 0;
		int32 AtlasHeight = 0;
		int32 AtlasTileResolution = 0;
		int32 AtlasTilePaddingPixels = 0;
		int32 SourceTriangleCount = 0;
		int32 SourceShadingNormalTriangleCount = 0;
		double AveragePlaneToShadingNormalDot = 1.0;
		double AveragePlaneToShadingNormalAngleDegrees = 0.0;
	};

	struct FPlaneProxyPlaneInfo
	{
		int32 SourcePlaneIndex = INDEX_NONE;
		FVector Normal = FVector::UpVector;
		double Rho = 0.0;
		FVector AxisU = FVector::RightVector;
		FVector AxisV = FVector::UpVector;
		FVector ShadingNormal = FVector::UpVector;
		double MinU = 0.0;
		double MaxU = 0.0;
		double MinV = 0.0;
		double MaxV = 0.0;
		FVector Corners[4] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		FVector2f AtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FIntPoint AtlasPixelMin = FIntPoint::ZeroValue;
		FIntPoint AtlasTileSize = FIntPoint::ZeroValue;
		int32 AtlasTileResolution = 0;
		int32 AtlasTilePaddingPixels = 0;
		TArray<int32> TriangleIndices;
	};

	bool ExtractTrianglesFromStaticMesh(const UStaticMesh* StaticMesh, int32 LODIndex, TArray<FSourceTriangle>& OutTriangles, FString& OutError);
	FPlaneCoverResult BuildGreedyPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings);
	bool BuildPlaneProxyMeshDescription(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverResult& Result, const FPlaneCoverSettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& OutStats, FString& OutError, TArray<FPlaneProxyPlaneInfo>* OutPlaneInfos = nullptr);
	FString SummarizePlaneCover(const FString& MeshName, const FPlaneCoverSettings& Settings, const FPlaneCoverResult& Result);
}
