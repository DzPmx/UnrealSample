#include "FoliageBakerToolChrome.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateColor.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace FoliageBakerToolChrome
{
	namespace
	{
		bool IsUsableBrush(const FSlateBrush* Brush, const ISlateStyle& Style)
		{
			return Brush != nullptr
				&& Brush != Style.GetDefaultBrush()
				&& Brush->DrawAs != ESlateBrushDrawType::NoDrawType;
		}

		const FSlateBrush* GetAppBrushOrFallback(
			const FName PreferredName,
			const FName FallbackName)
		{
			const ISlateStyle& Style = FAppStyle::Get();
			const FSlateBrush* Preferred = Style.GetOptionalBrush(PreferredName);
			if (IsUsableBrush(Preferred, Style))
			{
				return Preferred;
			}
			return Style.GetBrush(FallbackName);
		}

		const FSlateBrush* GetOptionalIcon(const FName IconName)
		{
			if (IconName.IsNone())
			{
				return nullptr;
			}
			const ISlateStyle& Style = FAppStyle::Get();
			const FSlateBrush* Brush = Style.GetOptionalBrush(IconName);
			return IsUsableBrush(Brush, Style) ? Brush : nullptr;
		}

		const FSlateBrush* CardBrush()
		{
			return GetAppBrushOrFallback(
				FName(TEXT("Brushes.Panel")),
				FName(TEXT("ToolPanel.GroupBorder")));
		}

		const FSlateBrush* StepBadgeBrush(const bool bPrimary)
		{
			static const FSlateRoundedBoxBrush PrimaryBrush(FStyleColors::Primary, 8.0f);
			static const FSlateRoundedBoxBrush NeutralBrush(FStyleColors::Hover, 8.0f);
			return bPrimary ? &PrimaryBrush : &NeutralBrush;
		}

		const FSlateBrush* CountBadgeBrush()
		{
			static const FSlateRoundedBoxBrush Brush(FStyleColors::Recessed, 8.0f);
			return &Brush;
		}
	}

	const FSlateBrush* RecessedBrush()
	{
		return GetAppBrushOrFallback(
			FName(TEXT("Brushes.Recessed")),
			FName(TEXT("ToolPanel.GroupBorder")));
	}

	const FSlateBrush* HeaderBrush()
	{
		return GetAppBrushOrFallback(
			FName(TEXT("Brushes.Header")),
			FName(TEXT("ToolPanel.GroupBorder")));
	}

	TSharedRef<SWidget> MakeCard(TSharedRef<SWidget> Content, const FMargin Padding)
	{
		return SNew(SBorder)
			.BorderImage(CardBrush())
			.Padding(Padding)
			[
				Content
			];
	}

	TSharedRef<SWidget> MakeRecessedPanel(TSharedRef<SWidget> Content, const FMargin Padding)
	{
		return SNew(SBorder)
			.BorderImage(RecessedBrush())
			.Padding(Padding)
			[
				Content
			];
	}

	TSharedRef<SWidget> MakeTitle(const TAttribute<FText>& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"));
	}

	TSharedRef<SWidget> MakeMuted(const TAttribute<FText>& Text, const bool bAutoWrap)
	{
		return SNew(STextBlock)
			.Text(Text)
			.AutoWrapText(bAutoWrap)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.TextStyle(FAppStyle::Get(), "SmallText");
	}

	TSharedRef<SWidget> MakeSectionHeader(
		const TAttribute<FText>& Title,
		const TAttribute<FText>& Subtitle,
		TSharedPtr<SWidget> Trailing)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				MakeTitle(Title)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(10.0f, 0.0f, 0.0f, 0.0f)
			.VAlign(VAlign_Center)
			[
				MakeMuted(Subtitle)
			];
		if (Trailing.IsValid())
		{
			Row->AddSlot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					Trailing.ToSharedRef()
				];
		}
		return SNew(SBorder)
			.BorderImage(HeaderBrush())
			.Padding(FMargin(12.0f, 8.0f))
			[
				Row
			];
	}

	TSharedRef<SWidget> MakeCountBadge(
		const TAttribute<FText>& Text,
		const TAttribute<bool>& bActive)
	{
		return SNew(SBorder)
			.BorderImage(CountBadgeBrush())
			.Padding(FMargin(8.0f, 3.0f))
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8))
				.ColorAndOpacity_Lambda([bActive]()
				{
					return bActive.Get(false)
						? FSlateColor(FStyleColors::AccentBlue)
						: FSlateColor::UseSubduedForeground();
				})
			];
	}

	TSharedRef<SWidget> MakeIconTextButton(
		const FName IconName,
		const TAttribute<FText>& Label,
		const TAttribute<FText>& Tooltip,
		FOnClicked OnClicked,
		const TAttribute<bool>& IsEnabled,
		const bool bPrimary)
	{
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		if (const FSlateBrush* Icon = GetOptionalIcon(IconName))
		{
			Content->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(14.0f)
					.HeightOverride(14.0f)
					[
						SNew(SImage)
						.Image(Icon)
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				];
		}
		Content->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
			];

		TSharedRef<SButton> Button = SNew(SButton)
			.ButtonStyle(
				FAppStyle::Get(),
				bPrimary ? TEXT("PrimaryButton") : TEXT("Button"))
			.ContentPadding(FMargin(10.0f, 4.0f))
			.ToolTipText(Tooltip)
			.IsEnabled(IsEnabled)
			.OnClicked(MoveTemp(OnClicked))
			[
				Content
			];
		return Button;
	}

	TSharedRef<SWidget> MakeStepButton(
		const int32 StepNumber,
		const FText& Label,
		const FText& Tooltip,
		FOnClicked OnClicked,
		const TAttribute<bool>& IsEnabled,
		const bool bPrimary)
	{
		return SNew(SButton)
			.ButtonStyle(
				FAppStyle::Get(),
				bPrimary ? TEXT("PrimaryButton") : TEXT("Button"))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.ContentPadding(FMargin(12.0f, 7.0f))
			.ToolTipText(Tooltip)
			.IsEnabled(IsEnabled)
			.OnClicked(MoveTemp(OnClicked))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(StepBadgeBrush(bPrimary))
					.Padding(FMargin(7.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text(FText::AsNumber(StepNumber))
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 9))
						.ColorAndOpacity(FLinearColor::White)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
			];
	}
}
