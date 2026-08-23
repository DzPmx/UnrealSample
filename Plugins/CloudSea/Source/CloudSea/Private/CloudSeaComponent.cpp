#include "CloudSeaComponent.h"

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
	InitializeMaterialDriver();
	UpdateCloudSeaTransform();
}

void UCloudSeaComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateCloudSeaTransform();
}

bool UCloudSeaComponent::TryGetView(FVector& OutViewLocation, FRotator& OutViewRotation) const
{
	if (GetWorld() == nullptr)
	{
		return false;
	}

	const UWorld& World = *GetWorld();

#if WITH_EDITOR
	// SIE and ejected PIE render through the level viewport while the player camera remains elsewhere.
	if (GCurrentLevelEditingViewportClient != nullptr)
	{
		const FLevelEditorViewportClient& ViewportClient = *GCurrentLevelEditingViewportClient;
		if (ViewportClient.GetWorld() == &World && ViewportClient.IsPerspective())
		{
			OutViewLocation = ViewportClient.GetViewLocation();
			OutViewRotation = ViewportClient.GetViewRotation();
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
	if (!TryGetView(ViewLocation, ViewRotation))
	{
		return;
	}

	InitializeMaterialDriver();

	const double CloudLayerTopWorldHeight =
		CloudLayerBaseWorldHeight
		- ReferenceCloudLayerRayMarchData.R * WorldUnitsPerMaterialUnit;
	const bool bUseFullScreenProxy =
		ViewLocation.Z <= CloudLayerTopWorldHeight;

	FVector CloudSeaLocation;
	FQuat CloudSeaRotation;
	if (bUseFullScreenProxy)
	{
		const FVector ViewForward = ViewRotation.Vector();
		CloudSeaLocation = ViewLocation + ViewForward * FullScreenProxyCameraDistance;
		CloudSeaRotation = FQuat::FindBetweenNormals(FVector::UpVector, ViewForward);
	}
	else
	{
		CloudSeaLocation = FVector(
			ViewLocation.X,
			ViewLocation.Y,
			ViewLocation.Z - DiskProxyCameraVerticalOffset);
		CloudSeaRotation = FQuat::Identity;
	}

	const FVector CloudSeaScale(ProxyWorldRadius, ProxyWorldRadius, 1.0);
	const FTransform CloudSeaTransform(CloudSeaRotation, CloudSeaLocation, CloudSeaScale);
	SetWorldTransform(CloudSeaTransform, false, nullptr, ETeleportType::TeleportPhysics);
	UpdateMaterialParameters(ViewLocation, bUseFullScreenProxy);
}

void UCloudSeaComponent::UpdateMaterialParameters(
	const FVector& ViewLocation,
	bool bUseFullScreenProxy)
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

	FLinearColor CameraRelativeCloudLayerRayMarchData = ReferenceCloudLayerRayMarchData;
	const double SignedCameraDepthBelowLayerBase =
		(CloudLayerBaseWorldHeight - ViewLocation.Z) / WorldUnitsPerMaterialUnit;
	CameraRelativeCloudLayerRayMarchData.G =
		SignedCameraDepthBelowLayerBase - CameraRelativeCloudLayerRayMarchData.R;

	FLinearColor CameraRelativeRayMarchDistanceLimits = ReferenceRayMarchDistanceLimits;
	float DensityVolumeZSign = 1.0f;
	if (bUseFullScreenProxy)
	{
		const float CloudLayerHeight = -ReferenceCloudLayerRayMarchData.R;
		const float CameraDepthBelowTop = CameraRelativeCloudLayerRayMarchData.G;
		const float CameraDepthBelowBase =
			CameraRelativeCloudLayerRayMarchData.R
			+ CameraRelativeCloudLayerRayMarchData.G;
		const bool bCameraBelowLayer = CameraDepthBelowBase > 0.0f;

		float PrefillCoordinate;
		if (bCameraBelowLayer)
		{
			const float NormalizedDepthBelowBase = FMath::Clamp(
				CameraDepthBelowBase / CloudLayerHeight,
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
				CameraDepthBelowTop / CloudLayerHeight,
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
	else
	{
		CameraRelativeCloudLayerRayMarchData.B = 0.0f;
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
