#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerPlaneCover.h"
#include "MaterialBakingStructures.h"
#include "MeshDescription.h"
#include "UObject/StrongObjectPtr.h"

#include "FoliageBakerMaskedMaterialBaker.generated.h"

class UMaterialInterface;
class UStaticMesh;

USTRUCT()
struct FOLIAGEBAKERCORE_API FFoliageBakerBakeStaticSwitchOverride
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Static Switch Override", meta = (DisplayName = "Parameter", ToolTip = "Name of a Global static switch parameter to override on selected-LOD source materials during Bake."))
	FName ParameterName = TEXT("EnableBake");

	UPROPERTY(EditAnywhere, Category = "Static Switch Override", meta = (DisplayName = "Value", ToolTip = "Temporary value used by this source material branch during Bake."))
	bool bValue = true;
};

/**
 * Owns transient child material instances used to override Global static
 * switches without modifying any source material asset.
 */
class FOLIAGEBAKERCORE_API FFoliageBakerBakeMaterialOverrideSet final
{
public:
	bool Build(
		const UStaticMesh& SourceStaticMesh,
		TConstArrayView<int32> ReferencedMaterialIndices,
		bool bEnableStaticSwitchOverrides,
		TConstArrayView<FFoliageBakerBakeStaticSwitchOverride> InStaticSwitchOverrides,
		FString& OutError);

	TStrongObjectPtr<UMaterialInterface> ResolveMaterial(int32 MaterialIndex) const;
	FString BuildReportDetails() const;

private:
	TMap<int32, TStrongObjectPtr<UMaterialInterface>> OverriddenMaterialsByIndex;
	bool bEnabled = false;
	TArray<FFoliageBakerBakeStaticSwitchOverride> StaticSwitchOverrides;
	int32 ReferencedMaterialCount = 0;
	int32 OverriddenMaterialCount = 0;
	int32 AppliedStaticSwitchOverrideCount = 0;
	TMap<FName, TArray<FString>> MissingParameterMaterialNames;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileMaterialInput
{
	FFoliageBakerDepthCorrectTileMaterialInput() = default;
	FFoliageBakerDepthCorrectTileMaterialInput(
		const FFoliageBakerDepthCorrectTileMaterialInput&) = delete;
	FFoliageBakerDepthCorrectTileMaterialInput& operator=(
		const FFoliageBakerDepthCorrectTileMaterialInput&) = delete;
	FFoliageBakerDepthCorrectTileMaterialInput(
		FFoliageBakerDepthCorrectTileMaterialInput&&) = default;
	FFoliageBakerDepthCorrectTileMaterialInput& operator=(
		FFoliageBakerDepthCorrectTileMaterialInput&&) = default;

	TStrongObjectPtr<UMaterialInterface> MaterialInterface;
	TUniquePtr<FMeshDescription> MeshDescription;
	FMeshData MeshSettings;
	TArray<int32> RasterSourceTriangleIndices;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileRequest
{
	FFoliageBakerDepthCorrectTileRequest() = default;
	FFoliageBakerDepthCorrectTileRequest(
		const FFoliageBakerDepthCorrectTileRequest&) = delete;
	FFoliageBakerDepthCorrectTileRequest& operator=(
		const FFoliageBakerDepthCorrectTileRequest&) = delete;
	FFoliageBakerDepthCorrectTileRequest(
		FFoliageBakerDepthCorrectTileRequest&&) = default;
	FFoliageBakerDepthCorrectTileRequest& operator=(
		FFoliageBakerDepthCorrectTileRequest&&) = default;

	FIntPoint TextureSize = FIntPoint::ZeroValue;
	FVector CaptureRayDirection = FVector::ZeroVector;
	FVector ProjectionAxisU = FVector::ZeroVector;
	FVector ProjectionAxisV = FVector::ZeroVector;
	double ProjectionMinU = 0.0;
	double ProjectionMaxU = 0.0;
	double ProjectionMinV = 0.0;
	double ProjectionMaxV = 0.0;
	FBoxSphereBounds SourceBounds = FBoxSphereBounds(ForceInitToZero);
	TArray<FFoliageBakerDepthCorrectTileMaterialInput> Materials;
	bool bFlipProjectionV = false;
	bool bBakeBaseColor = true;
	bool bBakeObjectSpaceNormal = true;
	bool bBakePackedMix = false;
	bool bBakeRoughnessSpecular = false;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerDepthCorrectTileResult
{
	TArray<FColor> BaseColor;
	TArray<FColor> ObjectSpaceNormal;
	TArray<FColor> PackedMix;
	TArray<FColor> Roughness;
	TArray<FColor> Specular;
	TArray<FColor> SourceTriangleIdAndDepth;
};

struct FOLIAGEBAKERCORE_API FFoliageBakerFixedFrameWPOResult
{
	FBoxSphereBounds Bounds = FBoxSphereBounds(ForceInitToZero);
	TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
	// Source triangle indices retained after treating non-finite WPO positions
	// as triangle-level geometry cuts. Kept parallel with Triangles.
	TArray<int32> RetainedSourceTriangleIndices;
};

/**
 * Renders projected material data through the source material's real masked
 * base-pass path. The output modes therefore use the real OpacityMask graph,
 * OpacityMaskClipValue, EarlyOpacityMask custom output, two-sided facing and
 * World Position Offset. Bake GameTime and RealTime are fixed at zero, while
 * material Displacement remains disabled.
 *
 * This intentionally lives in FoliageBakerCore so all proxy generators can
 * share it without modifying Engine MaterialBaking sources.
 */
class FOLIAGEBAKERCORE_API FFoliageBakerMaskedMaterialBaker final
{
public:
	/**
	 * Evaluates the selected-LOD vertices through the same source-material WPO
	 * path used by Bake, with GameTime and RealTime fixed at zero. The returned
	 * triangle positions are for capture bounds/projection only; formal material
	 * Bake must continue to submit the original positions so WPO is applied once.
	 * A triangle with any non-finite WPO vertex is omitted from both inputs via
	 * RetainedSourceTriangleIndices, matching GPU primitive rejection semantics.
	 */
	static bool EvaluateFixedFrameWorldPositionOffset(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& PrimitiveBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
		FFoliageBakerFixedFrameWPOResult& OutResult,
		FString& OutError);

	static bool BakeDepthCorrectTile(
		const FFoliageBakerDepthCorrectTileRequest& Request,
		FFoliageBakerDepthCorrectTileResult& OutResult,
		FString& OutError);

	/** Returns INDEX_NONE for the reserved uncovered value or malformed data. */
	static int32 DecodeSourceTriangleId(const FColor& EncodedTriangleId);
};
