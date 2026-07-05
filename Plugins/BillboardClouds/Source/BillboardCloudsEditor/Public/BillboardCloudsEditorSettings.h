#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BillboardCloudsEditorSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "Billboard Clouds"))
class BILLBOARDCLOUDSEDITOR_API UBillboardCloudsEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Relative object-space error. The analyzer multiplies this by the selected mesh bounds sphere radius."))
	double RelativeError = 0.01;

	UPROPERTY(config, EditAnywhere, Category = "Plane Cover", meta = (ClampMin = "0.0", ToolTip = "Minimum object-space error in Unreal centimeters."))
	double MinimumErrorCm = 1.0;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "4", ToolTip = "Number of azimuth samples used in plane-space normal discretization."))
	int32 NormalThetaSteps = 16;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "3", ToolTip = "Number of polar samples used in plane-space normal discretization."))
	int32 NormalPhiSteps = 9;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "8", ToolTip = "Number of rho bins used for each sampled normal direction."))
	int32 RhoBinCount = 256;

	UPROPERTY(config, EditAnywhere, Category = "Plane Space", meta = (ClampMin = "0", ClampMax = "16", ToolTip = "Safety cap for the paper's recursive adaptive refinement after picking the densest plane-space bin. Refinement still stops early when the sub-bin center plane is valid for the whole sub-bin validity set."))
	int32 AdaptiveRefinementDepth = 10;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "16", ClampMax = "1024", ToolTip = "Maximum interior pixel resolution used for the largest object-space billboard extent. Smaller billboards receive proportionally smaller atlas tiles."))
	int32 TextureTileResolution = 128;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "0", ClampMax = "128", ToolTip = "Transparent gutter around each billboard tile, in pixels."))
	int32 TextureTilePaddingPixels = 2;

	UPROPERTY(config, EditAnywhere, Category = "Texture", meta = (ClampMin = "256", ClampMax = "8192", ToolTip = "Maximum generated atlas width or height. Tile resolution is reduced if the selected plane count would exceed this limit."))
	int32 TextureAtlasMaxResolution = 4096;
};
