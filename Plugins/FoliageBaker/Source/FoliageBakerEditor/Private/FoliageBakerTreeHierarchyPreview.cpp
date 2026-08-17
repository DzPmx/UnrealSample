#include "FoliageBakerTreeHierarchyPreview.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMeshBuilder.h"
#include "EditorViewportClient.h"
#include "EditorModes.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshDescription.h"
#include "PrimitiveDrawInterface.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "StaticMeshAttributes.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr double MinimumPreviewRadius = 1.0;
	constexpr double PreviewCameraDistanceScale = 1.5;
	constexpr int32 CylinderSideCount = 12;
	constexpr int32 SphereSideCount = 12;
	constexpr int32 SphereRingCount = 6;
	constexpr float BranchLineThickness = 2.0f;
	constexpr float TrunkLineThickness = 3.0f;
	constexpr double HighlightRadiusScale = 1.35;
	const FLinearColor HighlightColor(1.0f, 0.65f, 0.05f, 1.0f);
	const FLinearColor LeafPivotColor(1.0f, 0.18f, 0.12f, 1.0f);
	const FLinearColor LeafTipColor(0.10f, 0.82f, 1.0f, 1.0f);
	constexpr float LeafAxisLineThickness = 2.0f;
	constexpr float LeafAxisPointSize = 7.0f;

	class FDynamicSelectionColorMaterialRenderProxy final
		: public FDynamicPrimitiveResource
		, public FOverrideSelectionColorMaterialRenderProxy
	{
	public:
		FDynamicSelectionColorMaterialRenderProxy(
			const FMaterialRenderProxy& InParent,
			const FLinearColor& InSelectionColor)
			: FOverrideSelectionColorMaterialRenderProxy(
				&InParent,
				InSelectionColor)
		{
		}

		virtual void InitPrimitiveResource(
			FRHICommandListBase& RHICmdList) override
		{
			(void)RHICmdList;
		}

		virtual void ReleasePrimitiveResource() override
		{
			delete this;
		}
	};

	class FSourceTriangleHighlightMeshBuilder final
	{
	public:
		FSourceTriangleHighlightMeshBuilder(
			const UStaticMesh& InSourceStaticMesh,
			const FMeshDescription& InMeshDescription,
			const ERHIFeatureLevel::Type InFeatureLevel)
			: SourceStaticMesh(InSourceStaticMesh)
			, MeshDescription(InMeshDescription)
			, Attributes(InMeshDescription)
			, VertexPositions(Attributes.GetVertexPositions())
			, VertexInstanceNormals(Attributes.GetVertexInstanceNormals())
			, VertexInstanceTangents(Attributes.GetVertexInstanceTangents())
			, VertexInstanceBinormalSigns(
				Attributes.GetVertexInstanceBinormalSigns())
			, VertexInstanceColors(Attributes.GetVertexInstanceColors())
			, VertexInstanceUVs(Attributes.GetVertexInstanceUVs())
			, MaterialSlotNames(Attributes.GetPolygonGroupMaterialSlotNames())
			, FeatureLevel(InFeatureLevel)
			, TextureCoordinateCount(FMath::Clamp(
				VertexInstanceUVs.GetNumChannels(),
				1,
				MAX_STATIC_TEXCOORDS))
		{
		}

		void AddSourceTriangle(const int32 SourceTriangleID)
		{
			if (AddedSourceTriangleIDs.Contains(SourceTriangleID))
			{
				return;
			}
			AddedSourceTriangleIDs.Add(SourceTriangleID);

			const FTriangleID TriangleID(SourceTriangleID);
			const FPolygonGroupID PolygonGroupID =
				MeshDescription.GetTrianglePolygonGroup(TriangleID);
			const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
			int32 MaterialIndex =
				SourceStaticMesh.GetMaterialIndex(MaterialSlotName);
			if (MaterialIndex == INDEX_NONE)
			{
				MaterialIndex =
					SourceStaticMesh.GetMaterialIndexFromImportedMaterialSlotName(
						MaterialSlotName);
			}

			TUniquePtr<FDynamicMeshBuilder>& HighlightMesh =
				HighlightMeshes.FindOrAdd(MaterialIndex);
			if (!HighlightMesh)
			{
				HighlightMesh = MakeUnique<FDynamicMeshBuilder>(
					FeatureLevel,
					static_cast<uint32>(TextureCoordinateCount));
			}

			int32 FirstVertexIndex = INDEX_NONE;
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const FVertexInstanceID VertexInstanceID =
					MeshDescription.GetTriangleVertexInstance(
						TriangleID,
						CornerIndex);
				const FVertexID VertexID =
					MeshDescription.GetVertexInstanceVertex(VertexInstanceID);
				FDynamicMeshVertex Vertex(VertexPositions[VertexID]);
				const FVector3f TangentX =
					VertexInstanceTangents[VertexInstanceID];
				const FVector3f TangentZ =
					VertexInstanceNormals[VertexInstanceID];
				const FVector3f TangentY = FVector3f::CrossProduct(
					TangentZ,
					TangentX).GetSafeNormal()
					* VertexInstanceBinormalSigns[VertexInstanceID];
				Vertex.SetTangents(TangentX, TangentY, TangentZ);
				for (int32 TextureCoordinateIndex = 0;
					TextureCoordinateIndex < VertexInstanceUVs.GetNumChannels()
						&& TextureCoordinateIndex < MAX_STATIC_TEXCOORDS;
					++TextureCoordinateIndex)
				{
					Vertex.TextureCoordinate[TextureCoordinateIndex] =
						VertexInstanceUVs.Get(
							VertexInstanceID,
							TextureCoordinateIndex);
				}
				Vertex.Color = FLinearColor(
					VertexInstanceColors[VertexInstanceID]).ToFColor(true);
				const int32 VertexIndex = HighlightMesh->AddVertex(Vertex);
				if (CornerIndex == 0)
				{
					FirstVertexIndex = VertexIndex;
				}
			}
			HighlightMesh->AddTriangle(
				FirstVertexIndex,
				FirstVertexIndex + 1,
				FirstVertexIndex + 2);
		}

		void Draw(FPrimitiveDrawInterface& DrawInterface)
		{
			for (TPair<int32, TUniquePtr<FDynamicMeshBuilder>>& HighlightMeshPair :
				HighlightMeshes)
			{
				TObjectPtr<UMaterialInterface> SourceMaterial =
					SourceStaticMesh.GetMaterial(HighlightMeshPair.Key);
				if (!SourceMaterial)
				{
					SourceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
				}
				FMaterialRenderProxy& SourceMaterialProxy =
					*SourceMaterial->GetRenderProxy();
				FDynamicSelectionColorMaterialRenderProxy& HighlightMaterialProxy =
					*new FDynamicSelectionColorMaterialRenderProxy(
						SourceMaterialProxy,
						HighlightColor);
				DrawInterface.RegisterDynamicResource(&HighlightMaterialProxy);
				HighlightMeshPair.Value->Draw(
					&DrawInterface,
					FMatrix::Identity,
					&HighlightMaterialProxy,
					SDPG_Foreground,
					true,
					false);
			}
		}

	private:
		const UStaticMesh& SourceStaticMesh;
		const FMeshDescription& MeshDescription;
		FStaticMeshConstAttributes Attributes;
		TVertexAttributesConstRef<FVector3f> VertexPositions;
		TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceNormals;
		TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceTangents;
		TVertexInstanceAttributesConstRef<float> VertexInstanceBinormalSigns;
		TVertexInstanceAttributesConstRef<FVector4f> VertexInstanceColors;
		TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs;
		TPolygonGroupAttributesConstRef<FName> MaterialSlotNames;
		ERHIFeatureLevel::Type FeatureLevel;
		int32 TextureCoordinateCount = 1;
		TSet<int32> AddedSourceTriangleIDs;
		TMap<int32, TUniquePtr<FDynamicMeshBuilder>> HighlightMeshes;
	};
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
		, PreviewMeshComponent(NewObject<UStaticMeshComponent>())
	{
		check(PreviewMeshComponent.IsValid());
		PreviewMeshComponent->SetCastShadow(false);
		PreviewMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PreviewMeshComponent->SetMobility(EComponentMobility::Movable);
		// FPreviewScene requires a borrowed engine pointer at this boundary.
		InPreviewScene.AddComponent(
			PreviewMeshComponent.Get(),
			FTransform::Identity);
		SetViewportType(LVT_Perspective);
		SetViewMode(VMI_Unlit);
		SetRealtime(false);
		bSetListenerPosition = false;
		EngineShowFlags.DisableAdvancedFeatures();
		EngineShowFlags.SetCompositeEditorPrimitives(false);
		DrawHelper.bDrawPivot = false;
		DrawHelper.bDrawWorldBox = false;
		DrawHelper.bDrawKillZ = false;
		DrawHelper.bDrawGrid = false;
		DrawHelper.GridColorAxis = FColor(55, 55, 55);
		DrawHelper.GridColorMajor = FColor(35, 35, 35);
		DrawHelper.GridColorMinor = FColor(22, 22, 22);
		ToggleOrbitCamera(true);
	}

	void SetPreviewData(
		TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData,
		const bool bFrameCamera)
	{
		PreviewData = MoveTemp(InPreviewData);
		// UStaticMeshComponent requires a borrowed UObject pointer at this boundary.
		PreviewMeshComponent->SetStaticMesh(
			PreviewData.IsValid()
				? PreviewData->SourceStaticMesh.Get()
				: nullptr);
		PreviewMeshComponent->SetForcedLodModel(
			PreviewData.IsValid() ? PreviewData->SourceLODIndex + 1 : 0);
		if (bFrameCamera
			&& PreviewData.IsValid()
			&& PreviewData->Bounds.IsValid)
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

	void SetDebugDrawingEnabled(const bool bInEnabled)
	{
		bDebugDrawingEnabled = bInEnabled;
		Invalidate();
	}

	void SetHighlightedBranchIDs(const TSet<int32>& InBranchIDs)
	{
		HighlightedBranchIDs = InBranchIDs;
		Invalidate();
	}

	void SetResolvedLeaves(
		const TArray<FFoliageBakerResolvedLeafCluster>& InResolvedLeaves)
	{
		ResolvedLeaves = InResolvedLeaves;
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
		if (!bDebugDrawingEnabled || !PreviewData.IsValid())
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
			const bool bTrunk = Branch.BranchID == INDEX_NONE;
			const bool bHighlighted =
				HighlightedBranchIDs.Contains(Branch.BranchID);
			const FLinearColor DrawColor =
				bHighlighted ? HighlightColor : Branch.Color;
			const double RadiusScale =
				bHighlighted ? HighlightRadiusScale : 1.0;
			// PDI owns and releases this one-frame Engine render resource.
			FDynamicColoredMaterialRenderProxy& MaterialProxy =
				*new FDynamicColoredMaterialRenderProxy(
					GEngine->DebugMeshMaterial->GetRenderProxy(),
					DrawColor);
			DrawInterface.RegisterDynamicResource(&MaterialProxy);
			for (const FFoliageBakerTreeHierarchyPreviewCylinder& Cylinder :
				Branch.Cylinders)
			{
				if (bTrunk)
				{
					DrawInterface.DrawLine(
						Cylinder.Start,
						Cylinder.End,
						DrawColor,
						SDPG_Foreground,
						TrunkLineThickness,
						0.0f,
						true);
				}
				else
				{
					DrawCylinder(
						&DrawInterface,
						Cylinder.Start,
						Cylinder.End,
						Cylinder.Radius * RadiusScale,
						CylinderSideCount,
						&MaterialProxy,
						SDPG_Foreground);
					DrawInterface.DrawLine(
						Cylinder.Start,
						Cylinder.End,
						DrawColor,
						SDPG_Foreground,
						BranchLineThickness,
						0.0f,
						true);
				}
			}
			for (const FFoliageBakerTreeHierarchyPreviewJoint& Joint :
				Branch.Joints)
			{
				DrawSphere(
					&DrawInterface,
					Joint.Position,
					FRotator::ZeroRotator,
					FVector(Joint.Radius * RadiusScale),
					SphereSideCount,
					SphereRingCount,
					&MaterialProxy,
					SDPG_Foreground);
			}
		}
		for (const FFoliageBakerResolvedLeafCluster& ResolvedLeaf :
			ResolvedLeaves)
		{
			if (!HighlightedBranchIDs.IsEmpty()
				&& !HighlightedBranchIDs.Contains(
					ResolvedLeaf.ParentBranchID))
			{
				continue;
			}
			FLinearColor AxisColor = FLinearColor::White;
			for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
				PreviewData->Branches)
			{
				if (Branch.BranchID == ResolvedLeaf.ParentBranchID)
				{
					AxisColor = Branch.Color;
					break;
				}
			}
			DrawInterface.DrawLine(
				ResolvedLeaf.PivotPosition,
				ResolvedLeaf.TipPosition,
				AxisColor,
				SDPG_Foreground,
				LeafAxisLineThickness,
				0.0f,
				true);
			DrawInterface.DrawPoint(
				ResolvedLeaf.PivotPosition,
				LeafPivotColor,
				LeafAxisPointSize,
				SDPG_Foreground);
			DrawInterface.DrawPoint(
				ResolvedLeaf.TipPosition,
				LeafTipColor,
				LeafAxisPointSize,
				SDPG_Foreground);
		}
		if (!HighlightedBranchIDs.IsEmpty() || !ResolvedLeaves.IsEmpty())
		{
			const UStaticMesh& SourceStaticMesh =
				*PreviewMeshComponent->GetStaticMesh();
			const FMeshDescription& MeshDescription =
				*SourceStaticMesh.GetMeshDescription(
					PreviewData->SourceLODIndex);
			FSourceTriangleHighlightMeshBuilder HighlightMeshBuilder(
				SourceStaticMesh,
				MeshDescription,
				View->GetFeatureLevel());
			if (!HighlightedBranchIDs.IsEmpty())
			{
				for (const FFoliageBakerTreeHierarchyPreviewBranch& Branch :
					PreviewData->Branches)
				{
					if (HighlightedBranchIDs.Contains(Branch.BranchID))
					{
						for (const int32 SourceTriangleID : Branch.SourceTriangleIDs)
						{
							HighlightMeshBuilder.AddSourceTriangle(SourceTriangleID);
						}
					}
				}
			}
			for (const FFoliageBakerResolvedLeafCluster& ResolvedLeaf :
				ResolvedLeaves)
			{
				if (HighlightedBranchIDs.IsEmpty()
					|| HighlightedBranchIDs.Contains(
						ResolvedLeaf.ParentBranchID))
				{
					for (const int32 SourceTriangleID :
						ResolvedLeaf.SourceTriangleIDs)
					{
						HighlightMeshBuilder.AddSourceTriangle(SourceTriangleID);
					}
				}
			}
			HighlightMeshBuilder.Draw(DrawInterface);
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
			: FText::FromString(TEXT("Analyze a Static Mesh to preview its trunk and branches."));
		FCanvasTextItem HeaderItem(
			FVector2D(10.0, 10.0),
			HeaderText,
			GEngine->GetSmallFont(),
			FLinearColor::White);
		HeaderItem.EnableShadow(FLinearColor::Black);
		Canvas.DrawItem(HeaderItem);
		if (!bDebugDrawingEnabled || !PreviewData.IsValid())
		{
			return;
		}
		if (!ResolvedLeaves.IsEmpty())
		{
			int32 VisibleLeafAxisCount = 0;
			for (const FFoliageBakerResolvedLeafCluster& ResolvedLeaf :
				ResolvedLeaves)
			{
				if (HighlightedBranchIDs.IsEmpty()
					|| HighlightedBranchIDs.Contains(
						ResolvedLeaf.ParentBranchID))
				{
					++VisibleLeafAxisCount;
				}
			}
			const FString AxisSummary = FString::Printf(
				TEXT("Leaf axes: %d / %d  |  pivot red, tip cyan, line = Parent Branch color"),
				VisibleLeafAxisCount,
				ResolvedLeaves.Num());
			FCanvasTextItem AxisSummaryItem(
				FVector2D(10.0, 28.0),
				FText::FromString(AxisSummary),
				GEngine->GetSmallFont(),
				FLinearColor::White);
			AxisSummaryItem.EnableShadow(FLinearColor::Black);
			Canvas.DrawItem(AxisSummaryItem);
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
			const FFoliageBakerTreeHierarchyBoneRecord& BoneRecord =
				Branch.BoneRecord;
			const FString DebugLabel = FString::Printf(
				TEXT("%s  Bone %d/%d  Length %.1f  Positive %.1f  Min %.1f"),
				*Branch.Label,
				BoneRecord.BakeID,
				BoneRecord.ParentBakeID,
				BoneRecord.AxisExtent,
				BoneRecord.PositiveAxisExtent,
				BoneRecord.MinimumAxisProjection);
			FCanvasTextItem LabelItem(
				ScreenPosition,
				FText::FromString(DebugLabel),
				GEngine->GetSmallFont(),
				HighlightedBranchIDs.Contains(Branch.BranchID)
					? HighlightColor
					: Branch.Color);
			LabelItem.EnableShadow(FLinearColor::Black);
			Canvas.DrawItem(LabelItem);
		}
	}

private:
	TStrongObjectPtr<UStaticMeshComponent> PreviewMeshComponent;
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> PreviewData;
	TSet<int32> HighlightedBranchIDs;
	TArray<FFoliageBakerResolvedLeafCluster> ResolvedLeaves;
	bool bDebugDrawingEnabled = true;
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
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData,
	const bool bFrameCamera)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetPreviewData(
			MoveTemp(InPreviewData),
			bFrameCamera);
	}
}

void SFoliageBakerTreeHierarchyPreview::SetHighlightedBranchIDs(
	const TSet<int32>& InBranchIDs)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetHighlightedBranchIDs(InBranchIDs);
	}
}

void SFoliageBakerTreeHierarchyPreview::SetResolvedLeaves(
	const TArray<FFoliageBakerResolvedLeafCluster>& InResolvedLeaves)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetResolvedLeaves(InResolvedLeaves);
	}
}

void SFoliageBakerTreeHierarchyPreview::SetDebugDrawingEnabled(
	const bool bInEnabled)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetDebugDrawingEnabled(bInEnabled);
	}
}

void SFoliageBakerTreeHierarchyPreview::ClearPreview()
{
	const TArray<FFoliageBakerResolvedLeafCluster> EmptyResolvedLeaves;
	SetResolvedLeaves(EmptyResolvedLeaves);
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
