#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "ArtResourceToolsBPLibrary.generated.h"


UCLASS()
class UArtResourceToolsBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

#if WITH_EDITOR
	public:
	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|SDFAOBaker",
		meta = (DisplayName = "Bake SDF AO To Vertex Color Alpha Channel"))
	static void BakeSDFAOToVertexColorAlpha(
		UStaticMesh* StaticMesh,
		float WrapOffset = 5.0f,
		int32 SmoothIterations = 25,
		int32 IcoSubdivisions = 2,
		int32 VoxelCount = 32,
		float AOPower = 2.0f,
		bool bInvertAO = true);
#endif
};
