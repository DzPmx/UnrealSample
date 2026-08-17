#include "FoliageBakerLeafUVPreview.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FoliageBakerTreeHierarchyColorBaker.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Rendering/DrawElements.h"
#include "StaticMeshAttributes.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "SFoliageBakerLeafUVPreview"

namespace
{
	constexpr float UVCanvasPadding = 14.0f;
	constexpr float UVViewPaddingScale = 0.05f;
	const FLinearColor UVCanvasBackgroundColor(0.008f, 0.008f, 0.008f, 1.0f);
	const FLinearColor UVTextureTint(1.0f, 1.0f, 1.0f, 0.85f);
	const FLinearColor UVGridColor(0.18f, 0.18f, 0.18f, 0.85f);
	const FLinearColor UVIslandColor(0.30f, 0.55f, 0.36f, 0.85f);
	const FLinearColor CompletedUVIslandColor(0.20f, 0.72f, 0.88f, 0.95f);
	const FLinearColor MultiSelectedUVIslandColor(1.0f, 0.86f, 0.25f, 1.0f);
	const FLinearColor SelectedUVIslandColor(1.0f, 0.65f, 0.05f, 1.0f);
	const FLinearColor PivotColor(1.0f, 0.18f, 0.12f, 1.0f);
	const FLinearColor TipColor(0.10f, 0.82f, 1.0f, 1.0f);
	const FLinearColor MarqueeFillColor(0.12f, 0.46f, 1.0f, 0.16f);
	const FLinearColor MarqueeOutlineColor(0.28f, 0.66f, 1.0f, 1.0f);
	constexpr float BoxSelectionDragThreshold = 4.0f;
	constexpr float MinimumUVViewZoom = 0.25f;
	constexpr float MaximumUVViewZoom = 64.0f;
	constexpr float UVViewZoomStep = 1.2f;

	struct FUVBuildTriangle
	{
		FFoliageBakerLeafUVTriangle Triangle;
		FString Signature;
		FString EdgeSignatures[3];
	};

	class FUVIslandDisjointSet final
	{
	public:
		explicit FUVIslandDisjointSet(const int32 Count)
		{
			Parents.SetNumUninitialized(Count);
			Ranks.Init(0, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Parents[Index] = Index;
			}
		}

		int32 Find(const int32 Index)
		{
			if (Parents[Index] != Index)
			{
				Parents[Index] = Find(Parents[Index]);
			}
			return Parents[Index];
		}

		void Union(const int32 First, const int32 Second)
		{
			int32 FirstRoot = Find(First);
			int32 SecondRoot = Find(Second);
			if (FirstRoot == SecondRoot)
			{
				return;
			}
			if (Ranks[FirstRoot] < Ranks[SecondRoot])
			{
				Swap(FirstRoot, SecondRoot);
			}
			Parents[SecondRoot] = FirstRoot;
			if (Ranks[FirstRoot] == Ranks[SecondRoot])
			{
				++Ranks[FirstRoot];
			}
		}

	private:
		TArray<int32> Parents;
		TArray<uint8> Ranks;
	};

	FString MakeUVPointToken(const FVector2f& UV)
	{
		const float CanonicalU = UV.X == 0.0f ? 0.0f : UV.X;
		const float CanonicalV = UV.Y == 0.0f ? 0.0f : UV.Y;
		return FString::Printf(
			TEXT("%08x,%08x"),
			FMath::AsUInt(CanonicalU),
			FMath::AsUInt(CanonicalV));
	}

	FString MakeUVEdgeToken(FString FirstPoint, FString SecondPoint)
	{
		if (SecondPoint < FirstPoint)
		{
			Swap(FirstPoint, SecondPoint);
		}
		return FirstPoint + TEXT("|") + SecondPoint;
	}

	FUVBuildTriangle MakeUVBuildTriangle(
		const FVector2f& FirstUV,
		const FVector2f& SecondUV,
		const FVector2f& ThirdUV)
	{
		FUVBuildTriangle Result;
		Result.Triangle.UVs[0] = FirstUV;
		Result.Triangle.UVs[1] = SecondUV;
		Result.Triangle.UVs[2] = ThirdUV;

		FString PointTokens[3] =
		{
			MakeUVPointToken(FirstUV),
			MakeUVPointToken(SecondUV),
			MakeUVPointToken(ThirdUV)
		};
		Result.EdgeSignatures[0] = MakeUVEdgeToken(PointTokens[0], PointTokens[1]);
		Result.EdgeSignatures[1] = MakeUVEdgeToken(PointTokens[1], PointTokens[2]);
		Result.EdgeSignatures[2] = MakeUVEdgeToken(PointTokens[2], PointTokens[0]);
		if (PointTokens[1] < PointTokens[0])
		{
			Swap(PointTokens[0], PointTokens[1]);
		}
		if (PointTokens[2] < PointTokens[1])
		{
			Swap(PointTokens[1], PointTokens[2]);
		}
		if (PointTokens[1] < PointTokens[0])
		{
			Swap(PointTokens[0], PointTokens[1]);
		}
		Result.Signature = PointTokens[0]
			+ TEXT("|")
			+ PointTokens[1]
			+ TEXT("|")
			+ PointTokens[2];
		return Result;
	}

	bool IsPointInsideUVTriangle(
		const FVector2f& Point,
		const FFoliageBakerLeafUVTriangle& Triangle)
	{
		const float FirstCross = FVector2f::CrossProduct(
			Triangle.UVs[1] - Triangle.UVs[0],
			Point - Triangle.UVs[0]);
		const float SecondCross = FVector2f::CrossProduct(
			Triangle.UVs[2] - Triangle.UVs[1],
			Point - Triangle.UVs[1]);
		const float ThirdCross = FVector2f::CrossProduct(
			Triangle.UVs[0] - Triangle.UVs[2],
			Point - Triangle.UVs[2]);
		const bool bHasNegative =
			FirstCross < -UE_KINDA_SMALL_NUMBER
			|| SecondCross < -UE_KINDA_SMALL_NUMBER
			|| ThirdCross < -UE_KINDA_SMALL_NUMBER;
		const bool bHasPositive =
			FirstCross > UE_KINDA_SMALL_NUMBER
			|| SecondCross > UE_KINDA_SMALL_NUMBER
			|| ThirdCross > UE_KINDA_SMALL_NUMBER;
		return !(bHasNegative && bHasPositive);
	}

	bool IsPointInsideUVRect(
		const FVector2f& Point,
		const FBox2f& Rect)
	{
		return Point.X >= Rect.Min.X
			&& Point.X <= Rect.Max.X
			&& Point.Y >= Rect.Min.Y
			&& Point.Y <= Rect.Max.Y;
	}

	bool DoUVSegmentsIntersect(
		const FVector2f& FirstStart,
		const FVector2f& FirstEnd,
		const FVector2f& SecondStart,
		const FVector2f& SecondEnd)
	{
		FVector IntersectionPoint = FVector::ZeroVector;
		return FMath::SegmentIntersection2D(
			FVector(FirstStart.X, FirstStart.Y, 0.0),
			FVector(FirstEnd.X, FirstEnd.Y, 0.0),
			FVector(SecondStart.X, SecondStart.Y, 0.0),
			FVector(SecondEnd.X, SecondEnd.Y, 0.0),
			IntersectionPoint);
	}

	bool DoesUVTriangleIntersectRect(
		const FFoliageBakerLeafUVTriangle& Triangle,
		const FBox2f& Rect)
	{
		for (const FVector2f& UV : Triangle.UVs)
		{
			if (IsPointInsideUVRect(UV, Rect))
			{
				return true;
			}
		}
		const FVector2f RectCorners[4] =
		{
			Rect.Min,
			FVector2f(Rect.Max.X, Rect.Min.Y),
			Rect.Max,
			FVector2f(Rect.Min.X, Rect.Max.Y)
		};
		for (const FVector2f& Corner : RectCorners)
		{
			if (IsPointInsideUVTriangle(Corner, Triangle))
			{
				return true;
			}
		}
		for (int32 TriangleEdgeIndex = 0;
			TriangleEdgeIndex < 3;
			++TriangleEdgeIndex)
		{
			const FVector2f& TriangleEdgeStart =
				Triangle.UVs[TriangleEdgeIndex];
			const FVector2f& TriangleEdgeEnd =
				Triangle.UVs[(TriangleEdgeIndex + 1) % 3];
			for (int32 RectEdgeIndex = 0; RectEdgeIndex < 4; ++RectEdgeIndex)
			{
				if (DoUVSegmentsIntersect(
						TriangleEdgeStart,
						TriangleEdgeEnd,
						RectCorners[RectEdgeIndex],
						RectCorners[(RectEdgeIndex + 1) % 4]))
				{
					return true;
				}
			}
		}
		return false;
	}

	void DrawCrossMarker(
		FSlateWindowElementList& OutDrawElements,
		const FGeometry& Geometry,
		const int32 LayerId,
		const FVector2f& Position,
		const FLinearColor& Color)
	{
		TArray<FVector2f> HorizontalLine;
		HorizontalLine.Add(Position + FVector2f(-6.0f, 0.0f));
		HorizontalLine.Add(Position + FVector2f(6.0f, 0.0f));
		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			Geometry.ToPaintGeometry(),
			MoveTemp(HorizontalLine),
			ESlateDrawEffect::None,
			Color,
			true,
			2.0f);
		TArray<FVector2f> VerticalLine;
		VerticalLine.Add(Position + FVector2f(0.0f, -6.0f));
		VerticalLine.Add(Position + FVector2f(0.0f, 6.0f));
		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			Geometry.ToPaintGeometry(),
			MoveTemp(VerticalLine),
			ESlateDrawEffect::None,
			Color,
			true,
			2.0f);
	}
}

class SFoliageBakerLeafUVCanvas final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFoliageBakerLeafUVCanvas)
	{
	}
		SLATE_ARGUMENT(TWeakPtr<SFoliageBakerLeafUVPreview>, OwnerPreview)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OwnerPreview = InArgs._OwnerPreview;
		SetClipping(EWidgetClipping::ClipToBounds);
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		(void)LayoutScaleMultiplier;
		return FVector2D(480.0, 420.0);
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		const int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		const bool bParentEnabled) const override
	{
		(void)Args;
		(void)MyCullingRect;
		(void)InWidgetStyle;
		(void)bParentEnabled;
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			FCoreStyle::Get().GetBrush("WhiteBrush"),
			ESlateDrawEffect::None,
			UVCanvasBackgroundColor);

		const TSharedPtr<SFoliageBakerLeafUVPreview> Preview =
			OwnerPreview.Pin();
		if (!Preview)
		{
			return LayerId;
		}
		const FVector2f LocalSize(
			static_cast<float>(AllottedGeometry.GetLocalSize().X),
			static_cast<float>(AllottedGeometry.GetLocalSize().Y));
		if (Preview->SelectedPreviewTexture.IsValid())
		{
			const FVector2f TextureTopLeft = Preview->UVToLocal(
				FVector2f(0.0f, 0.0f),
				LocalSize);
			const FVector2f TextureBottomRight = Preview->UVToLocal(
				FVector2f(1.0f, 1.0f),
				LocalSize);
			const FVector2f TextureSize = TextureBottomRight - TextureTopLeft;
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId + 1,
				AllottedGeometry.ToPaintGeometry(
					TextureSize,
					FSlateLayoutTransform(TextureTopLeft)),
				&Preview->PreviewTextureBrush,
				ESlateDrawEffect::None,
				UVTextureTint);
		}

		for (int32 GridLineIndex = 0; GridLineIndex <= 10; ++GridLineIndex)
		{
			const float UVCoordinate = static_cast<float>(GridLineIndex) / 10.0f;
			TArray<FVector2f> VerticalLine;
			VerticalLine.Add(Preview->UVToLocal(FVector2f(UVCoordinate, 0.0f), LocalSize));
			VerticalLine.Add(Preview->UVToLocal(FVector2f(UVCoordinate, 1.0f), LocalSize));
			FSlateDrawElement::MakeLines(
				OutDrawElements,
				LayerId + 2,
				AllottedGeometry.ToPaintGeometry(),
				MoveTemp(VerticalLine),
				ESlateDrawEffect::None,
				UVGridColor,
				true,
				GridLineIndex == 0 || GridLineIndex == 10 ? 1.5f : 0.5f);
			TArray<FVector2f> HorizontalLine;
			HorizontalLine.Add(Preview->UVToLocal(FVector2f(0.0f, UVCoordinate), LocalSize));
			HorizontalLine.Add(Preview->UVToLocal(FVector2f(1.0f, UVCoordinate), LocalSize));
			FSlateDrawElement::MakeLines(
				OutDrawElements,
				LayerId + 2,
				AllottedGeometry.ToPaintGeometry(),
				MoveTemp(HorizontalLine),
				ESlateDrawEffect::None,
				UVGridColor,
				true,
				GridLineIndex == 0 || GridLineIndex == 10 ? 1.5f : 0.5f);
		}

		for (int32 IslandIndex = 0;
			IslandIndex < Preview->UVIslands.Num();
			++IslandIndex)
		{
			if (Preview->IsIslandHidden(IslandIndex))
			{
				continue;
			}
			const bool bActive = IslandIndex == Preview->SelectedIslandIndex;
			const bool bSelected =
				Preview->SelectedIslandIndices.Contains(IslandIndex);
			const TOptional<FFoliageBakerLeafUVAnnotation> Annotation =
				Preview->FindAnnotation(IslandIndex);
			const bool bCompleted = Annotation.IsSet()
				&& Annotation.GetValue().bHasPivot
				&& Annotation.GetValue().bHasTip;
			const FLinearColor DrawColor = bActive
				? SelectedUVIslandColor
				: bSelected
					? MultiSelectedUVIslandColor
					: bCompleted
						? CompletedUVIslandColor
						: UVIslandColor;
			const float LineThickness = bActive
				? 2.75f
				: bSelected || bCompleted
					? 2.0f
					: 1.0f;
			for (const FFoliageBakerLeafUVTriangle& Triangle :
				Preview->UVIslands[IslandIndex].Triangles)
			{
				TArray<FVector2f> TriangleOutline;
				TriangleOutline.Reserve(4);
				TriangleOutline.Add(Preview->UVToLocal(Triangle.UVs[0], LocalSize));
				TriangleOutline.Add(Preview->UVToLocal(Triangle.UVs[1], LocalSize));
				TriangleOutline.Add(Preview->UVToLocal(Triangle.UVs[2], LocalSize));
				TriangleOutline.Add(Preview->UVToLocal(Triangle.UVs[0], LocalSize));
				FSlateDrawElement::MakeLines(
					OutDrawElements,
					LayerId + 3,
					AllottedGeometry.ToPaintGeometry(),
					MoveTemp(TriangleOutline),
					ESlateDrawEffect::None,
					DrawColor,
					true,
					LineThickness);
			}

			if (Annotation.IsSet())
			{
				const FFoliageBakerLeafUVAnnotation& AnnotationValue =
					Annotation.GetValue();
				if (AnnotationValue.bHasPivot && AnnotationValue.bHasTip)
				{
					const FVector2f PivotPosition =
						Preview->UVToLocal(AnnotationValue.PivotUV, LocalSize);
					const FVector2f TipPosition =
						Preview->UVToLocal(AnnotationValue.TipUV, LocalSize);
					TArray<FVector2f> AxisLine;
					AxisLine.Add(PivotPosition);
					AxisLine.Add(TipPosition);
					FSlateDrawElement::MakeLines(
						OutDrawElements,
						LayerId + 4,
						AllottedGeometry.ToPaintGeometry(),
						MoveTemp(AxisLine),
						ESlateDrawEffect::None,
						bSelected
							? SelectedUVIslandColor
							: CompletedUVIslandColor,
						true,
						bSelected ? 2.0f : 1.25f);
				}
				if (AnnotationValue.bHasPivot)
				{
					DrawCrossMarker(
						OutDrawElements,
						AllottedGeometry,
						LayerId + 5,
						Preview->UVToLocal(AnnotationValue.PivotUV, LocalSize),
						PivotColor);
				}
				if (AnnotationValue.bHasTip)
				{
					DrawCrossMarker(
						OutDrawElements,
						AllottedGeometry,
						LayerId + 5,
						Preview->UVToLocal(AnnotationValue.TipUV, LocalSize),
						TipColor);
				}
			}
		}

		if (bBoxSelecting)
		{
			const FVector2f MarqueeMin(
				FMath::Min(SelectionStart.X, SelectionCurrent.X),
				FMath::Min(SelectionStart.Y, SelectionCurrent.Y));
			const FVector2f MarqueeMax(
				FMath::Max(SelectionStart.X, SelectionCurrent.X),
				FMath::Max(SelectionStart.Y, SelectionCurrent.Y));
			const FVector2f MarqueeSize = MarqueeMax - MarqueeMin;
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId + 6,
				AllottedGeometry.ToPaintGeometry(
					MarqueeSize,
					FSlateLayoutTransform(MarqueeMin)),
				FCoreStyle::Get().GetBrush("WhiteBrush"),
				ESlateDrawEffect::None,
				MarqueeFillColor);
			TArray<FVector2f> MarqueeOutline;
			MarqueeOutline.Add(MarqueeMin);
			MarqueeOutline.Add(FVector2f(MarqueeMax.X, MarqueeMin.Y));
			MarqueeOutline.Add(MarqueeMax);
			MarqueeOutline.Add(FVector2f(MarqueeMin.X, MarqueeMax.Y));
			MarqueeOutline.Add(MarqueeMin);
			FSlateDrawElement::MakeLines(
				OutDrawElements,
				LayerId + 7,
				AllottedGeometry.ToPaintGeometry(),
				MoveTemp(MarqueeOutline),
				ESlateDrawEffect::None,
				MarqueeOutlineColor,
				true,
				1.5f);
		}
		return LayerId + 7;
	}

	virtual FReply OnMouseButtonDown(
		const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		const TSharedPtr<SFoliageBakerLeafUVPreview> Preview =
			OwnerPreview.Pin();
		if (!Preview)
		{
			return FReply::Unhandled();
		}
		const FVector2D LocalPositionDouble = MyGeometry.AbsoluteToLocal(
			MouseEvent.GetScreenSpacePosition());
		const FVector2f LocalPosition(
			static_cast<float>(LocalPositionDouble.X),
			static_cast<float>(LocalPositionDouble.Y));
		if (MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton)
		{
			bPanning = true;
			LastPointerPosition = LocalPosition;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		const FVector2f LocalSize(
			static_cast<float>(MyGeometry.GetLocalSize().X),
			static_cast<float>(MyGeometry.GetLocalSize().Y));
		if (Preview->EditMode == EFoliageBakerLeafUVEditMode::SelectIsland)
		{
			bSelecting = true;
			bBoxSelecting = false;
			bAddToSelection =
				MouseEvent.IsControlDown() || MouseEvent.IsShiftDown();
			SelectionStart = LocalPosition;
			SelectionCurrent = LocalPosition;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		Preview->HandleCanvasClick(LocalPosition, LocalSize, false);
		return FReply::Handled();
	}

	virtual FReply OnMouseMove(
		const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		const FVector2D LocalPositionDouble = MyGeometry.AbsoluteToLocal(
			MouseEvent.GetScreenSpacePosition());
		const FVector2f LocalPosition(
			static_cast<float>(LocalPositionDouble.X),
			static_cast<float>(LocalPositionDouble.Y));
		const FVector2f LocalSize(
			static_cast<float>(MyGeometry.GetLocalSize().X),
			static_cast<float>(MyGeometry.GetLocalSize().Y));
		if (bPanning)
		{
			if (const TSharedPtr<SFoliageBakerLeafUVPreview> Preview =
				OwnerPreview.Pin())
			{
				Preview->PanUVViewByLocalDelta(
					LocalPosition - LastPointerPosition,
					LocalSize);
			}
			LastPointerPosition = LocalPosition;
			return FReply::Handled();
		}
		if (bSelecting)
		{
			SelectionCurrent = LocalPosition;
			bBoxSelecting =
				(SelectionCurrent - SelectionStart).SizeSquared()
				> FMath::Square(BoxSelectionDragThreshold);
			Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(
		const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton
			&& bPanning)
		{
			bPanning = false;
			return FReply::Handled().ReleaseMouseCapture();
		}
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton
			|| !bSelecting)
		{
			return FReply::Unhandled();
		}
		const FVector2D LocalPositionDouble = MyGeometry.AbsoluteToLocal(
			MouseEvent.GetScreenSpacePosition());
		SelectionCurrent = FVector2f(
			static_cast<float>(LocalPositionDouble.X),
			static_cast<float>(LocalPositionDouble.Y));
		bBoxSelecting = bBoxSelecting
			|| (SelectionCurrent - SelectionStart).SizeSquared()
				> FMath::Square(BoxSelectionDragThreshold);
		const FVector2f LocalSize(
			static_cast<float>(MyGeometry.GetLocalSize().X),
			static_cast<float>(MyGeometry.GetLocalSize().Y));
		if (const TSharedPtr<SFoliageBakerLeafUVPreview> Preview =
			OwnerPreview.Pin())
		{
			if (bBoxSelecting)
			{
				Preview->HandleCanvasBoxSelection(
					SelectionStart,
					SelectionCurrent,
					LocalSize,
					bAddToSelection);
			}
			else
			{
				Preview->HandleCanvasClick(
					SelectionCurrent,
					LocalSize,
					bAddToSelection);
			}
		}
		bSelecting = false;
		bBoxSelecting = false;
		Invalidate(EInvalidateWidgetReason::Paint);
		return FReply::Handled().ReleaseMouseCapture();
	}

	virtual FReply OnMouseWheel(
		const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override
	{
		if (const TSharedPtr<SFoliageBakerLeafUVPreview> Preview =
			OwnerPreview.Pin())
		{
			const FVector2D LocalPositionDouble = MyGeometry.AbsoluteToLocal(
				MouseEvent.GetScreenSpacePosition());
			Preview->ZoomUVViewAtLocal(
				FVector2f(
					static_cast<float>(LocalPositionDouble.X),
					static_cast<float>(LocalPositionDouble.Y)),
				FVector2f(
					static_cast<float>(MyGeometry.GetLocalSize().X),
					static_cast<float>(MyGeometry.GetLocalSize().Y)),
				MouseEvent.GetWheelDelta());
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual void OnMouseCaptureLost(
		const FCaptureLostEvent& CaptureLostEvent) override
	{
		(void)CaptureLostEvent;
		bPanning = false;
		bSelecting = false;
		bBoxSelecting = false;
		Invalidate(EInvalidateWidgetReason::Paint);
	}

private:
	TWeakPtr<SFoliageBakerLeafUVPreview> OwnerPreview;
	FVector2f LastPointerPosition = FVector2f::ZeroVector;
	FVector2f SelectionStart = FVector2f::ZeroVector;
	FVector2f SelectionCurrent = FVector2f::ZeroVector;
	bool bPanning = false;
	bool bSelecting = false;
	bool bBoxSelecting = false;
	bool bAddToSelection = false;
};

void SFoliageBakerLeafUVPreview::Construct(const FArguments& InArgs)
{
	OnResolvedLeavesChanged = InArgs._OnResolvedLeavesChanged;
	OnLeafMaterialChanged = InArgs._OnLeafMaterialChanged;
	SAssignNew(UVCanvas, SFoliageBakerLeafUVCanvas)
		.OwnerPreview(SharedThis(this));

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.0f, 4.0f, 6.0f, 6.0f)
		[
			SNew(SBox)
			.WidthOverride(230.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f, 2.0f, 2.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MaterialSectionListTitle", "Material Sections"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.FillHeight(0.52f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					[
						SAssignNew(
							MaterialSectionList,
							SListView<TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption>>)
						.ListItemsSource(&MaterialSectionOptions)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SFoliageBakerLeafUVPreview::GenerateMaterialSectionRow)
						.OnSelectionChanged(
							this,
							&SFoliageBakerLeafUVPreview::HandleMaterialSectionSelectionChanged)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f, 8.0f, 2.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LeafTextureTitle", "Leaf Texture Overlay"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(
						TextureComboBox,
						SComboBox<TSharedPtr<FFoliageBakerLeafUVTextureOption>>)
					.OptionsSource(&TextureOptions)
					.OnGenerateWidget(
						this,
						&SFoliageBakerLeafUVPreview::GenerateTextureOptionWidget)
					.OnSelectionChanged(
						this,
						&SFoliageBakerLeafUVPreview::HandleTextureSelectionChanged)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return GetSelectedTextureText(); })
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f, 8.0f, 2.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UVIslandListTitle", "UV0 Islands"))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.FillHeight(0.48f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					[
						SAssignNew(IslandList, SListView<TSharedPtr<int32>>)
						.ListItemsSource(&IslandOptions)
						.SelectionMode(ESelectionMode::Multi)
						.OnGenerateRow(this, &SFoliageBakerLeafUVPreview::GenerateIslandRow)
						.OnSelectionChanged(
							this,
							&SFoliageBakerLeafUVPreview::HandleIslandSelectionChanged)
					]
				]
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0.0f, 4.0f, 6.0f, 6.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(4.0f, 4.0f))
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("SelectIslandMode", "Select Island"))
					.OnClicked_Lambda(
						[this]()
						{
							return SetEditMode(EFoliageBakerLeafUVEditMode::SelectIsland);
						})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("SetPivotMode", "Set Pivot"))
					.OnClicked_Lambda(
						[this]()
						{
							return SetEditMode(EFoliageBakerLeafUVEditMode::SetPivot);
						})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("SetTipMode", "Set Tip"))
					.OnClicked_Lambda(
						[this]()
						{
							return SetEditMode(EFoliageBakerLeafUVEditMode::SetTip);
						})
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ClearLeafUVPoints", "Clear Points"))
					.OnClicked(this, &SFoliageBakerLeafUVPreview::ClearSelectedAnnotation)
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ResetLeafUVView", "Reset View"))
					.OnClicked(this, &SFoliageBakerLeafUVPreview::ResetUVView)
				]
				+ SWrapBox::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetEditModeText(); })
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(4.0f, 4.0f))
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("HideSelectedLeafUV", "Hide Selected"))
					.OnClicked(this, &SFoliageBakerLeafUVPreview::HideSelectedIslands)
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("HideCompletedLeafUV", "Hide Completed"))
					.OnClicked(this, &SFoliageBakerLeafUVPreview::HideCompletedIslands)
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowAllLeafUV", "Show All"))
					.OnClicked(this, &SFoliageBakerLeafUVPreview::ShowAllIslands)
				]
				+ SWrapBox::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT(
						"LeafUVNavigationHelp",
						"Wheel: Zoom  |  MMB Drag: Pan  |  LMB Drag: Box Select  |  Ctrl/Shift: Add"))
					.AutoWrapText(true)
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(0.0f)
				[
					UVCanvas.ToSharedRef()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 5.0f, 2.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return GetSelectedIslandText(); })
				.TextStyle(FAppStyle::Get(), "SmallText")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 3.0f, 2.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return GetStatusText(); })
				.AutoWrapText(true)
				.TextStyle(FAppStyle::Get(), "SmallText")
			]
		]
	];

	ClearPreview();
}

void SFoliageBakerLeafUVPreview::SetSourceMesh(
	TWeakObjectPtr<UStaticMesh> InSourceStaticMesh,
	const int32 InSourceLODIndex)
{
	const bool bSameSource = SourceStaticMesh == InSourceStaticMesh
		&& SourceLODIndex == InSourceLODIndex;
	const FString PreferredMaterialIdentity = bSameSource
		? SelectedMaterialIdentity
		: FString();
	SourceStaticMesh = MoveTemp(InSourceStaticMesh);
	SourceLODIndex = InSourceLODIndex;
	PreviewData.Reset();
	ResolvedLeafClusters.Reset();
	bLeafOwnershipDirty = true;
	SelectedMaterialIndex = INDEX_NONE;
	SelectedIslandIndex = INDEX_NONE;
	SelectedIslandIndices.Reset();
	UVViewPan = FVector2f::ZeroVector;
	UVViewZoom = 1.0f;
	UVIslands.Reset();
	IslandOptions.Reset();
	UVViewBounds.Init();
	if (!bSameSource)
	{
		SelectedMaterialIdentity.Reset();
		HiddenIslandKeys.Reset();
	}
	if (TextureComboBox.IsValid())
	{
		TextureComboBox->ClearSelection();
	}
	TextureOptions.Reset();
	PreviewTextureBrush.SetResourceObject(nullptr);
	SelectedPreviewTexture.Reset();
	if (TextureComboBox.IsValid())
	{
		TextureComboBox->RefreshOptions();
	}

	RebuildMaterialSectionOptions();
	TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> PreferredOption;
	for (const TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption>& Option :
		MaterialSectionOptions)
	{
		if (Option.IsValid()
			&& Option->MaterialIdentity == PreferredMaterialIdentity)
		{
			PreferredOption = Option;
			break;
		}
	}
	if (MaterialSectionList.IsValid())
	{
		MaterialSectionList->ClearSelection();
		MaterialSectionList->RequestListRefresh();
		if (PreferredOption.IsValid())
		{
			MaterialSectionList->SetSelection(
				PreferredOption,
				ESelectInfo::Direct);
		}
	}
	if (IslandList.IsValid())
	{
		IslandList->ClearSelection();
		IslandList->RequestListRefresh();
	}
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
}

void SFoliageBakerLeafUVPreview::SetPreviewData(
	TSharedPtr<const FFoliageBakerTreeHierarchyPreviewData> InPreviewData)
{
	PreviewData = MoveTemp(InPreviewData);
	ResolvedLeafClusters.Reset();
	bLeafOwnershipDirty = true;
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
}

void SFoliageBakerLeafUVPreview::ClearPreview()
{
	SetSourceMesh(nullptr, 0);
	StatusText = LOCTEXT(
		"LeafUVNoPreview",
		"Choose a Static Mesh above, select its Leaf Material Section, then Analyze Hierarchy.");
}

int32 SFoliageBakerLeafUVPreview::GetSelectedMaterialIndex() const
{
	return SelectedMaterialIndex;
}

bool SFoliageBakerLeafUVPreview::CanResolveLeafOwnership() const
{
	return PreviewData.IsValid()
		&& SourceStaticMesh.IsValid()
		&& SelectedMaterialIndex != INDEX_NONE;
}

bool SFoliageBakerLeafUVPreview::HasResolvedLeafOwnership() const
{
	return PreviewData.IsValid()
		&& SourceStaticMesh.IsValid()
		&& !bLeafOwnershipDirty;
}

const TArray<FFoliageBakerResolvedLeafCluster>&
SFoliageBakerLeafUVPreview::GetResolvedLeafClusters() const
{
	return ResolvedLeafClusters;
}

void SFoliageBakerLeafUVPreview::RebuildMaterialSectionOptions()
{
	MaterialSectionOptions.Reset();
	if (!SourceStaticMesh.IsValid())
	{
		StatusText = LOCTEXT(
			"LeafUVNoSourceMesh",
			"The Hierarchy View has no current Static Mesh.");
		return;
	}

	const TObjectPtr<UStaticMesh> StaticMesh = SourceStaticMesh.Get();
	const TArray<FStaticMaterial>& StaticMaterials =
		StaticMesh->GetStaticMaterials();
	MaterialSectionOptions.Reserve(StaticMaterials.Num());
	for (int32 MaterialIndex = 0;
		MaterialIndex < StaticMaterials.Num();
		++MaterialIndex)
	{
		const FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
		const FString MaterialName = StaticMaterial.MaterialInterface
			? StaticMaterial.MaterialInterface->GetName()
			: TEXT("None");
		TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> Option =
			MakeShared<FFoliageBakerLeafUVMaterialSectionOption>();
		Option->MaterialIndex = MaterialIndex;
		Option->DisplayName = FText::FromString(FString::Printf(
			TEXT("[%d] %s  (%s)"),
			MaterialIndex,
			*StaticMaterial.MaterialSlotName.ToString(),
			*MaterialName));
		const FString MaterialPath = StaticMaterial.MaterialInterface
			? StaticMaterial.MaterialInterface->GetPathName()
			: TEXT("None");
		Option->MaterialIdentity = FString::Printf(
			TEXT("%d|%s|%s"),
			MaterialIndex,
			*StaticMaterial.MaterialSlotName.ToString(),
			*MaterialPath);
		MaterialSectionOptions.Add(MoveTemp(Option));
	}
	StatusText = MaterialSectionOptions.IsEmpty()
		? LOCTEXT("LeafUVNoMaterials", "The current Static Mesh has no material slots.")
		: LOCTEXT(
			"LeafUVSelectMaterial",
			"Select the leaf Material Section to inspect only that section's UV0 islands.");
}

void SFoliageBakerLeafUVPreview::RebuildTextureOptions()
{
	if (TextureComboBox.IsValid())
	{
		TextureComboBox->ClearSelection();
	}
	TextureOptions.Reset();
	PreviewTextureBrush.SetResourceObject(nullptr);
	SelectedPreviewTexture.Reset();

	if (!SourceStaticMesh.IsValid()
		|| SelectedMaterialIndex == INDEX_NONE)
	{
		if (TextureComboBox.IsValid())
		{
			TextureComboBox->RefreshOptions();
		}
		return;
	}

	const TObjectPtr<UStaticMesh> StaticMesh = SourceStaticMesh.Get();
	const TArray<FStaticMaterial>& StaticMaterials =
		StaticMesh->GetStaticMaterials();
	if (!StaticMaterials.IsValidIndex(SelectedMaterialIndex)
		|| !StaticMaterials[SelectedMaterialIndex].MaterialInterface)
	{
		if (TextureComboBox.IsValid())
		{
			TextureComboBox->RefreshOptions();
		}
		return;
	}

	const TObjectPtr<UMaterialInterface> MaterialInterface =
		StaticMaterials[SelectedMaterialIndex].MaterialInterface;
	TSet<FString> AddedTextureIdentities;
	const auto AddTextureOption =
		[this, &AddedTextureIdentities](const TObjectPtr<UTexture2D>& Texture)
		{
			if (!Texture)
			{
				return;
			}
			const FString TextureIdentity = Texture->GetPathName();
			if (AddedTextureIdentities.Contains(TextureIdentity))
			{
				return;
			}
			AddedTextureIdentities.Add(TextureIdentity);

			TSharedPtr<FFoliageBakerLeafUVTextureOption> Option =
				MakeShared<FFoliageBakerLeafUVTextureOption>();
			Option->Texture = Texture.Get();
			Option->TextureIdentity = TextureIdentity;
			Option->DisplayName = FText::FromString(FString::Printf(
				TEXT("%s  (%d x %d)"),
				*Texture->GetName(),
				Texture->GetSizeX(),
				Texture->GetSizeY()));
			TextureOptions.Add(MoveTemp(Option));
		};
	for (const TObjectPtr<UObject>& ReferencedObject :
		MaterialInterface->GetReferencedTextures())
	{
		const TObjectPtr<UTexture2D> Texture =
			Cast<UTexture2D>(ReferencedObject.Get());
		AddTextureOption(Texture);
	}

	TArray<FMaterialParameterInfo> TextureParameterInfos;
	TArray<FGuid> TextureParameterIds;
	MaterialInterface->GetAllTextureParameterInfo(
		TextureParameterInfos,
		TextureParameterIds);
	for (const FMaterialParameterInfo& ParameterInfo : TextureParameterInfos)
	{
		// UMaterialInterface's API requires a raw output at this engine boundary.
		// Convert it immediately and do not retain the raw pointer in tool state.
		UTexture* ParameterTextureAtEngineBoundary = nullptr;
		if (MaterialInterface->GetTextureParameterValue(
				ParameterInfo,
				ParameterTextureAtEngineBoundary))
		{
			const TObjectPtr<UTexture2D> ParameterTexture =
				Cast<UTexture2D>(ParameterTextureAtEngineBoundary);
			AddTextureOption(ParameterTexture);
		}
	}
	TextureOptions.Sort(
		[](const TSharedPtr<FFoliageBakerLeafUVTextureOption>& First,
			const TSharedPtr<FFoliageBakerLeafUVTextureOption>& Second)
		{
			return First->TextureIdentity < Second->TextureIdentity;
		});

	if (TextureComboBox.IsValid())
	{
		TextureComboBox->RefreshOptions();
		if (TextureOptions.Num() == 1)
		{
			TextureComboBox->SetSelectedItem(TextureOptions[0]);
		}
	}
}

void SFoliageBakerLeafUVPreview::RebuildUVIslands()
{
	UVIslands.Reset();
	IslandOptions.Reset();
	SelectedIslandIndex = INDEX_NONE;
	SelectedIslandIndices.Reset();
	UVViewPan = FVector2f::ZeroVector;
	UVViewZoom = 1.0f;
	UVViewBounds.Init();
	if (IslandList.IsValid())
	{
		IslandList->ClearSelection();
		IslandList->RequestListRefresh();
	}
	if (!SourceStaticMesh.IsValid()
		|| SelectedMaterialIndex == INDEX_NONE)
	{
		return;
	}

	const TObjectPtr<UStaticMesh> StaticMesh = SourceStaticMesh.Get();
	if (!StaticMesh->IsSourceModelValid(SourceLODIndex))
	{
		StatusText = FText::Format(
			LOCTEXT("LeafUVInvalidLOD", "Source LOD {0} is not available."),
			FText::AsNumber(SourceLODIndex));
		return;
	}
	if (!StaticMesh->GetMeshDescription(SourceLODIndex))
	{
		StatusText = FText::Format(
			LOCTEXT(
				"LeafUVMissingMeshDescription",
				"Source LOD {0} has no editable Mesh Description."),
			FText::AsNumber(SourceLODIndex));
		return;
	}
	const FMeshDescription& MeshDescription =
		*StaticMesh->GetMeshDescription(SourceLODIndex);
	const FStaticMeshConstAttributes Attributes(MeshDescription);
	const TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs =
		Attributes.GetVertexInstanceUVs();
	if (VertexInstanceUVs.GetNumChannels() <= 0)
	{
		StatusText = LOCTEXT(
			"LeafUVMissingUV0",
			"The selected source LOD has no UV0 channel. Data Bake is unchanged; only this preview is unavailable.");
		return;
	}
	if (!MeshDescription.PolygonGroupAttributes().HasAttribute(
			MeshAttribute::PolygonGroup::ImportedMaterialSlotName))
	{
		StatusText = LOCTEXT(
			"LeafUVMissingMaterialGroups",
			"The selected source LOD has no polygon-group material slot names.");
		return;
	}
	const TPolygonGroupAttributesConstRef<FName> MaterialSlotNames =
		Attributes.GetPolygonGroupMaterialSlotNames();

	TArray<FUVBuildTriangle> BuildTriangles;
	TSet<FString> AddedTriangleSignatures;
	for (const FTriangleID TriangleID :
		MeshDescription.Triangles().GetElementIDs())
	{
		const FPolygonGroupID PolygonGroupID =
			MeshDescription.GetTrianglePolygonGroup(TriangleID);
		if (!MeshDescription.IsPolygonGroupValid(PolygonGroupID))
		{
			continue;
		}
		const FName MaterialSlotName = MaterialSlotNames[PolygonGroupID];
		int32 TriangleMaterialIndex =
			StaticMesh->GetMaterialIndex(MaterialSlotName);
		if (TriangleMaterialIndex == INDEX_NONE)
		{
			TriangleMaterialIndex =
				StaticMesh->GetMaterialIndexFromImportedMaterialSlotName(
					MaterialSlotName);
		}
		if (TriangleMaterialIndex != SelectedMaterialIndex)
		{
			continue;
		}
		const FVertexInstanceID FirstVertexInstanceID =
			MeshDescription.GetTriangleVertexInstance(TriangleID, 0);
		const FVertexInstanceID SecondVertexInstanceID =
			MeshDescription.GetTriangleVertexInstance(TriangleID, 1);
		const FVertexInstanceID ThirdVertexInstanceID =
			MeshDescription.GetTriangleVertexInstance(TriangleID, 2);
		FUVBuildTriangle BuildTriangle = MakeUVBuildTriangle(
			VertexInstanceUVs.Get(FirstVertexInstanceID, 0),
			VertexInstanceUVs.Get(SecondVertexInstanceID, 0),
			VertexInstanceUVs.Get(ThirdVertexInstanceID, 0));
		if (FVector2f::CrossProduct(
				BuildTriangle.Triangle.UVs[1] - BuildTriangle.Triangle.UVs[0],
				BuildTriangle.Triangle.UVs[2] - BuildTriangle.Triangle.UVs[0])
			== 0.0f)
		{
			continue;
		}
		if (!AddedTriangleSignatures.Contains(BuildTriangle.Signature))
		{
			AddedTriangleSignatures.Add(BuildTriangle.Signature);
			BuildTriangles.Add(MoveTemp(BuildTriangle));
		}
	}
	if (BuildTriangles.IsEmpty())
	{
		StatusText = LOCTEXT(
			"LeafUVNoSectionTriangles",
			"The selected Material Section has no non-degenerate UV0 triangles in this source LOD.");
		return;
	}

	FUVIslandDisjointSet DisjointSet(BuildTriangles.Num());
	TMap<FString, int32> FirstTriangleByEdge;
	for (int32 TriangleIndex = 0;
		TriangleIndex < BuildTriangles.Num();
		++TriangleIndex)
	{
		for (const FString& EdgeSignature :
			BuildTriangles[TriangleIndex].EdgeSignatures)
		{
			if (FirstTriangleByEdge.Contains(EdgeSignature))
			{
				DisjointSet.Union(
					TriangleIndex,
					FirstTriangleByEdge.FindRef(EdgeSignature));
			}
			else
			{
				FirstTriangleByEdge.Add(EdgeSignature, TriangleIndex);
			}
		}
	}

	TMap<int32, int32> IslandIndexByRoot;
	TArray<TArray<FString>> TriangleSignaturesByIsland;
	for (int32 TriangleIndex = 0;
		TriangleIndex < BuildTriangles.Num();
		++TriangleIndex)
	{
		const int32 RootIndex = DisjointSet.Find(TriangleIndex);
		int32 IslandIndex = INDEX_NONE;
		if (IslandIndexByRoot.Contains(RootIndex))
		{
			IslandIndex = IslandIndexByRoot.FindRef(RootIndex);
		}
		else
		{
			IslandIndex = UVIslands.AddDefaulted();
			IslandIndexByRoot.Add(RootIndex, IslandIndex);
			TriangleSignaturesByIsland.AddDefaulted();
		}
		FFoliageBakerLeafUVIsland& Island = UVIslands[IslandIndex];
		Island.Triangles.Add(BuildTriangles[TriangleIndex].Triangle);
		Island.TriangleSignatures.Add(BuildTriangles[TriangleIndex].Signature);
		TriangleSignaturesByIsland[IslandIndex].Add(
			BuildTriangles[TriangleIndex].Signature);
		for (const FVector2f& UV : BuildTriangles[TriangleIndex].Triangle.UVs)
		{
			Island.Bounds += UV;
		}
	}
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		TriangleSignaturesByIsland[IslandIndex].Sort();
		const FString CanonicalIslandSignature = FString::Join(
			TriangleSignaturesByIsland[IslandIndex],
			TEXT(";"));
		UVIslands[IslandIndex].Signature = CanonicalIslandSignature;
	}
	UVIslands.Sort(
		[](const FFoliageBakerLeafUVIsland& First,
			const FFoliageBakerLeafUVIsland& Second)
		{
			if (First.Bounds.Min.Y != Second.Bounds.Min.Y)
			{
				return First.Bounds.Min.Y < Second.Bounds.Min.Y;
			}
			if (First.Bounds.Min.X != Second.Bounds.Min.X)
			{
				return First.Bounds.Min.X < Second.Bounds.Min.X;
			}
			return First.Signature < Second.Signature;
		});

	UVViewBounds += FVector2f(0.0f, 0.0f);
	UVViewBounds += FVector2f(1.0f, 1.0f);
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		UVViewBounds += UVIslands[IslandIndex].Bounds.Min;
		UVViewBounds += UVIslands[IslandIndex].Bounds.Max;
	}
	const FVector2f Extent = UVViewBounds.Max - UVViewBounds.Min;
	const FVector2f Padding(
		FMath::Max(Extent.X * UVViewPaddingScale, 0.01f),
		FMath::Max(Extent.Y * UVViewPaddingScale, 0.01f));
	UVViewBounds.Min -= Padding;
	UVViewBounds.Max += Padding;
	RefreshVisibleIslandOptions();
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVIslandCount",
			"UV0: {0} unique triangle(s), {1} selectable island(s). Overlapping repeated templates are merged by their UV0 coordinates."),
		FText::AsNumber(BuildTriangles.Num()),
		FText::AsNumber(UVIslands.Num()));
}

void SFoliageBakerLeafUVPreview::HandleMaterialSectionSelectionChanged(
	TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> Option,
	const ESelectInfo::Type SelectionType)
{
	SelectedMaterialIndex = Option.IsValid()
		? Option->MaterialIndex
		: INDEX_NONE;
	SelectedMaterialIdentity = Option.IsValid()
		? Option->MaterialIdentity
		: FString();
	PreviewData.Reset();
	ResolvedLeafClusters.Reset();
	bLeafOwnershipDirty = true;
	RebuildTextureOptions();
	RebuildUVIslands();
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
	if (SelectionType != ESelectInfo::Direct)
	{
		OnLeafMaterialChanged.ExecuteIfBound(SelectedMaterialIndex);
	}
}

void SFoliageBakerLeafUVPreview::HandleTextureSelectionChanged(
	TSharedPtr<FFoliageBakerLeafUVTextureOption> Option,
	const ESelectInfo::Type SelectionType)
{
	(void)SelectionType;
	const TObjectPtr<UTexture2D> Texture = Option.IsValid()
		? Option->Texture.Get()
		: nullptr;
	PreviewTextureBrush.SetResourceObject(nullptr);
	SelectedPreviewTexture.Reset(Texture.Get());
	PreviewTextureBrush.SetResourceObject(SelectedPreviewTexture.Get());
	if (SelectedPreviewTexture.IsValid())
	{
		PreviewTextureBrush.SetImageSize(FVector2D(
			SelectedPreviewTexture->GetSizeX(),
			SelectedPreviewTexture->GetSizeY()));
	}
	InvalidatePreview();
}

void SFoliageBakerLeafUVPreview::HandleIslandSelectionChanged(
	TSharedPtr<int32> IslandIndex,
	const ESelectInfo::Type SelectionType)
{
	(void)SelectionType;
	if (bUpdatingIslandListSelection || !IslandList.IsValid())
	{
		return;
	}
	SelectedIslandIndices.Reset();
	for (const TSharedPtr<int32>& SelectedItem :
		IslandList->GetSelectedItems())
	{
		if (SelectedItem.IsValid()
			&& UVIslands.IsValidIndex(*SelectedItem)
			&& !IsIslandHidden(*SelectedItem))
		{
			SelectedIslandIndices.Add(*SelectedItem);
		}
	}
	if (IslandIndex.IsValid()
		&& SelectedIslandIndices.Contains(*IslandIndex))
	{
		SelectedIslandIndex = *IslandIndex;
	}
	else if (!SelectedIslandIndices.Contains(SelectedIslandIndex))
	{
		SelectedIslandIndex = INDEX_NONE;
		for (const int32 SelectedIndex : SelectedIslandIndices)
		{
			if (SelectedIslandIndex == INDEX_NONE
				|| SelectedIndex < SelectedIslandIndex)
			{
				SelectedIslandIndex = SelectedIndex;
			}
		}
	}
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
}

TSharedRef<ITableRow> SFoliageBakerLeafUVPreview::GenerateMaterialSectionRow(
	TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption> Option,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(
		STableRow<TSharedPtr<FFoliageBakerLeafUVMaterialSectionOption>>,
		OwnerTable)
	[
		SNew(STextBlock)
		.Text(Option.IsValid() ? Option->DisplayName : FText::GetEmpty())
	];
}

TSharedRef<ITableRow> SFoliageBakerLeafUVPreview::GenerateIslandRow(
	TSharedPtr<int32> IslandIndex,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	FText RowText = FText::GetEmpty();
	if (IslandIndex.IsValid() && UVIslands.IsValidIndex(*IslandIndex))
	{
		const FFoliageBakerLeafUVIsland& Island = UVIslands[*IslandIndex];
		const FString AnnotationKey = MakeAnnotationKey(Island);
		const bool bHasAnnotation = AnnotationsByKey.Contains(AnnotationKey);
		const FFoliageBakerLeafUVAnnotation Annotation = bHasAnnotation
			? AnnotationsByKey.FindRef(AnnotationKey)
			: FFoliageBakerLeafUVAnnotation();
		FString MarkerState;
		if (bHasAnnotation && Annotation.bHasPivot && Annotation.bHasTip)
		{
			MarkerState = TEXT("  [Done: P+T]");
		}
		else if (bHasAnnotation && Annotation.bHasPivot)
		{
			MarkerState = TEXT("  [P]");
		}
		else if (bHasAnnotation && Annotation.bHasTip)
		{
			MarkerState = TEXT("  [T]");
		}
		RowText = FText::FromString(FString::Printf(
			TEXT("Island %d  (%d tri)%s"),
			*IslandIndex,
			Island.Triangles.Num(),
			*MarkerState));
	}
	return SNew(STableRow<TSharedPtr<int32>>, OwnerTable)
	[
		SNew(STextBlock)
		.Text(RowText)
	];
}

TSharedRef<SWidget> SFoliageBakerLeafUVPreview::GenerateTextureOptionWidget(
	TSharedPtr<FFoliageBakerLeafUVTextureOption> Option) const
{
	return SNew(STextBlock)
		.Text(Option.IsValid() ? Option->DisplayName : FText::GetEmpty());
}

FText SFoliageBakerLeafUVPreview::GetStatusText() const
{
	if (bLeafOwnershipDirty && PreviewData.IsValid())
	{
		return FText::Format(
			LOCTEXT(
				"LeafOwnershipNeedsResolve",
				"{0}\nLeaf ownership is out of date. Click Resolve Leaf Ownership after completing the desired PivotUV/TipUV templates."),
			StatusText);
	}
	return StatusText;
}

FText SFoliageBakerLeafUVPreview::GetSelectedTextureText() const
{
	if (SelectedPreviewTexture.IsValid())
	{
		return FText::FromString(FString::Printf(
			TEXT("%s  (%d x %d)"),
			*SelectedPreviewTexture->GetName(),
			SelectedPreviewTexture->GetSizeX(),
			SelectedPreviewTexture->GetSizeY()));
	}
	return TextureOptions.IsEmpty()
		? LOCTEXT(
			"LeafUVNoReferencedTextures",
			"No referenced Texture2D")
		: LOCTEXT(
			"LeafUVSelectReferencedTexture",
			"Choose a referenced Texture2D");
}

FText SFoliageBakerLeafUVPreview::GetSelectedIslandText() const
{
	if (!UVIslands.IsValidIndex(SelectedIslandIndex))
	{
		return LOCTEXT(
			"LeafUVNoIslandSelected",
			"Select islands from the list, click an outline, or drag a selection box.");
	}
	const TOptional<FFoliageBakerLeafUVAnnotation> Annotation =
		FindSelectedAnnotation();
	const FString PivotText = Annotation.IsSet() && Annotation.GetValue().bHasPivot
		? FString::Printf(
			TEXT("(%.6f, %.6f)"),
			Annotation.GetValue().PivotUV.X,
			Annotation.GetValue().PivotUV.Y)
		: TEXT("unset");
	const FString TipText = Annotation.IsSet() && Annotation.GetValue().bHasTip
		? FString::Printf(
			TEXT("(%.6f, %.6f)"),
			Annotation.GetValue().TipUV.X,
			Annotation.GetValue().TipUV.Y)
		: TEXT("unset");
	return FText::FromString(FString::Printf(
		TEXT("%d selected  |  Active Island %d  |  PivotUV %s  |  TipUV %s"),
		SelectedIslandIndices.Num(),
		SelectedIslandIndex,
		*PivotText,
		*TipText));
}

FText SFoliageBakerLeafUVPreview::GetEditModeText() const
{
	switch (EditMode)
	{
	case EFoliageBakerLeafUVEditMode::SetPivot:
		return LOCTEXT("LeafUVPivotModeStatus", "Mode: set PivotUV on every selected island under the click.");
	case EFoliageBakerLeafUVEditMode::SetTip:
		return LOCTEXT("LeafUVTipModeStatus", "Mode: set TipUV on every selected island under the click.");
	default:
		return LOCTEXT("LeafUVSelectModeStatus", "Mode: click or drag-box islands; Ctrl/Shift adds to selection.");
	}
}

FReply SFoliageBakerLeafUVPreview::SetEditMode(
	const EFoliageBakerLeafUVEditMode NewMode)
{
	EditMode = NewMode;
	InvalidatePreview();
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::ClearSelectedAnnotation()
{
	int32 ClearedCount = 0;
	for (const int32 IslandIndex : SelectedIslandIndices)
	{
		if (UVIslands.IsValidIndex(IslandIndex))
		{
			ClearedCount += AnnotationsByKey.Remove(
				MakeAnnotationKey(UVIslands[IslandIndex]));
		}
	}
	if (IslandList.IsValid())
	{
		IslandList->RequestListRefresh();
	}
	InvalidatePreview();
	MarkLeafOwnershipDirty();
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVPointsCleared",
			"Cleared PivotUV and TipUV from {0} selected island(s)."),
		FText::AsNumber(ClearedCount));
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::ResolveLeafOwnership()
{
	if (!CanResolveLeafOwnership())
	{
		return FReply::Handled();
	}

	TArray<FFoliageBakerLeafTemplateAnnotation> CompleteAnnotations;
	for (const FFoliageBakerLeafUVIsland& Island : UVIslands)
	{
		const FString AnnotationKey = MakeAnnotationKey(Island);
		if (!AnnotationsByKey.Contains(AnnotationKey))
		{
			continue;
		}
		const FFoliageBakerLeafUVAnnotation& Annotation =
			AnnotationsByKey.FindChecked(AnnotationKey);
		if (!Annotation.bHasPivot || !Annotation.bHasTip)
		{
			continue;
		}
		FFoliageBakerLeafTemplateAnnotation& CompleteAnnotation =
			CompleteAnnotations.AddDefaulted_GetRef();
		CompleteAnnotation.UVIslandSignature = Island.Signature;
		CompleteAnnotation.TriangleSignatures = Island.TriangleSignatures;
		CompleteAnnotation.PivotUV = Annotation.PivotUV;
		CompleteAnnotation.TipUV = Annotation.TipUV;
	}

	const UStaticMesh& StaticMesh = *SourceStaticMesh.Get();
	bLeafOwnershipDirty = true;
	FFoliageBakerLeafOwnershipResolveResult ResolveResult =
		FFoliageBakerTreeHierarchyColorBaker::ResolveLeafOwnership(
			StaticMesh,
			SourceLODIndex,
			SelectedMaterialIndex,
			*PreviewData,
			CompleteAnnotations);
	if (ResolveResult.bSucceeded)
	{
		ResolvedLeafClusters = MoveTemp(ResolveResult.ResolvedLeafClusters);
		bLeafOwnershipDirty = false;
	}
	StatusText = FText::FromString(ResolveResult.Report);
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::HideSelectedIslands()
{
	const int32 HiddenCount = SelectedIslandIndices.Num();
	for (const int32 IslandIndex : SelectedIslandIndices)
	{
		if (UVIslands.IsValidIndex(IslandIndex))
		{
			HiddenIslandKeys.Add(MakeAnnotationKey(UVIslands[IslandIndex]));
		}
	}
	SelectedIslandIndices.Reset();
	SelectedIslandIndex = INDEX_NONE;
	RefreshVisibleIslandOptions();
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVSelectedHidden",
			"Hidden {0} selected island(s) from the 2D UV preview."),
		FText::AsNumber(HiddenCount));
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::HideCompletedIslands()
{
	int32 HiddenCount = 0;
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		const TOptional<FFoliageBakerLeafUVAnnotation> Annotation =
			FindAnnotation(IslandIndex);
		if (!IsIslandHidden(IslandIndex)
			&& Annotation.IsSet()
			&& Annotation.GetValue().bHasPivot
			&& Annotation.GetValue().bHasTip)
		{
			HiddenIslandKeys.Add(MakeAnnotationKey(UVIslands[IslandIndex]));
			SelectedIslandIndices.Remove(IslandIndex);
			++HiddenCount;
		}
	}
	if (!SelectedIslandIndices.Contains(SelectedIslandIndex))
	{
		SelectedIslandIndex = INDEX_NONE;
		for (const int32 IslandIndex : SelectedIslandIndices)
		{
			if (SelectedIslandIndex == INDEX_NONE
				|| IslandIndex < SelectedIslandIndex)
			{
				SelectedIslandIndex = IslandIndex;
			}
		}
	}
	RefreshVisibleIslandOptions();
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVCompletedHidden",
			"Hidden {0} completed island(s) with both PivotUV and TipUV."),
		FText::AsNumber(HiddenCount));
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::ShowAllIslands()
{
	int32 ShownCount = 0;
	for (const FFoliageBakerLeafUVIsland& Island : UVIslands)
	{
		ShownCount += HiddenIslandKeys.Remove(MakeAnnotationKey(Island));
	}
	RefreshVisibleIslandOptions();
	InvalidatePreview();
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVAllShown",
			"Restored {0} hidden island(s) in the 2D UV preview."),
		FText::AsNumber(ShownCount));
	return FReply::Handled();
}

FReply SFoliageBakerLeafUVPreview::ResetUVView()
{
	UVViewPan = FVector2f::ZeroVector;
	UVViewZoom = 1.0f;
	InvalidatePreview();
	return FReply::Handled();
}

void SFoliageBakerLeafUVPreview::HandleCanvasClick(
	const FVector2f& LocalPosition,
	const FVector2f& LocalSize,
	const bool bAddToSelection)
{
	if (UVIslands.IsEmpty())
	{
		return;
	}
	const FVector2f UV = LocalToUV(LocalPosition, LocalSize);
	if (EditMode == EFoliageBakerLeafUVEditMode::SelectIsland)
	{
		SelectIsland(FindIslandAtUV(UV), bAddToSelection);
		return;
	}
	if (SelectedIslandIndices.IsEmpty())
	{
		StatusText = LOCTEXT(
			"LeafUVNoSelectionForPoint",
			"Select one or more UV0 islands before setting PivotUV or TipUV.");
		InvalidatePreview();
		return;
	}
	int32 UpdatedCount = 0;
	int32 FirstUpdatedIslandIndex = INDEX_NONE;
	for (const int32 IslandIndex : SelectedIslandIndices)
	{
		if (!IsUVInsideIsland(IslandIndex, UV))
		{
			continue;
		}
		FFoliageBakerLeafUVAnnotation& Annotation =
			FindOrAddAnnotation(IslandIndex);
		if (EditMode == EFoliageBakerLeafUVEditMode::SetPivot)
		{
			Annotation.PivotUV = UV;
			Annotation.bHasPivot = true;
		}
		else
		{
			Annotation.TipUV = UV;
			Annotation.bHasTip = true;
		}
		if (FirstUpdatedIslandIndex == INDEX_NONE
			|| IslandIndex < FirstUpdatedIslandIndex)
		{
			FirstUpdatedIslandIndex = IslandIndex;
		}
		++UpdatedCount;
	}
	if (UpdatedCount > 0
		&& !IsUVInsideIsland(SelectedIslandIndex, UV))
	{
		SelectedIslandIndex = FirstUpdatedIslandIndex;
	}
	if (IslandList.IsValid())
	{
		IslandList->RequestListRefresh();
	}
	InvalidatePreview();
	MarkLeafOwnershipDirty();
	if (UpdatedCount == 0)
	{
		StatusText = FText::Format(
			LOCTEXT(
				"LeafUVPointNotStored",
				"No selected island covers UV0 ({0}, {1}); no PivotUV or TipUV value was changed."),
			FText::AsNumber(UV.X),
			FText::AsNumber(UV.Y));
		return;
	}
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVPointStored",
			"Stored {0} at ({1}, {2}) for {3} of {4} selected island(s); islands not covering the click were unchanged."),
		EditMode == EFoliageBakerLeafUVEditMode::SetPivot
			? LOCTEXT("LeafUVPivotName", "PivotUV")
			: LOCTEXT("LeafUVTipName", "TipUV"),
		FText::AsNumber(UV.X),
		FText::AsNumber(UV.Y),
		FText::AsNumber(UpdatedCount),
		FText::AsNumber(SelectedIslandIndices.Num()));
}

void SFoliageBakerLeafUVPreview::HandleCanvasBoxSelection(
	const FVector2f& FirstLocalPosition,
	const FVector2f& SecondLocalPosition,
	const FVector2f& LocalSize,
	const bool bAddToSelection)
{
	const FVector2f FirstUV = LocalToUV(FirstLocalPosition, LocalSize);
	const FVector2f SecondUV = LocalToUV(SecondLocalPosition, LocalSize);
	FBox2f SelectionBounds(EForceInit::ForceInit);
	SelectionBounds += FirstUV;
	SelectionBounds += SecondUV;
	TSet<int32> NewSelection = bAddToSelection
		? SelectedIslandIndices
		: TSet<int32>();
	int32 FirstBoxIslandIndex = INDEX_NONE;
	int32 BoxIslandCount = 0;
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		if (!IsIslandHidden(IslandIndex)
			&& DoesIslandIntersectUVRect(IslandIndex, SelectionBounds))
		{
			NewSelection.Add(IslandIndex);
			if (FirstBoxIslandIndex == INDEX_NONE)
			{
				FirstBoxIslandIndex = IslandIndex;
			}
			++BoxIslandCount;
		}
	}
	const int32 ActiveIslandIndex =
		NewSelection.Contains(SelectedIslandIndex)
			? SelectedIslandIndex
			: FirstBoxIslandIndex;
	ApplyIslandSelection(NewSelection, ActiveIslandIndex, false);
	StatusText = FText::Format(
		LOCTEXT(
			"LeafUVBoxSelected",
			"Box selected {0} visible island(s); {1} island(s) are selected in total."),
		FText::AsNumber(BoxIslandCount),
		FText::AsNumber(SelectedIslandIndices.Num()));
}

void SFoliageBakerLeafUVPreview::ZoomUVViewAtLocal(
	const FVector2f& LocalPosition,
	const FVector2f& LocalSize,
	const float WheelDelta)
{
	const FVector2f UVBeforeZoom = LocalToUV(LocalPosition, LocalSize);
	const float NewZoom = FMath::Clamp(
		UVViewZoom * FMath::Pow(UVViewZoomStep, WheelDelta),
		MinimumUVViewZoom,
		MaximumUVViewZoom);
	if (NewZoom == UVViewZoom)
	{
		return;
	}
	UVViewZoom = NewZoom;
	const FVector2f UVAfterZoom = LocalToUV(LocalPosition, LocalSize);
	UVViewPan += UVBeforeZoom - UVAfterZoom;
	InvalidatePreview();
}

void SFoliageBakerLeafUVPreview::PanUVViewByLocalDelta(
	const FVector2f& LocalDelta,
	const FVector2f& LocalSize)
{
	const FSlateRect ViewRect = GetUVViewRect(LocalSize);
	const FBox2f VisibleUVBounds = GetVisibleUVBounds();
	const FVector2f VisibleUVSize =
		VisibleUVBounds.Max - VisibleUVBounds.Min;
	UVViewPan -= FVector2f(
		LocalDelta.X
			/ FMath::Max(
				static_cast<float>(ViewRect.Right - ViewRect.Left),
				1.0f)
			* VisibleUVSize.X,
		LocalDelta.Y
			/ FMath::Max(
				static_cast<float>(ViewRect.Bottom - ViewRect.Top),
				1.0f)
			* VisibleUVSize.Y);
	InvalidatePreview();
}

FSlateRect SFoliageBakerLeafUVPreview::GetUVViewRect(
	const FVector2f& LocalSize) const
{
	const float AvailableWidth = FMath::Max(
		LocalSize.X - UVCanvasPadding * 2.0f,
		1.0f);
	const float AvailableHeight = FMath::Max(
		LocalSize.Y - UVCanvasPadding * 2.0f,
		1.0f);
	const FVector2f UVSize = UVViewBounds.bIsValid
		? UVViewBounds.Max - UVViewBounds.Min
		: FVector2f(1.0f, 1.0f);
	const float UVAspect = UVSize.X / FMath::Max(UVSize.Y, UE_SMALL_NUMBER);
	const float AvailableAspect = AvailableWidth / AvailableHeight;
	float ViewWidth = AvailableWidth;
	float ViewHeight = AvailableHeight;
	if (AvailableAspect > UVAspect)
	{
		ViewWidth = ViewHeight * UVAspect;
	}
	else
	{
		ViewHeight = ViewWidth / FMath::Max(UVAspect, UE_SMALL_NUMBER);
	}
	const float Left = (LocalSize.X - ViewWidth) * 0.5f;
	const float Top = (LocalSize.Y - ViewHeight) * 0.5f;
	return FSlateRect(Left, Top, Left + ViewWidth, Top + ViewHeight);
}

FBox2f SFoliageBakerLeafUVPreview::GetVisibleUVBounds() const
{
	const FVector2f BaseMin = UVViewBounds.bIsValid
		? UVViewBounds.Min
		: FVector2f(0.0f, 0.0f);
	const FVector2f BaseMax = UVViewBounds.bIsValid
		? UVViewBounds.Max
		: FVector2f(1.0f, 1.0f);
	const FVector2f BaseCenter = (BaseMin + BaseMax) * 0.5f;
	const FVector2f VisibleExtent =
		(BaseMax - BaseMin) * (0.5f / UVViewZoom);
	const FVector2f VisibleCenter = BaseCenter + UVViewPan;
	return FBox2f(
		VisibleCenter - VisibleExtent,
		VisibleCenter + VisibleExtent);
}

FVector2f SFoliageBakerLeafUVPreview::UVToLocal(
	const FVector2f& UV,
	const FVector2f& LocalSize) const
{
	const FSlateRect ViewRect = GetUVViewRect(LocalSize);
	const FBox2f VisibleUVBounds = GetVisibleUVBounds();
	const FVector2f ViewMin = VisibleUVBounds.Min;
	const FVector2f ViewMax = VisibleUVBounds.Max;
	const FVector2f UVSize = ViewMax - ViewMin;
	const FVector2f Normalized(
		(UV.X - ViewMin.X) / FMath::Max(UVSize.X, UE_SMALL_NUMBER),
		(UV.Y - ViewMin.Y) / FMath::Max(UVSize.Y, UE_SMALL_NUMBER));
	return FVector2f(
		static_cast<float>(ViewRect.Left)
			+ Normalized.X * static_cast<float>(ViewRect.Right - ViewRect.Left),
		static_cast<float>(ViewRect.Top)
			+ Normalized.Y * static_cast<float>(ViewRect.Bottom - ViewRect.Top));
}

FVector2f SFoliageBakerLeafUVPreview::LocalToUV(
	const FVector2f& LocalPosition,
	const FVector2f& LocalSize) const
{
	const FSlateRect ViewRect = GetUVViewRect(LocalSize);
	const FBox2f VisibleUVBounds = GetVisibleUVBounds();
	const FVector2f ViewMin = VisibleUVBounds.Min;
	const FVector2f ViewMax = VisibleUVBounds.Max;
	const FVector2f Normalized(
		(LocalPosition.X - static_cast<float>(ViewRect.Left))
			/ FMath::Max(
				static_cast<float>(ViewRect.Right - ViewRect.Left),
				UE_SMALL_NUMBER),
		(LocalPosition.Y - static_cast<float>(ViewRect.Top))
			/ FMath::Max(
				static_cast<float>(ViewRect.Bottom - ViewRect.Top),
				UE_SMALL_NUMBER));
	return ViewMin + Normalized * (ViewMax - ViewMin);
}

int32 SFoliageBakerLeafUVPreview::FindIslandAtUV(const FVector2f& UV) const
{
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		if (!IsIslandHidden(IslandIndex)
			&& IsUVInsideIsland(IslandIndex, UV))
		{
			return IslandIndex;
		}
	}
	return INDEX_NONE;
}

bool SFoliageBakerLeafUVPreview::IsUVInsideIsland(
	const int32 IslandIndex,
	const FVector2f& UV) const
{
	if (!UVIslands.IsValidIndex(IslandIndex))
	{
		return false;
	}
	const FFoliageBakerLeafUVIsland& Island = UVIslands[IslandIndex];
	if (UV.X < Island.Bounds.Min.X
		|| UV.X > Island.Bounds.Max.X
		|| UV.Y < Island.Bounds.Min.Y
		|| UV.Y > Island.Bounds.Max.Y)
	{
		return false;
	}
	for (const FFoliageBakerLeafUVTriangle& Triangle : Island.Triangles)
	{
		if (IsPointInsideUVTriangle(UV, Triangle))
		{
			return true;
		}
	}
	return false;
}

bool SFoliageBakerLeafUVPreview::DoesIslandIntersectUVRect(
	const int32 IslandIndex,
	const FBox2f& UVRect) const
{
	if (!UVIslands.IsValidIndex(IslandIndex)
		|| !UVIslands[IslandIndex].Bounds.Intersect(UVRect))
	{
		return false;
	}
	for (const FFoliageBakerLeafUVTriangle& Triangle :
		UVIslands[IslandIndex].Triangles)
	{
		if (DoesUVTriangleIntersectRect(Triangle, UVRect))
		{
			return true;
		}
	}
	return false;
}

bool SFoliageBakerLeafUVPreview::IsIslandHidden(
	const int32 IslandIndex) const
{
	return UVIslands.IsValidIndex(IslandIndex)
		&& HiddenIslandKeys.Contains(
			MakeAnnotationKey(UVIslands[IslandIndex]));
}

TOptional<FFoliageBakerLeafUVAnnotation>
SFoliageBakerLeafUVPreview::FindAnnotation(const int32 IslandIndex) const
{
	if (!UVIslands.IsValidIndex(IslandIndex))
	{
		return {};
	}
	const FString AnnotationKey = MakeAnnotationKey(UVIslands[IslandIndex]);
	return AnnotationsByKey.Contains(AnnotationKey)
		? TOptional<FFoliageBakerLeafUVAnnotation>(
			AnnotationsByKey.FindRef(AnnotationKey))
		: TOptional<FFoliageBakerLeafUVAnnotation>();
}

TOptional<FFoliageBakerLeafUVAnnotation>
SFoliageBakerLeafUVPreview::FindSelectedAnnotation() const
{
	return FindAnnotation(SelectedIslandIndex);
}

FFoliageBakerLeafUVAnnotation&
SFoliageBakerLeafUVPreview::FindOrAddAnnotation(const int32 IslandIndex)
{
	return AnnotationsByKey.FindOrAdd(
		MakeAnnotationKey(UVIslands[IslandIndex]));
}

FString SFoliageBakerLeafUVPreview::MakeAnnotationKey(
	const FFoliageBakerLeafUVIsland& Island) const
{
	const FString SourceIdentity = SourceStaticMesh.IsValid()
		? SourceStaticMesh->GetPathName()
		: FString();
	return FString::Printf(
		TEXT("%s|LOD%d|%s|%s"),
		*SourceIdentity,
		SourceLODIndex,
		*SelectedMaterialIdentity,
		*Island.Signature);
}

void SFoliageBakerLeafUVPreview::RefreshVisibleIslandOptions()
{
	IslandOptions.Reset();
	for (int32 IslandIndex = 0; IslandIndex < UVIslands.Num(); ++IslandIndex)
	{
		if (!IsIslandHidden(IslandIndex))
		{
			IslandOptions.Add(MakeShared<int32>(IslandIndex));
		}
		else
		{
			SelectedIslandIndices.Remove(IslandIndex);
		}
	}
	if (!SelectedIslandIndices.Contains(SelectedIslandIndex))
	{
		SelectedIslandIndex = INDEX_NONE;
		for (const int32 IslandIndex : SelectedIslandIndices)
		{
			if (SelectedIslandIndex == INDEX_NONE
				|| IslandIndex < SelectedIslandIndex)
			{
				SelectedIslandIndex = IslandIndex;
			}
		}
	}
	if (IslandList.IsValid())
	{
		TGuardValue<bool> SelectionGuard(bUpdatingIslandListSelection, true);
		IslandList->ClearSelection();
		IslandList->RequestListRefresh();
		for (const TSharedPtr<int32>& Option : IslandOptions)
		{
			if (Option.IsValid()
				&& SelectedIslandIndices.Contains(*Option))
			{
				IslandList->SetItemSelection(
					Option,
					true,
					ESelectInfo::Direct);
			}
		}
	}
	InvalidatePreview();
}

void SFoliageBakerLeafUVPreview::ApplyIslandSelection(
	const TSet<int32>& NewSelection,
	const int32 ActiveIslandIndex,
	const bool bScrollActiveIntoView)
{
	SelectedIslandIndices.Reset();
	for (const int32 IslandIndex : NewSelection)
	{
		if (UVIslands.IsValidIndex(IslandIndex)
			&& !IsIslandHidden(IslandIndex))
		{
			SelectedIslandIndices.Add(IslandIndex);
		}
	}
	SelectedIslandIndex = SelectedIslandIndices.Contains(ActiveIslandIndex)
		? ActiveIslandIndex
		: INDEX_NONE;
	if (SelectedIslandIndex == INDEX_NONE)
	{
		for (const int32 IslandIndex : SelectedIslandIndices)
		{
			if (SelectedIslandIndex == INDEX_NONE
				|| IslandIndex < SelectedIslandIndex)
			{
				SelectedIslandIndex = IslandIndex;
			}
		}
	}

	TSharedPtr<int32> ActiveOption;
	if (IslandList.IsValid())
	{
		TGuardValue<bool> SelectionGuard(bUpdatingIslandListSelection, true);
		IslandList->ClearSelection();
		for (const TSharedPtr<int32>& Option : IslandOptions)
		{
			if (Option.IsValid()
				&& SelectedIslandIndices.Contains(*Option))
			{
				IslandList->SetItemSelection(
					Option,
					true,
					ESelectInfo::Direct);
				if (*Option == SelectedIslandIndex)
				{
					ActiveOption = Option;
				}
			}
		}
		if (bScrollActiveIntoView && ActiveOption.IsValid())
		{
			IslandList->RequestScrollIntoView(ActiveOption);
		}
	}
	InvalidatePreview();
	BroadcastSelectedResolvedLeaves();
}

void SFoliageBakerLeafUVPreview::SelectIsland(
	const int32 IslandIndex,
	const bool bAddToSelection)
{
	const bool bValidIsland = UVIslands.IsValidIndex(IslandIndex)
		&& !IsIslandHidden(IslandIndex);
	TSet<int32> NewSelection = bAddToSelection
		? SelectedIslandIndices
		: TSet<int32>();
	int32 ActiveIslandIndex = INDEX_NONE;
	if (bValidIsland)
	{
		if (bAddToSelection && NewSelection.Contains(IslandIndex))
		{
			NewSelection.Remove(IslandIndex);
			ActiveIslandIndex = SelectedIslandIndex == IslandIndex
				? INDEX_NONE
				: SelectedIslandIndex;
		}
		else
		{
			NewSelection.Add(IslandIndex);
			ActiveIslandIndex = IslandIndex;
		}
	}
	else if (bAddToSelection)
	{
		ActiveIslandIndex = SelectedIslandIndex;
	}
	ApplyIslandSelection(NewSelection, ActiveIslandIndex, true);
}

void SFoliageBakerLeafUVPreview::InvalidatePreview()
{
	Invalidate(EInvalidateWidgetReason::Paint);
	if (UVCanvas.IsValid())
	{
		UVCanvas->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void SFoliageBakerLeafUVPreview::MarkLeafOwnershipDirty()
{
	bLeafOwnershipDirty = true;
	InvalidatePreview();
}

void SFoliageBakerLeafUVPreview::BroadcastSelectedResolvedLeaves()
{
	TArray<FFoliageBakerResolvedLeafCluster> SelectedResolvedLeaves;
	if (!SelectedIslandIndices.IsEmpty())
	{
		TSet<FString> SelectedIslandSignatures;
		for (const int32 IslandIndex : SelectedIslandIndices)
		{
			if (UVIslands.IsValidIndex(IslandIndex))
			{
				SelectedIslandSignatures.Add(UVIslands[IslandIndex].Signature);
			}
		}
		for (const FFoliageBakerResolvedLeafCluster& ResolvedLeaf :
			ResolvedLeafClusters)
		{
			if (SelectedIslandSignatures.Contains(
					ResolvedLeaf.UVIslandSignature))
			{
				SelectedResolvedLeaves.Add(ResolvedLeaf);
			}
		}
	}
	OnResolvedLeavesChanged.ExecuteIfBound(SelectedResolvedLeaves);
}

#undef LOCTEXT_NAMESPACE
