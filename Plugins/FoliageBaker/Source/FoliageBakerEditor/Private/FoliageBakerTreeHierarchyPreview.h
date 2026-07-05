#pragma once

#include "CoreMinimal.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"

class FFoliageBakerTreeHierarchyViewportClient;
struct FFoliageBakerResolvedLeafCluster;
struct FFoliageBakerTreeHierarchyPreviewData;

class SFoliageBakerTreeHierarchyPreview final : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SFoliageBakerTreeHierarchyPreview)
	{
	}
	SLATE_END_ARGS()

	SFoliageBakerTreeHierarchyPreview();

	void Construct(const FArguments& InArgs);
	void SetPreviewData(
		TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData,
		bool bFrameCamera = true);
	void SetHighlightedBranchIDs(const TSet<int32>& InBranchIDs);
	void SetResolvedLeaves(
		const TArray<FFoliageBakerResolvedLeafCluster>& InResolvedLeaves);
	void SetDebugDrawingEnabled(bool bInEnabled);
	void ClearPreview();

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	FPreviewScene PreviewScene;
	TSharedPtr<FFoliageBakerTreeHierarchyViewportClient> ViewportClient;
};
