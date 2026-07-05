#pragma once

#include "CoreMinimal.h"
#include "FoliageBakerTreeHierarchyBaker.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class SFoliageBakerLeafUVCanvas;
class UStaticMesh;
class UTexture2D;

struct FFoliageBakerLeafUVMaterialSectionOption
{
	int32 MaterialIndex = INDEX_NONE;
	FText DisplayName;
	FString MaterialIdentity;
};

struct FFoliageBakerLeafUVTextureOption
{
	TWeakObjectPtr<UTexture2D> Texture;
	FText DisplayName;
	FString TextureIdentity;
};

struct FFoliageBakerLeafUVTriangle
{
	FVector2f UVs[3] =
	{
		FVector2f::ZeroVector,
		FVector2f::ZeroVector,
		FVector2f::ZeroVector
	};
};

struct FFoliageBakerLeafUVIsland
{
	FString Signature;
	FBox2f Bounds = FBox2f(EForceInit::ForceInit);
	TArray<FFoliageBakerLeafUVTriangle> Triangles;
	TSet<FString> TriangleSignatures;
};

struct FFoliageBakerLeafUVAnnotation
{
	bool bHasPivot = false;
	bool bHasTip = false;
	FVector2f PivotUV = FVector2f::ZeroVector;
	FVector2f TipUV = FVector2f::ZeroVector;
};

enum class EFoliageBakerLeafUVEditMode : uint8
{
	SelectIsland,
	SetPivot,
	SetTip
};

DECLARE_DELEGATE_OneParam(
	FFoliageBakerResolvedLeavesChanged,
	const TArray<FFoliageBakerResolvedLeafCluster>&);
DECLARE_DELEGATE_OneParam(
	FFoliageBakerLeafMaterialChanged,
	int32);

class SFoliageBakerLeafUVPreview final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFoliageBakerLeafUVPreview)
	{
	}
		SLATE_EVENT(
			FFoliageBakerResolvedLeavesChanged,
			OnResolvedLeavesChanged)
		SLATE_EVENT(
			FFoliageBakerLeafMaterialChanged,
			OnLeafMaterialChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetSourceMesh(
		TWeakObjectPtr<UStaticMesh> InSourceStaticMesh,
		int32 InSourceLODIndex);
	void SetPreviewData(
		TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData);
	void ClearPreview();
	int32 GetSelectedMaterialIndex() const;
	bool CanResolveLeafOwnership() const;
	bool HasResolvedLeafOwnership() const;
	const TArray<FFoliageBakerResolvedLeafCluster>& GetResolvedLeafClusters() const;
	FReply ResolveLeafOwnership();

private:
	friend class SFoliageBakerLeafUVCanvas;

	void RebuildMaterialSectionOptions();
	void RebuildTextureOptions();
	void RebuildUVIslands();
	void HandleMaterialSectionSelectionChanged(
		TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> Option,
		ESelectInfo::Type SelectionType);
	void HandleTextureSelectionChanged(
		TSharedPtr<FFoliageBakerLeafUVTextureOption> Option,
		ESelectInfo::Type SelectionType);
	void HandleIslandSelectionChanged(
		TSharedPtr<int32> IslandIndex,
		ESelectInfo::Type SelectionType);
	TSharedRef<ITableRow> GenerateMaterialSectionRow(
		TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> Option,
		const TSharedRef<STableViewBase>& OwnerTable) const;
	TSharedRef<ITableRow> GenerateIslandRow(
		TSharedPtr<int32> IslandIndex,
		const TSharedRef<STableViewBase>& OwnerTable) const;
	TSharedRef<SWidget> GenerateTextureOptionWidget(
		TSharedPtr<FFoliageBakerLeafUVTextureOption> Option) const;
	FText GetStatusText() const;
	FText GetSelectedTextureText() const;
	FText GetSelectedIslandText() const;
	FText GetEditModeText() const;
	FReply SetEditMode(EFoliageBakerLeafUVEditMode NewMode);
	FReply ClearSelectedAnnotation();
	FReply HideSelectedIslands();
	FReply HideCompletedIslands();
	FReply ShowAllIslands();
	FReply ResetUVView();
	void HandleCanvasClick(
		const FVector2f& LocalPosition,
		const FVector2f& LocalSize,
		bool bAddToSelection);
	void HandleCanvasBoxSelection(
		const FVector2f& FirstLocalPosition,
		const FVector2f& SecondLocalPosition,
		const FVector2f& LocalSize,
		bool bAddToSelection);
	void ZoomUVViewAtLocal(
		const FVector2f& LocalPosition,
		const FVector2f& LocalSize,
		float WheelDelta);
	void PanUVViewByLocalDelta(
		const FVector2f& LocalDelta,
		const FVector2f& LocalSize);
	FSlateRect GetUVViewRect(const FVector2f& LocalSize) const;
	FBox2f GetVisibleUVBounds() const;
	FVector2f UVToLocal(const FVector2f& UV, const FVector2f& LocalSize) const;
	FVector2f LocalToUV(
		const FVector2f& LocalPosition,
		const FVector2f& LocalSize) const;
	int32 FindIslandAtUV(const FVector2f& UV) const;
	bool IsUVInsideIsland(int32 IslandIndex, const FVector2f& UV) const;
	bool DoesIslandIntersectUVRect(
		int32 IslandIndex,
		const FBox2f& UVRect) const;
	bool IsIslandHidden(int32 IslandIndex) const;
	TOptional<FFoliageBakerLeafUVAnnotation> FindAnnotation(
		int32 IslandIndex) const;
	TOptional<FFoliageBakerLeafUVAnnotation> FindSelectedAnnotation() const;
	FFoliageBakerLeafUVAnnotation& FindOrAddAnnotation(int32 IslandIndex);
	FString MakeAnnotationKey(const FFoliageBakerLeafUVIsland& Island) const;
	void RefreshVisibleIslandOptions();
	void ApplyIslandSelection(
		const TSet<int32>& NewSelection,
		int32 ActiveIslandIndex,
		bool bScrollActiveIntoView);
	void SelectIsland(
		int32 IslandIndex,
		bool bAddToSelection);
	void InvalidatePreview();
	void MarkLeafOwnershipDirty();
	void BroadcastSelectedResolvedLeaves();

	TWeakObjectPtr<UStaticMesh> SourceStaticMesh;
	int32 SourceLODIndex = 0;
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> PreviewData;
	TArray<FFoliageBakerResolvedLeafCluster> ResolvedLeafClusters;
	TArray<TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption>>
		MaterialSectionOptions;
	TSharedPtr<SListView<TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption>>>
		MaterialSectionList;
	TArray<TSharedPtr<FFoliageBakerLeafUVTextureOption>> TextureOptions;
	TSharedPtr<SComboBox<TSharedPtr<FFoliageBakerLeafUVTextureOption>>>
		TextureComboBox;
	TArray<FFoliageBakerLeafUVIsland> UVIslands;
	TArray<TSharedPtr<int32>> IslandOptions;
	TSharedPtr<SListView<TSharedPtr<int32>>> IslandList;
	TSharedPtr<SFoliageBakerLeafUVCanvas> UVCanvas;
	FFoliageBakerResolvedLeavesChanged OnResolvedLeavesChanged;
	FFoliageBakerLeafMaterialChanged OnLeafMaterialChanged;
	TMap<FString, FFoliageBakerLeafUVAnnotation> AnnotationsByKey;
	FBox2f UVViewBounds = FBox2f(EForceInit::ForceInit);
	FString SelectedMaterialIdentity;
	TSet<int32> SelectedIslandIndices;
	TSet<FString> HiddenIslandKeys;
	TStrongObjectPtr<UTexture2D> SelectedPreviewTexture;
	FSlateBrush PreviewTextureBrush;
	FText StatusText;
	FVector2f UVViewPan = FVector2f::ZeroVector;
	float UVViewZoom = 1.0f;
	int32 SelectedMaterialIndex = INDEX_NONE;
	int32 SelectedIslandIndex = INDEX_NONE;
	bool bUpdatingIslandListSelection = false;
	bool bLeafOwnershipDirty = true;
	EFoliageBakerLeafUVEditMode EditMode =
		EFoliageBakerLeafUVEditMode::SelectIsland;
};
