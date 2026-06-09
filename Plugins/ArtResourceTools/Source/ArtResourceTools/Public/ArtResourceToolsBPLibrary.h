#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Texture2D.h"
#include "ArtResourceToolsBPLibrary.generated.h"


UCLASS()
class UArtResourceToolsBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()
#if WITH_EDITOR

public:
	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|SDFAOBaker",
		meta = (DisplayName = "Bake SDF AO To Vertex Color Alpha Channel",
			AutoCreateRefTerm = "MaterialIDNameBlacklist",
			AdvancedDisplay = "MaterialIDNameBlacklist,MinTriangleCount"))
	static void BakeSDFAOToVertexColorAlpha(
		UStaticMesh* StaticMesh,
		const TArray<FName>& MaterialIDNameBlacklist,
		float WrapOffset = 5.0f,
		int32 SmoothIterations = 25,
		int32 IcoSubdivisions = 2,
		int32 VoxelCount = 32,
		float AOPower = 2.0f,
		bool bInvertAO = true,
		int32 MinTriangleCount = 36);

	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|NormalTransfer",
		meta = (DisplayName = "Transfer Wrap Mesh Normals To Mesh",
			AutoCreateRefTerm = "MaterialIDNameBlacklist",
			AdvancedDisplay = "MaterialIDNameBlacklist,MinTriangleCount"))
	static void TransferWrapMeshNormals(
		UStaticMesh* StaticMesh,
		const TArray<FName>& MaterialIDNameBlacklist,
		float WrapOffset = 5.0f,
		int32 SmoothIterations = 25,
		int32 VoxelCount = 32,
		int32 MinTriangleCount = 36);

	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|Texture",
		meta = (DisplayName = "Flip Selected Textures V"))
	static int32 FlipSelectedTexturesV();

	UFUNCTION(BlueprintCallable, Category = "ArtResourceProcessing|Texture",
		meta = (DisplayName = "Flip Textures V"))
	static int32 FlipTexturesV(const TArray<UTexture2D*>& Textures);

#endif
};
