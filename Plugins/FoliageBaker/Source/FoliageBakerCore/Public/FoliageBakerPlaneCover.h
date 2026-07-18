#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
struct FMeshDescription;

namespace UE::FoliageBaker::PlaneCover
{
	constexpr int32 MaxMaterialBakeUVChannels = 6;

	enum class EPlaneProxyCrackReductionMode : uint8
	{
		Off,
		ScaledEnvelopeClip
	};

	enum class EDoubleSidedBakeMode : uint8
	{
		Off,
		TrunkCardsOnly,
		BillboardPlanesOnly,
		AllPlanes
	};


	enum class EAtlasVConvention : uint8
	{

		GeometryMinVToTextureMinV,


		GeometryMinVToTextureMaxV
	};

	struct FOLIAGEBAKERCORE_API FSourceTriangle
	{
		FVector Vertices[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		FVector2f UVs[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
		FVector2f UVChannels[MaxMaterialBakeUVChannels][3] = {};
		FVector VertexNormals[3] = { FVector::UpVector, FVector::UpVector, FVector::UpVector };
		FVector VertexTangents[3] = { FVector::ForwardVector, FVector::ForwardVector, FVector::ForwardVector };
		FVector4f VertexColors[3] =
		{
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f),
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f),
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f)
		};
		float BinormalSigns[3] = { 1.0f, 1.0f, 1.0f };
		FVector Normal = FVector::UpVector;
		FVector ShadingNormal = FVector::UpVector;
		double Area = 0.0;
		int32 MaterialIndex = INDEX_NONE;
		int32 NumUVChannels = 0;
		bool bHasUVs = false;
		bool bHasSourceShadingNormal = false;
		bool bHasTangents = false;
		bool bHasVertexColors = false;
		bool bIsTrunk = false;
		bool bTrunkCardOnly = false;
	};

	struct FOLIAGEBAKERCORE_API FPlaneProxyInput
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

	struct FOLIAGEBAKERCORE_API FPlaneProxySettings
	{
		double ErrorTolerance = 1.0;
		EPlaneProxyCrackReductionMode CrackReductionMode = EPlaneProxyCrackReductionMode::Off;
		double CrackReductionProjectionScale = 1.0;
		int32 TextureAtlasResolution = 4096;
		EDoubleSidedBakeMode DoubleSidedBakeMode = EDoubleSidedBakeMode::Off;
		bool bEmitBackFaceGeometry = false;
		EAtlasVConvention AtlasVConvention = EAtlasVConvention::GeometryMinVToTextureMinV;
		double TrunkCardAtlasScale = 1.0;
		bool bEnableAlphaAwareTileCrop = false;
		int32 AlphaAwareTileCropGuardPixels = 2;
	};

	struct FOLIAGEBAKERCORE_API FPlaneProxySet
	{
		int32 SourceTriangleCount = 0;
		int32 CoveredTriangleCount = 0;
		double SourceArea = 0.0;
		double CoveredArea = 0.0;
		TArray<FPlaneProxyInput> Planes;
	};

	struct FOLIAGEBAKERCORE_API FPlaneProxyMeshStats
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

	struct FOLIAGEBAKERCORE_API FCrackReductionProjection
	{
		int32 TriangleIndex = INDEX_NONE;
		int32 SourcePlaneInfoIndex = INDEX_NONE;
		TArray<FVector> ClippedPolygon;
	};

	struct FOLIAGEBAKERCORE_API FPlaneProxyPlaneInfo
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

	struct FOLIAGEBAKERCORE_API FPlaneProxyTileCrop
	{
		bool bEnabled = false;
		double MinUFraction = 0.0;
		double MaxUFraction = 1.0;
		double MinVFraction = 0.0;
		double MaxVFraction = 1.0;
	};

	FOLIAGEBAKERCORE_API bool ExtractTrianglesFromStaticMesh(const UStaticMesh* StaticMesh, int32 LODIndex, TArray<FSourceTriangle>& OutTriangles, FString& OutError);
	FOLIAGEBAKERCORE_API FVector ProjectPointToPlane(const FVector& Point, const FVector& PlaneNormal, double PlaneRho);
	FOLIAGEBAKERCORE_API bool BuildPlaneProxyMeshDescription(const TArray<FSourceTriangle>& Triangles, const FPlaneProxySet& Result, const FPlaneProxySettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& OutStats, FString& OutError, TArray<FPlaneProxyPlaneInfo>* OutPlaneInfos = nullptr);
	FOLIAGEBAKERCORE_API bool ApplyPlaneProxyTileCropsAndRebuildMeshDescription(TArray<FPlaneProxyPlaneInfo>& PlaneInfos, const TArray<FPlaneProxyTileCrop>& TileCrops, const FPlaneProxySettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& InOutStats, FString& OutError);
}
