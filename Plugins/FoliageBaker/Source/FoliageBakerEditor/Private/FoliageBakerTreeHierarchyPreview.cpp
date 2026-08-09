#include "FoliageBakerTreeHierarchyPreview.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/StaticMeshComponent.h"
#include "EditorViewportClient.h"
#include "EditorModes.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "Materials/Material.h"
#include "PrimitiveDrawInterface.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr double MinimumPreviewRadius = 1.0;
	constexpr double PreviewCameraDistanceScale = 2.5;
	constexpr int32 CylinderSideCount = 12;
	constexpr int32 SphereSideCount = 12;
	constexpr int32 SphereRingCount = 6;
}

class FFoliageBakerTreeHierarchyViewportClient final
	: public FEditorViewportClient
{
public:
	FFoliageBakerTreeHierarchyViewportClient(
		FPreviewScene& InPreviewScene,
		const TSharedRef<SFoliageBakerTreeHierarchyPreview>& InViewport)
		// Unreal's viewport boundary requires borrowed raw pointers.
		: FEditorViewportClient(
			nullptr,
			&InPreviewScene,
			StaticCastSharedRef<SEditorViewport>(InViewport))
		, PreviewScene(InPreviewScene)
		, PreviewMeshComponent(NewObject<UStaticMeshComponent>())
	{
		check(PreviewMeshComponent.IsValid());
		PreviewMeshComponent->SetCastShadow(false);
		PreviewMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PreviewMeshComponent->SetMobility(EComponentMobility::Movable);
		// FPreviewScene requires a borrowed engine pointer at this boundary.
		PreviewScene.AddComponent(
			PreviewMeshComponent.Get(),
			FTransform::Identity);
		SetViewportType(LVT_Perspective);
		SetViewMode(VMI_Unlit);
		SetRealtime(false);
		bSetListenerPosition = false;
		EngineShowFlags.DisableAdvancedFeatures();
		EngineShowFlags.SetCompositeEditorPrimitives(true);
		DrawHelper.bDrawPivot = false;
		DrawHelper.bDrawWorldBox = false;
		DrawHelper.bDrawKillZ = false;
		DrawHelper.bDrawGrid = true;
		DrawHelper.GridColorAxis = FColor(55, 55, 55);
		DrawHelper.GridColorMajor = FColor(35, 35, 35);
		DrawHelper.GridColorMinor = FColor(22, 22, 22);
		ToggleOrbitCamera(true);
	}

	virtual ~FFoliageBakerTreeHierarchyViewportClient() override
	{
		// FPreviewScene requires a borrowed engine pointer at this boundary.
		PreviewScene.RemoveComponent(PreviewMeshComponent.Get());
	}

	void SetPreviewData(
		TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData)
	{
		PreviewData = MoveTemp(InPreviewData);
		// UStaticMeshComponent requires a borrowed UObject pointer at this boundary.
		PreviewMeshComponent->SetStaticMesh(
			PreviewData.IsValid()
				? PreviewData->SourceStaticMesh.Get()
				: nullptr);
		PreviewMeshComponent->SetForcedLodModel(
			PreviewData.IsValid() ? PreviewData->SourceLODIndex + 1 : 0);
		if (PreviewData.IsValid() && PreviewData->Bounds.IsValid)
		{
			const float CameraDistance = static_cast<float>(FMath::Max(
				PreviewData->Bounds.GetExtent().Size()
					* PreviewCameraDistanceScale,
				MinimumPreviewRadius));
			SetViewRotation(FRotator(-15.0, -135.0, 0.0));
			SetViewLocationForOrbiting(
				PreviewData->Bounds.GetCenter(),
				CameraDistance);
		}
		Invalidate();
	}

	virtual FLinearColor GetBackgroundColor() const override
	{
		return FLinearColor(0.008f, 0.008f, 0.008f, 1.0f);
	}

	virtual void Draw(
		const FSceneView* View,
		FPrimitiveDrawInterface* PrimitiveDrawInterface) override
	{
		if (!View || !PrimitiveDrawInterface)
		{
			return;
		}
		FEditorViewportClient::Draw(View, PrimitiveDrawInterface);
		if (!PreviewData.IsValid())
		{
			return;
		}
		if (!ensure(GEngine && GEngine->DebugMeshMaterial))
		{
			return;
		}

		FPrimitiveDrawInterface& DrawInterface = *PrimitiveDrawInterface;
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData->Branches)
		{
			// PDI owns and releases this one-frame Engine render resource.
			FDynamicColoredMaterialRenderProxy& MaterialProxy =
				*new FDynamicColoredMaterialRenderProxy(
					GEngine->DebugMeshMaterial->GetRenderProxy(),
					Branch.Color);
			DrawInterface.RegisterDynamicResource(&MaterialProxy);
			for (const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder :
				Branch.Cylinders)
			{
				DrawCylinder(
					&DrawInterface,
					Cylinder.Start,
					Cylinder.End,
					Cylinder.Radius,
					CylinderSideCount,
					&MaterialProxy,
					SDPG_Foreground);
			}
			for (const FFoliageBakerTreeHierarchyPreviewJoint& Joint :
				Branch.Joints)
			{
				DrawSphere(
					&DrawInterface,
					Joint.Position,
					FRotator::ZeroRotator,
					FVector(Joint.Radius),
					SphereSideCount,
					SphereRingCount,
					&MaterialProxy,
					SDPG_Foreground);
			}
		}
	}

	virtual void DrawCanvas(
		FViewport& InViewport,
		FSceneView& View,
		FCanvas& Canvas) override
	{
		FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);
		const FText HeaderText = PreviewData.IsValid()
			? FText::FromString(PreviewData->AssetName)
			: FText::FromString(TEXT("Bake a Static Mesh to preview its tree hierarchy."));
		FCanvasTextItem HeaderItem(
			FVector2D(10.0, 10.0),
			HeaderText,
			GEngine->GetSmallFont(),
			FLinearColor::White);
		HeaderItem.EnableShadow(FLinearColor::Black);
		Canvas.DrawItem(HeaderItem);

		if (!PreviewData.IsValid())
		{
			return;
		}
		const double HalfWidth =
			static_cast<double>(InViewport.GetSizeXY().X) * 0.5 / GetDPIScale();
		const double HalfHeight =
			static_cast<double>(InViewport.GetSizeXY().Y) * 0.5 / GetDPIScale();
		for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
			PreviewData->Branches)
		{
			if (Branch.Label.IsEmpty())
			{
				continue;
			}
			const FPlane ProjectedPosition = View.Project(Branch.LabelPosition);
			if (ProjectedPosition.W <= 0.0)
			{
				continue;
			}
			const FVector2D ScreenPosition(
				HalfWidth + HalfWidth * ProjectedPosition.X + 4.0,
				HalfHeight - HalfHeight * ProjectedPosition.Y - 4.0);
			FCanvasTextItem LabelItem(
				ScreenPosition,
				FText::FromString(Branch.Label),
				GEngine->GetSmallFont(),
				Branch.Color);
			LabelItem.EnableShadow(FLinearColor::Black);
			Canvas.DrawItem(LabelItem);
		}
	}

private:
	FPreviewScene& PreviewScene;
	TStrongObjectPtr<UStaticMeshComponent> PreviewMeshComponent;
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> PreviewData;
};

SFoliageBakerTreeHierarchyPreview::SFoliageBakerTreeHierarchyPreview()
	: PreviewScene(FPreviewScene::ConstructionValues())
{
}

void SFoliageBakerTreeHierarchyPreview::Construct(const FArguments& InArgs)
{
	(void)InArgs;
	SEditorViewport::Construct(SEditorViewport::FArguments());
}

void SFoliageBakerTreeHierarchyPreview::SetPreviewData(
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetPreviewData(MoveTemp(InPreviewData));
	}
}

void SFoliageBakerTreeHierarchyPreview::ClearPreview()
{
	SetPreviewData(nullptr);
}

TSharedRef<FEditorViewportClient>
SFoliageBakerTreeHierarchyPreview::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FFoliageBakerTreeHierarchyViewportClient>(
		PreviewScene,
		SharedThis(this));
	return ViewportClient.ToSharedRef();
}
