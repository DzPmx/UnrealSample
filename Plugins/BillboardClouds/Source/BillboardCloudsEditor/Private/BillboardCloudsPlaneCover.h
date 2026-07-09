#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
struct FMeshDescription;

namespace UE::BillboardClouds
{
	enum class EPlaneCoverTechnique : uint8
	{
		PlaneSpaceGreedy,
		KMeansClustering,
		GodOfWarCards
	};

	enum class EKMeansCrackReductionMode : uint8
	{
		Off,
		PaperExact,
		BoundaryAware
	};

	enum class EDoubleSidedBakeMode : uint8
	{
		Off,
		TrunkCardsOnly,
		BillboardPlanesOnly,
		AllPlanes
	};

	struct FSourceTriangle
	{
		FVector Vertices[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		FVector2f UVs[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FVector VertexNormals[3] = { FVector::UpVector, FVector::UpVector, FVector::UpVector };
		FVector Normal = FVector::UpVector;
		FVector ShadingNormal = FVector::UpVector;
		double Area = 0.0;
		int32 MaterialIndex = INDEX_NONE;
		bool bHasUVs = false;
		bool bHasSourceShadingNormal = false;
		bool bTrunkCardOnly = false;
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
		bool bIsTrunkCard = false;
		bool bUseFixedPlaneFrame = false;
		FVector FixedAxisU = FVector::RightVector;
		FVector FixedAxisV = FVector::UpVector;
	};

	struct FPlaneCoverSettings
	{
		EPlaneCoverTechnique Technique = EPlaneCoverTechnique::PlaneSpaceGreedy;
		double ErrorTolerance = 1.0;
		double TextureCompactnessWeight = 0.25;
		int32 KMeansPlaneCount = 150;
		int32 KMeansMaxIterations = 64;
		EKMeansCrackReductionMode KMeansCrackReductionMode = EKMeansCrackReductionMode::Off;
		double KMeansBoundaryCrackReductionWidth = 8.0;
		int32 GodOfWarGeodesicSubdivisions = 2;
		double GodOfWarCandidateSpacingMultiplier = 1.0;
		int32 NormalThetaSteps = 16;
		int32 NormalPhiSteps = 9;
		int32 RhoBinCount = 256;
		int32 AdaptiveRefinementDepth = 10;
		int32 TextureTilePaddingPixels = 2;
		int32 TextureAtlasResolution = 4096;
		int32 SourceMaterialBakeResolution = 2048;
		EDoubleSidedBakeMode DoubleSidedBakeMode = EDoubleSidedBakeMode::Off;
		bool bBoostTrunkCardAtlasResolution = true;
		bool bEnableAlphaAwareTileCrop = false;
		int32 AlphaAwareTileCropGuardPixels = 2;
	};

	struct FPlaneCoverResult
	{
		int32 SourceTriangleCount = 0;
		int32 CandidatePlaneCount = 0;
		int32 MaxIterationCandidatePlaneCount = 0;
		int32 TotalCandidatePlaneCount = 0;
		int32 GreedyIterationCount = 0;
		int32 CoveredTriangleCount = 0;
		int32 FinalReassignedTriangleCount = 0;
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

	struct FCrackReductionProjection
	{
		int32 TriangleIndex = INDEX_NONE;
		int32 SourcePlaneInfoIndex = INDEX_NONE;
		bool bBoundaryAware = false;
	};

	struct FPlaneProxyPlaneInfo
	{
		int32 SourcePlaneIndex = INDEX_NONE;
		bool bIsTrunkCard = false;
		FVector Normal = FVector::UpVector;
		double Rho = 0.0;
		FVector AxisU = FVector::RightVector;
		FVector AxisV = FVector::UpVector;
		FVector ShadingNormal = FVector::UpVector;
		double MinU = 0.0;
		double MaxU = 0.0;
		double MinV = 0.0;
		double MaxV = 0.0;
		double MinSignedDistance = 0.0;
		double MaxSignedDistance = 0.0;
		double EnvelopeMinU = 0.0;
		double EnvelopeMaxU = 0.0;
		double EnvelopeMinV = 0.0;
		double EnvelopeMaxV = 0.0;
		double EnvelopeMinSignedDistance = 0.0;
		double EnvelopeMaxSignedDistance = 0.0;
		FVector Corners[4] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		FVector2f AtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FVector2f BackAtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FIntPoint AtlasPixelMin = FIntPoint::ZeroValue;
		FIntPoint AtlasTileSize = FIntPoint::ZeroValue;
		FIntPoint BackAtlasPixelMin = FIntPoint::ZeroValue;
		FIntPoint BackAtlasTileSize = FIntPoint::ZeroValue;
		int32 AtlasTileResolution = 0;
		int32 AtlasTilePaddingPixels = 0;
		bool bHasBackFaceAtlas = false;
		TArray<int32> TriangleIndices;
		TArray<FCrackReductionProjection> CrackReductionProjections;
	};

	struct FPlaneProxyTileCrop
	{
		bool bEnabled = false;
		double MinUFraction = 0.0;
		double MaxUFraction = 1.0;
		double MinVFraction = 0.0;
		double MaxVFraction = 1.0;
	};

	bool ExtractTrianglesFromStaticMesh(const UStaticMesh* StaticMesh, int32 LODIndex, TArray<FSourceTriangle>& OutTriangles, FString& OutError);
	FVector ProjectPointToPlane(const FVector& Point, const FVector& PlaneNormal, double PlaneRho);
	bool IsPointWithinPlaneError(const FVector& Point, const FVector& PlaneNormal, double PlaneRho, const FPlaneCoverSettings& Settings);
	bool IsTriangleValidForPlane(const FSourceTriangle& Triangle, const FVector& PlaneNormal, double PlaneRho, const FPlaneCoverSettings& Settings);
	bool DoesTriangleIntersectPlaneValidZone(const FSourceTriangle& Triangle, const FVector& PlaneNormal, double PlaneRho, const FPlaneCoverSettings& Settings);
	FPlaneCoverResult BuildGreedyPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings);
	FPlaneCoverResult BuildKMeansPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings);
	FPlaneCoverResult BuildPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings);
	bool BuildPlaneProxyMeshDescription(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverResult& Result, const FPlaneCoverSettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& OutStats, FString& OutError, TArray<FPlaneProxyPlaneInfo>* OutPlaneInfos = nullptr);
	bool ApplyPlaneProxyTileCropsAndRebuildMeshDescription(TArray<FPlaneProxyPlaneInfo>& PlaneInfos, const TArray<FPlaneProxyTileCrop>& TileCrops, const FPlaneCoverSettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& InOutStats, FString& OutError);
	FString SummarizePlaneCover(const FString& MeshName, const FPlaneCoverSettings& Settings, const FPlaneCoverResult& Result);
}
