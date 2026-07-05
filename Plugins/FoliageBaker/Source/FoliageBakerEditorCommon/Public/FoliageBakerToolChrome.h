#pragma once

#include "CoreMinimal.h"
#include "Framework/SlateDelegates.h"
#include "Layout/Margin.h"
#include "Misc/Attribute.h"
#include "Widgets/SWidget.h"

struct FSlateBrush;

namespace FoliageBakerToolChrome
{
	FOLIAGEBAKEREDITORCOMMON_API const FSlateBrush* RecessedBrush();
	FOLIAGEBAKEREDITORCOMMON_API const FSlateBrush* HeaderBrush();

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeCard(
		TSharedRef<SWidget> Content,
		FMargin Padding = FMargin(14.0f, 12.0f));

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeRecessedPanel(
		TSharedRef<SWidget> Content,
		FMargin Padding = FMargin(0.0f));

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeTitle(
		const TAttribute<FText>& Text);

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeMuted(
		const TAttribute<FText>& Text,
		bool bAutoWrap = true);

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeSectionHeader(
		const TAttribute<FText>& Title,
		const TAttribute<FText>& Subtitle,
		TSharedPtr<SWidget> Trailing = nullptr);

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeCountBadge(
		const TAttribute<FText>& Text,
		const TAttribute<bool>& bActive);

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeIconTextButton(
		FName IconName,
		const TAttribute<FText>& Label,
		const TAttribute<FText>& Tooltip,
		FOnClicked OnClicked,
		const TAttribute<bool>& IsEnabled = true,
		bool bPrimary = false);

	FOLIAGEBAKEREDITORCOMMON_API TSharedRef<SWidget> MakeStepButton(
		int32 StepNumber,
		const FText& Label,
		const FText& Tooltip,
		FOnClicked OnClicked,
		const TAttribute<bool>& IsEnabled,
		bool bPrimary = false);
}
