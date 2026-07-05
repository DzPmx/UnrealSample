#include "CloudSeaComponent.h"

#include "CoreGlobals.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

namespace
{
	constexpr double DiskHeightCompressionFactor = 10.0;
	constexpr double DiskHeightRemainderFactor = DiskHeightCompressionFactor - 1.0;
	constexpr double DiskProxyNearClipSafetyFactor = 1.5;
	constexpr double DiskProxyNearClipSafetyPadding = 10.0;
}

UCloudSeaComponent::UCloudSeaComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	PrimaryComponentTick.EndTickGroup = TG_PostUpdateWork;
	bTickInEditor = true;
	SetMobility(EComponentMobility::Movable);
}

void UCloudSeaComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	FEditorDelegates::OnEditorCameraMoved.AddUObject(
		this,
		&UCloudSeaComponent::HandleEditorCameraMoved);
#endif
	InitializeMaterialDriver();
	UpdateCloudSeaTransform();
}

void UCloudSeaComponent::OnUnregister()
{
#if WITH_EDITOR
	FEditorDelegates::OnEditorCameraMoved.RemoveAll(this);
#endif
	Super::OnUnregister();
}

void UCloudSeaComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateCloudSeaTransform();
}

#if WITH_EDITOR
void UCloudSeaComponent::HandleEditorCameraMoved(
	const FVector&,
	const FRotator&,
	ELevelViewportType,
	int32)
{
	UpdateCloudSeaTransform();
}
#endif

bool UCloudSeaComponent::TryGetView(
	FVector& OutViewLocation,
	FRotator& OutViewRotation,
	float& OutNearClipPlane) const
{
	if (GetWorld() == nullptr)
	{
		return false;
	}

	const UWorld& World = *GetWorld();
	OutNearClipPlane = GNearClippingPlane;

#if WITH_EDITOR
	// SIE and ejected PIE render through the level viewport while the player camera remains elsewhere.
	if (GCurrentLevelEditingViewportClient != nullptr)
	{
		const FLevelEditorViewportClient& ViewportClient = *GCurrentLevelEditingViewportClient;
		if (ViewportClient.GetWorld() == &World && ViewportClient.IsPerspective())
		{
			OutViewLocation = ViewportClient.GetViewLocation();
			OutViewRotation = ViewportClient.GetViewRotation();
			OutNearClipPlane = ViewportClient.GetNearClipPlane();
			return true;
		}
	}
#endif

	if (World.IsGameWorld())
	{
		const TWeakObjectPtr<APlayerCameraManager> CameraManager =
			UGameplayStatics::GetPlayerCameraManager(this, CameraPlayerIndex);
		if (CameraManager.IsValid())
		{
			OutViewLocation = CameraManager->GetCameraLocation();
			OutViewRotation = CameraManager->GetCameraRotation();
			return true;
		}

		return false;
	}

	return false;
}

void UCloudSeaComponent::InitializeMaterialDriver()
{
	if (!ProxyMeshComponent.IsValid())
	{
		const TWeakObjectPtr<AActor> Owner = GetOwner();
		if (Owner.IsValid())
		{
			ProxyMeshComponent = Owner->FindComponentByClass<UStaticMeshComponent>();
		}
	}

	if (ProxyMeshComponent.IsValid() && ProxyMeshComponent->bSelectable)
	{
		ProxyMeshComponent->bSelectable = false;
		ProxyMeshComponent->MarkRenderStateDirty();
	}

	if (ProxyMeshComponent.IsValid() && DynamicMaterial == nullptr)
	{
		DynamicMaterial = ProxyMeshComponent->CreateDynamicMaterialInstance(0);
	}
}

void UCloudSeaComponent::UpdateCloudSeaTransform()
{
	FVector ViewLocation;
	FRotator ViewRotation;
	float NearClipPlane;
	if (!TryGetView(ViewLocation, ViewRotation, NearClipPlane))
	{
		return;
	}

	InitializeMaterialDriver();

	const double CloudLayerTopWorldHeight =
		CloudLayerBaseWorldHeight
		- ReferenceCloudLayerRayMarchData.R * WorldUnitsPerMaterialUnit;
	const double SignedCameraDepthBelowCloudTopMaterialUnits =
		(CloudLayerTopWorldHeight - ViewLocation.Z) / WorldUnitsPerMaterialUnit;
	const double CameraHeightAboveCloudTopWorldUnits =
		FMath::Max(ViewLocation.Z - CloudLayerTopWorldHeight, 0.0);
	const double DiskRayMarchTransitionAlpha = FMath::SmoothStep(
		0.0,
		static_cast<double>(DiskRayMarchTransitionWorldDistance),
		CameraHeightAboveCloudTopWorldUnits);
	const bool bUseReferenceMirrorTransition =
		VerticalRenderingMode == ECloudSeaVerticalRenderingMode::ReferenceMirrorWithPrefill;
	const float DiskTransitionStartMarchDistanceMaterialUnits =
		bUseReferenceMirrorTransition
		? FullScreenTransitionMarchDistance
		: ReferenceRayMarchDistanceLimits.G;
	const float ActiveDiskRayMarchDistanceMaterialUnits = FMath::Lerp(
		DiskTransitionStartMarchDistanceMaterialUnits,
		ReferenceRayMarchDistanceLimits.G,
		static_cast<float>(DiskRayMarchTransitionAlpha));
	const bool bUseFullScreenProxy =
		ViewLocation.Z <= CloudLayerTopWorldHeight;

	FVector CloudSeaLocation;
	FQuat CloudSeaRotation;
	double CloudSeaWorldRadius = ProxyWorldRadius;
	if (bUseFullScreenProxy)
	{
		const FVector ViewForward = ViewRotation.Vector();
		CloudSeaLocation = ViewLocation + ViewForward * FullScreenProxyCameraDistance;
		CloudSeaRotation = FQuat::FindBetweenNormals(FVector::UpVector, ViewForward);
	}
	else
	{
		const double CompressedCameraDepthBelowCloudTopMaterialUnits =
			SignedCameraDepthBelowCloudTopMaterialUnits / DiskHeightCompressionFactor;
		CloudSeaWorldRadius =
			DiskHeightCompressionFactor
			* FMath::Sqrt(
				FMath::Square(ActiveDiskRayMarchDistanceMaterialUnits)
				- FMath::Square(CompressedCameraDepthBelowCloudTopMaterialUnits))
			* WorldUnitsPerMaterialUnit;
		const double ReconstructedDiskProxyWorldOffset =
			CompressedCameraDepthBelowCloudTopMaterialUnits
			* WorldUnitsPerMaterialUnit
			* (DiskMarchFarDistance / ActiveDiskRayMarchDistanceMaterialUnits);
		const double DiskProxyWorldOffset = FMath::Min(
			ReconstructedDiskProxyWorldOffset,
			-(static_cast<double>(NearClipPlane) * DiskProxyNearClipSafetyFactor
				+ DiskProxyNearClipSafetyPadding));
		const double DiskProxyWorldZ = ViewLocation.Z + DiskProxyWorldOffset;
		CloudSeaLocation = FVector(
			ViewLocation.X,
			ViewLocation.Y,
			DiskProxyWorldZ);
		CloudSeaRotation = FQuat::Identity;
	}

	const FVector CloudSeaScale(CloudSeaWorldRadius, CloudSeaWorldRadius, 1.0);
	const FTransform CloudSeaTransform(CloudSeaRotation, CloudSeaLocation, CloudSeaScale);
	SetWorldTransform(CloudSeaTransform, false, nullptr, ETeleportType::TeleportPhysics);
	UpdateMaterialParameters(
		ViewLocation,
		bUseFullScreenProxy,
		SignedCameraDepthBelowCloudTopMaterialUnits,
		ActiveDiskRayMarchDistanceMaterialUnits);
}

void UCloudSeaComponent::UpdateMaterialParameters(
	const FVector& ViewLocation,
	bool bUseFullScreenProxy,
	double SignedCameraDepthBelowCloudTopMaterialUnits,
	float ActiveDiskRayMarchDistanceMaterialUnits)
{
	if (DynamicMaterial == nullptr)
	{
		return;
	}

	DynamicMaterial->SetScalarParameterValue(
		TEXT("RayMarchStepCount"),
		FMath::Clamp(RayMarchStepCount, 16.0f, 128.0f));
	DynamicMaterial->SetScalarParameterValue(TEXT("NoiseVolumeUVScale"), NoiseVolumeUVScale);
	DynamicMaterial->SetScalarParameterValue(TEXT("NoiseWarpAmplitude"), NoiseWarpAmplitude);
	DynamicMaterial->SetScalarParameterValue(TEXT("DiskMarchFarDistance"), DiskMarchFarDistance);
	DynamicMaterial->SetScalarParameterValue(TEXT("ExtinctionDensityScale"), ExtinctionDensityScale);
	DynamicMaterial->SetScalarParameterValue(
		TEXT("SceneDepthFadeReciprocalDistance"),
		SceneDepthFadeReciprocalDistance);
	DynamicMaterial->SetVectorParameterValue(
		TEXT("CloudScatteringTintAndSecondaryWeight"),
		CloudScatteringTintAndSecondaryWeight);
	DynamicMaterial->SetVectorParameterValue(
		TEXT("CloudPhaseFunctionCoefficients"),
		CloudPhaseFunctionCoefficients);
	DynamicMaterial->SetVectorParameterValue(TEXT("DensityVolumeUVScale"), DensityVolumeUVScale);
	DynamicMaterial->SetVectorParameterValue(TEXT("CloudLightingWeights"), CloudLightingWeights);
	DynamicMaterial->SetVectorParameterValue(TEXT("PrimaryLightDirection"), PrimaryLightDirection);
	DynamicMaterial->SetVectorParameterValue(TEXT("PrimaryLightRadiance"), PrimaryLightRadiance);
	DynamicMaterial->SetVectorParameterValue(TEXT("SecondaryLightDirection"), SecondaryLightDirection);
	DynamicMaterial->SetVectorParameterValue(TEXT("SecondaryLightRadiance"), SecondaryLightRadiance);

	const float CameraDepthBelowCloudTopMaterialUnits =
		static_cast<float>(SignedCameraDepthBelowCloudTopMaterialUnits);
	FLinearColor CameraRelativeCloudLayerRayMarchData(
		ReferenceCloudLayerRayMarchData.R,
		CameraDepthBelowCloudTopMaterialUnits,
		0.0f,
		0.0f);
	FLinearColor CameraRelativeRayMarchDistanceLimits = ReferenceRayMarchDistanceLimits;
	const bool bUseReferenceMirrorTransition =
		VerticalRenderingMode == ECloudSeaVerticalRenderingMode::ReferenceMirrorWithPrefill;
	const float CameraDepthBelowCloudBaseMaterialUnits =
		CameraRelativeCloudLayerRayMarchData.R
		+ CameraDepthBelowCloudTopMaterialUnits;
	if (bUseFullScreenProxy)
	{
		// Keep the mirrored layer and its prefill transition intact before compressing the gap below it.
		const float FullScreenCompressionAnchorDepthBelowCloudBaseMaterialUnits =
			bUseReferenceMirrorTransition ? -CameraRelativeCloudLayerRayMarchData.R : 0.0f;
		const float CameraDistanceBelowFullScreenCompressionAnchorMaterialUnits =
			CameraDepthBelowCloudBaseMaterialUnits
			- FullScreenCompressionAnchorDepthBelowCloudBaseMaterialUnits;
		if (CameraDistanceBelowFullScreenCompressionAnchorMaterialUnits > 0.0f)
		{
			const float CompressedCameraDistanceBelowAnchorMaterialUnits =
				CameraDistanceBelowFullScreenCompressionAnchorMaterialUnits
				/ static_cast<float>(DiskHeightCompressionFactor);
			CameraRelativeCloudLayerRayMarchData.G =
				-CameraRelativeCloudLayerRayMarchData.R
				+ FullScreenCompressionAnchorDepthBelowCloudBaseMaterialUnits
				+ CompressedCameraDistanceBelowAnchorMaterialUnits;
			CameraRelativeCloudLayerRayMarchData.A =
				CameraDistanceBelowFullScreenCompressionAnchorMaterialUnits
				- CompressedCameraDistanceBelowAnchorMaterialUnits;
		}
	}
	else
	{
		CameraRelativeCloudLayerRayMarchData.G =
			SignedCameraDepthBelowCloudTopMaterialUnits / DiskHeightCompressionFactor;
		CameraRelativeCloudLayerRayMarchData.A =
			CameraRelativeCloudLayerRayMarchData.G * DiskHeightRemainderFactor;
		CameraRelativeRayMarchDistanceLimits.G =
			ActiveDiskRayMarchDistanceMaterialUnits;
	}

	float DensityVolumeZSign = 1.0f;
	if (bUseFullScreenProxy && bUseReferenceMirrorTransition)
	{
		const float CloudLayerHeight = -ReferenceCloudLayerRayMarchData.R;
		const bool bCameraBelowLayer = CameraDepthBelowCloudBaseMaterialUnits > 0.0f;

		float PrefillCoordinate;
		if (bCameraBelowLayer)
		{
			const float NormalizedDepthBelowBase = FMath::Clamp(
				CameraDepthBelowCloudBaseMaterialUnits / CloudLayerHeight,
				0.0f,
				1.0f);
			CameraRelativeRayMarchDistanceLimits.G =
				FullScreenTransitionMarchDistance * NormalizedDepthBelowBase;
			PrefillCoordinate = 1.0f - NormalizedDepthBelowBase;
			DensityVolumeZSign = -1.0f;
		}
		else
		{
			const float NormalizedDepthBelowTop = FMath::Clamp(
				CameraDepthBelowCloudTopMaterialUnits / CloudLayerHeight,
				0.0f,
				1.0f);
			CameraRelativeRayMarchDistanceLimits.G =
				FullScreenTransitionMarchDistance * (1.0f - NormalizedDepthBelowTop);
			PrefillCoordinate = NormalizedDepthBelowTop;
		}

		const float PrefillBase = FMath::Clamp(
			1.5f * PrefillCoordinate,
			0.0f,
			1.0f);
		CameraRelativeCloudLayerRayMarchData.B =
			FMath::Square(FMath::Square(PrefillBase));
	}

	DynamicMaterial->SetVectorParameterValue(
		TEXT("CloudLayerRayMarchData"),
		CameraRelativeCloudLayerRayMarchData);
	DynamicMaterial->SetVectorParameterValue(
		TEXT("RayMarchDistanceLimits"),
		CameraRelativeRayMarchDistanceLimits);

	const FVector2D CameraDeltaMaterialUnits(
		(ViewLocation.X - VolumeAnchorReferenceCameraXY.X) / WorldUnitsPerMaterialUnit,
		(ViewLocation.Y - VolumeAnchorReferenceCameraXY.Y) / WorldUnitsPerMaterialUnit);
	// All supplied RDCs share one camera XY. Applying the camera delta to the UV offsets
	// is therefore an explicit reconstruction assumption; its sign follows the shader's
	// camera-relative ray coordinates so a fixed world sample keeps a fixed volume UV.
	FLinearColor CameraRelativeVolumeUVOffsets = ReferenceVolumeUVOffsets;
	CameraRelativeVolumeUVOffsets.R += CameraDeltaMaterialUnits.X;
	CameraRelativeVolumeUVOffsets.G += CameraDeltaMaterialUnits.Y;
	CameraRelativeVolumeUVOffsets.B += CameraDeltaMaterialUnits.X * 0.5;
	CameraRelativeVolumeUVOffsets.A += CameraDeltaMaterialUnits.Y * 0.5;
	DynamicMaterial->SetVectorParameterValue(
		TEXT("VolumeUVOffsets"),
		CameraRelativeVolumeUVOffsets);

	DynamicMaterial->SetScalarParameterValue(
		TEXT("UseFullScreenProxy"),
		bUseFullScreenProxy ? 1.0f : 0.0f);
	DynamicMaterial->SetScalarParameterValue(
		TEXT("DensityVolumeZSign"),
		DensityVolumeZSign);
}
