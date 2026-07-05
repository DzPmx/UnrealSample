#pragma once

#include "Components/SceneComponent.h"

#include "CloudSeaComponent.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;

#if WITH_EDITOR
enum ELevelViewportType : int;
#endif

UENUM(BlueprintType)
enum class ECloudSeaVerticalRenderingMode : uint8
{
	ReferenceMirrorWithPrefill UMETA(DisplayName = "Reference Mirror with Prefill"),
	SingleLayerWithoutPrefill UMETA(DisplayName = "Single Layer without Prefill")
};

/**
 * Drives disk and camera-facing cloud-sea proxies whose material ray-marches
 * one fixed world-space cloud layer.
 *
 * Attach the proxy mesh beneath this component with an identity relative transform.
 */
UCLASS(ClassGroup = (Rendering), BlueprintType, meta = (BlueprintSpawnableComponent))
class CLOUDSEA_API UCloudSeaComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UCloudSeaComponent();

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool TryGetView(
		FVector& OutViewLocation,
		FRotator& OutViewRotation,
		float& OutNearClipPlane) const;
	void InitializeMaterialDriver();
	void UpdateCloudSeaTransform();
	void UpdateMaterialParameters(
		const FVector& ViewLocation,
		bool bUseFullScreenProxy,
		double SignedCameraDepthBelowCloudTopMaterialUnits,
		float ActiveDiskRayMarchDistanceMaterialUnits);

#if WITH_EDITOR
	void HandleEditorCameraMoved(
		const FVector& ViewLocation,
		const FRotator& ViewRotation,
		ELevelViewportType ViewportType,
		int32 ViewIndex);
#endif

	TWeakObjectPtr<UStaticMeshComponent> ProxyMeshComponent;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicMaterial;

public:
	/** Local player whose camera drives this component in a game world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Camera")
	int32 CameraPlayerIndex = 0;

	/** Radius applied to the camera-facing full-screen proxy; the disk radius is derived per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Proxy Transform")
	double ProxyWorldRadius = 949966.6875;

	/** Distance in front of the camera used by the full-screen proxy plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Proxy Transform")
	double FullScreenProxyCameraDistance = 297.1015625;

	/** Fixed world-space height of the cloud layer base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|World Anchoring")
	double CloudLayerBaseWorldHeight = 62595.295533;

	/** World-units-to-material-units scale reconstructed from the RiderDuck constants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|World Anchoring")
	double WorldUnitsPerMaterialUnit = 100.0;

	/** Camera XY at which the authored volume UV offsets are the reference values. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|World Anchoring")
	FVector2D VolumeAnchorReferenceCameraXY = FVector2D::ZeroVector;

	/** Scalar material inputs that are not reconstructed from the active camera height. */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Cloud Sea|Material|Ray March",
		meta = (ClampMin = "16.0", ClampMax = "128.0", UIMin = "16.0", UIMax = "128.0"))
	float RayMarchStepCount = 64.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	float NoiseVolumeUVScale = 0.009908398613333702f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	float NoiseWarpAmplitude = 14.206369400024414f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	float DiskMarchFarDistance = 2047.33349609375f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	float SceneDepthFadeReciprocalDistance = 0.0009999999310821295f;

	/** Reference full-screen march distance at either edge of the layer transition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	float FullScreenTransitionMarchDistance = 750.0f;

	/** World-space height above the cloud top over which the disk march distance reaches its configured limit. */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Cloud Sea|Material|Ray March",
		meta = (ClampMin = "1.0", UIMin = "1.0"))
	float DiskRayMarchTransitionWorldDistance = 1000.0f;

	/** Selects the reference mirrored transition or one physical cloud layer without screen prefill. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Ray March")
	ECloudSeaVerticalRenderingMode VerticalRenderingMode =
		ECloudSeaVerticalRenderingMode::ReferenceMirrorWithPrefill;

	/** RGBA: negative layer height, mode-space camera depth below cloud top, prefill opacity, proxy depth-restoration numerator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Reference")
	FLinearColor ReferenceCloudLayerRayMarchData = FLinearColor(-50.0f, -1.3785701990127563f, 0.0f, -12.407132148742676f);

	/** RG: maximum ray-march distance and the currently active distance limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Reference")
	FLinearColor ReferenceRayMarchDistanceLimits = FLinearColor(1000.0f, 949.9676513671875f, 0.0f, 0.0f);

	/** RG offsets density UV; BA offsets noise UV at the reference camera position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Reference")
	FLinearColor ReferenceVolumeUVOffsets = FLinearColor(-18.425291061401367f, 243.47021484375f, -9.212645530700684f, 121.735107421875f);

	/** RGB tints both light radiances; A weights the density-volume secondary color channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Appearance")
	FLinearColor CloudScatteringTintAndSecondaryWeight = FLinearColor(0.450980007648468f, 0.556863009929657f, 0.764706015586853f, 0.03921600058674812f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Appearance")
	FLinearColor CloudPhaseFunctionCoefficients = FLinearColor(0.015119723975658417f, 1.809999942779541f, 1.7999999523162842f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Appearance")
	FLinearColor DensityVolumeUVScale = FLinearColor(0.0014563801232725382f, 0.0014563801232725382f, 0.019999999552965164f, 0.0f);

	/** Converts sampled density and ray length into per-step optical depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Appearance")
	float ExtinctionDensityScale = 0.10000000149011612f;

	/** RG: unphased light weight and phase-function light weight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Lighting")
	FLinearColor CloudLightingWeights = FLinearColor(0.10000000149011612f, 1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Lighting")
	FLinearColor PrimaryLightDirection = FLinearColor(0.47468826174736023f, -0.7995903491973877f, -0.3678671717643738f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Lighting")
	FLinearColor PrimaryLightRadiance = FLinearColor(6.375365734100342f, 4.829521656036377f, 2.932486057281494f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Lighting")
	FLinearColor SecondaryLightDirection = FLinearColor(-0.7957783937454224f, 0.4902935326099396f, -0.35545602440834045f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Sea|Material|Lighting")
	FLinearColor SecondaryLightRadiance = FLinearColor(0.6158669590950012f, 0.4554755687713623f, 0.26222044229507446f, 0.0f);
};
