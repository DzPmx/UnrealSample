#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/EngineTypes.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"

#include <atomic>

class UObject;

namespace SHThicknessBaker
{

inline constexpr double NormalizedRayNormalOffset = 1.1e-5;
inline constexpr double NormalizedRayNearClip = 1.0e-5;

enum class EBakeMode : uint8
{
	CPU,
	GPU
};

enum class ECoefficientSpace : uint8
{
	Tangent,
	Local
};

enum class ETextureResolution : int32
{
	Size256 = 256,
	Size512 = 512,
	Size1024 = 1024,
	Size2048 = 2048
};

constexpr int32 GetTextureResolution(
	const ETextureResolution Resolution)
{
	return static_cast<int32>(Resolution);
}

struct FBakeSettings
{
	EBakeMode BakeMode = EBakeMode::GPU;
	ECoefficientSpace CoefficientSpace = ECoefficientSpace::Tangent;
	int32 BakeUVChannel = 1;
	bool bRegenerateBakeUV = false;
	bool bRemapCoefficientRange = true;
	ETextureResolution TextureResolution =
		ETextureResolution::Size1024;
	int32 DirectionCount = 256;
	int32 SamplesPerPixel = 16;
	int32 PaddingSize = 16;
};

enum class EJobStage : uint8
{
	Ready,
	Baking,
	CollectingSamples,
	GPUComputing,
	Filtering,
	Encoding,
	Succeeded,
	Cancelled,
	Failed
};

struct FBakeTargetPreparation
{
	TWeakObjectPtr<UObject> SourceMesh;
	FMeshDescription MeshDescription;
	TUniquePtr<UE::Geometry::FDynamicMesh3> DynamicMesh;
	TArray<int32> CombinedTriangleIDs;
	bool bSourceIsSkeletalMesh = false;
	bool bDisableLightmapGeneration = false;
	bool bUseLegacyTangentScaling = false;
	FSHAHash SourceMeshDescriptionHash;
	FSHAHash SourceMeshTopologyHash;
	FMeshBuildSettings SourceBuildSettings;
	FSkeletalMeshBuildSettings SourceSkeletalBuildSettings;
	TArray<FText> Warnings;
};

struct FBakePreparation
{
	TArray<FBakeTargetPreparation> Targets;
	TUniquePtr<UE::Geometry::FDynamicMesh3> CombinedDynamicMesh;
	FBakeSettings Settings;
	double ThicknessScale = 0.0;
	TArray<FText> Warnings;
};

class FBakeJob : public TSharedFromThis<FBakeJob, ESPMode::ThreadSafe>
{
public:
	explicit FBakeJob(FBakePreparation&& InPreparation);

	void Run();
	void RunCPU();
	void RequestCancel();

	FString GetStatusText() const;

	FBakePreparation Preparation;
	std::atomic<EJobStage> Stage{ EJobStage::Ready };
	std::atomic<bool> bCancelRequested{ false };
	std::atomic<int64> ProcessedSurfaceSamples{ 0 };
	std::atomic<int64> TotalSurfaceSamples{ 0 };
	std::atomic<int64> InvalidRayCount{ 0 };

	TArray<TArray64<uint8>> EncodedRGBA;
	FString Error;
};

bool PrepareBake(
	TConstArrayView<UObject*> SourceMeshes,
	const FBakeSettings& Settings,
	FBakePreparation& OutPreparation,
	FText& OutError);

bool ApplyPreparedBakeUV(
	const FMeshDescription& PreparedMeshDescription,
	FMeshDescription& InOutTargetMeshDescription,
	int32 BakeUVChannel,
	FText& OutError);

bool EncodeCoefficientImagesRGBA8(
	const TArray<TArray64<FVector4f>>& CoefficientImages,
	bool bRemapCoefficientRange,
	TArray<TArray64<uint8>>& OutRGBAImages);

FSHAHash ComputeMeshDescriptionTopologyHash(
	const FMeshDescription& MeshDescription);

} // namespace SHThicknessBaker
