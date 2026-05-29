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

	// Builds a smoothed "wrap" envelope around the mesh (Mesh->Volume->Mesh +
	// Smooth, like the Blender Geometry Nodes approach) and transfers its soft
	// surface normals onto the original mesh as custom normals. Great for
	// foliage / vegetation shading.
	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|NormalTransfer",
		meta = (DisplayName = "Transfer Wrap Mesh Normals To Mesh"))
	static void TransferWrapMeshNormals(
		UStaticMesh* StaticMesh,
		float WrapOffset = 5.0f,
		int32 SmoothIterations = 25,
		int32 VoxelCount = 32);
#endif
};
