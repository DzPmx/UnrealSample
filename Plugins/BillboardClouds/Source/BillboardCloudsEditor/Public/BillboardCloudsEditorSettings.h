#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BillboardCloudsEditorSettings.generated.h"

UENUM()
enum class EBillboardCloudsTechnique : uint8
{
	PlaneSpaceGreedy UMETA(DisplayName = "Paper 1 - Plane-Space Greedy Cover"),
	KMeansClustering UMETA(DisplayName = "Paper 2 - K-Means Best-Fit Planes"),
	GodOfWarCards UMETA(DisplayName = "God of War - Greedy Card Capture")
};

UENUM()
enum class EBillboardCloudsCrackReductionMode : uint8
{
	Off UMETA(DisplayName = "Off"),
	PaperExact UMETA(DisplayName = "Paper Exact - Envelope Intersection"),
	BoundaryAware UMETA(DisplayName = "Boundary Aware - Envelope Boundary Band")
};

UENUM()
enum class EBillboardCloudsDoubleSidedBakeMode : uint8
{
	Off UMETA(DisplayName = "Off"),
	TrunkCardsOnly UMETA(DisplayName = "Trunk Cards Only"),
	BillboardPlanesOnly UMETA(DisplayName = "Billboard Planes Only"),
	AllPlanes UMETA(DisplayName = "All Planes")
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Billboard Clouds"))
class BILLBOARDCLOUDSEDITOR_API UBillboardCloudsEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "Technique", meta = (ToolTip = "Plane-space greedy cover follows the first paper. K-means clustering follows the improved second paper. God of War card capture greedily picks the best card until all faces are claimed."))
	EBillboardCloudsTechnique Technique = EBillboardCloudsTechnique::PlaneSpaceGreedy;

	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy || Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Relative object-space error. The analyzer multiplies this by the selected mesh bounds sphere radius. For God of War cards this is the thick-slice closeness parameter."))
	double RelativeError = 0.01;

	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy || Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Minimum object-space error in Unreal centimeters. For God of War cards this is the minimum thick-slice closeness."))
	double MinimumErrorCm = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "4", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of azimuth samples used in plane-space normal discretization."))
	int32 NormalThetaSteps = 16;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "3", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of polar samples used in plane-space normal discretization."))
	int32 NormalPhiSteps = 9;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "8", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Number of rho bins used for each sampled normal direction."))
	int32 RhoBinCount = 256;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Safety cap for the paper's recursive adaptive refinement after picking the densest plane-space bin. Refinement still stops early when the sub-bin center plane is valid for the whole sub-bin validity set."))
	int32 AdaptiveRefinementDepth = 10;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "1", ClampMax = "4096", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Target number of billboard planes for the second paper's budget-driven k-means clustering."))
	int32 KMeansPlaneCount = 150;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "1", ClampMax = "512", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Maximum assignment/refit iterations for the second paper's k-means solver."))
	int32 KMeansMaxIterations = 64;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering", EditConditionHides, ToolTip = "Crack reduction for the second paper. Paper Exact projects intersecting envelope content onto both planes. Boundary Aware limits the extra projection to an envelope-boundary band for alpha-card vegetation."))
	EBillboardCloudsCrackReductionMode KMeansCrackReductionMode = EBillboardCloudsCrackReductionMode::Off;

	UPROPERTY(config, EditAnywhere, Category = "K-Means", meta = (ClampMin = "0.0", ClampMax = "200.0", EditCondition = "Technique == EBillboardCloudsTechnique::KMeansClustering && KMeansCrackReductionMode == EBillboardCloudsCrackReductionMode::BoundaryAware", EditConditionHides, ToolTip = "Object-space width in centimeters for boundary-aware crack reduction. Only pixels near either intersecting envelope boundary are projected onto the neighbor plane."))
	double KMeansBoundaryCrackReductionWidthCm = 8.0;

	UPROPERTY(config, EditAnywhere, Category = "God of War Cards", meta = (ClampMin = "0", ClampMax = "5", EditCondition = "Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Subdivisions of the geodesic sphere used for half-sphere card directions. Higher values test more directions."))
	int32 GodOfWarGeodesicSubdivisions = 2;

	UPROPERTY(config, EditAnywhere, Category = "God of War Cards", meta = (ClampMin = "0.1", ClampMax = "8.0", EditCondition = "Technique == EBillboardCloudsTechnique::GodOfWarCards", EditConditionHides, ToolTip = "Distance spacing between candidate card planes as a multiplier of the closeness/error metric."))
	double GodOfWarCandidateSpacingMultiplier = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (ToolTip = "Route matching trunk/branch material triangles into a fixed vertical cross-card pass instead of the selected Billboard Clouds technique."))
	bool bEnableTrunkCards = true;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (ClampMin = "2", ClampMax = "4", EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Number of vertical trunk cross-card planes. 2 is a cross card, 3 is a three-way star, 4 is a four-way star."))
	int32 TrunkCardPlaneCount = 4;

	UPROPERTY(config, EditAnywhere, Category = "Trunk Cards", meta = (EditCondition = "bEnableTrunkCards", EditConditionHides, ToolTip = "Material instance or parent material name keywords used to route triangles into fixed vertical trunk cross-card planes instead of the selected Billboard Clouds technique. Empty means no triangles are routed."))
	TArray<FString> TrunkCardMaterialKeywords;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0.0", ClampMax = "8.0", EditCondition = "Technique == EBillboardCloudsTechnique::PlaneSpaceGreedy", EditConditionHides, ToolTip = "Paper Section 6.1 compactness weight. Higher values favor spatially compact validity sets and compact clusters."))
	double TextureCompactnessWeight = 0.25;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0", ClampMax = "128", ToolTip = "Transparent gutter around each billboard tile, in pixels."))
	int32 TextureTilePaddingPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "256", ClampMax = "8192", ToolTip = "Generated square atlas resolution. Billboard tile sizes are automatically scaled and packed to maximize use of this texture."))
	int32 TextureAtlasResolution = 4096;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ToolTip = "Bake a separate back-side atlas tile for selected proxy planes. The material uses TwoSidedSign to sample UV0 on front faces and UV1 on back faces."))
	EBillboardCloudsDoubleSidedBakeMode DoubleSidedBakeMode = EBillboardCloudsDoubleSidedBakeMode::Off;
};
