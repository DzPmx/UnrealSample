#if WITH_EDITOR
#include "ArtResourceToolsBPLibrary.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "IContentBrowserSingleton.h"
#include "Materials/MaterialInstance.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "StaticMeshAttributes.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Spatial/MeshAABBTree3.h"
#include "Intersection/IntrRay3Triangle3.h"
#include "Generators/MarchingCubes.h"
#include "Async/ParallelFor.h"
#include "Distance/DistPoint3Triangle3.h"
#include "MeshQueries.h"

DEFINE_LOG_CATEGORY_STATIC(ArResourceProcessor, Log, All);
using namespace UE::Geometry;

UArtResourceToolsBPLibrary::UArtResourceToolsBPLibrary(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

static TArray<FVector3d> BuildIcosphereDirections(int32 Subdivisions)
{
	
	const double T = (1.0 + FMath::Sqrt(5.0)) / 2.0;

	TArray<FVector3d> Verts =
	{
		FVector3d(-1,  T,  0), FVector3d( 1,  T,  0),
		FVector3d(-1, -T,  0), FVector3d( 1, -T,  0),
		FVector3d( 0, -1,  T), FVector3d( 0,  1,  T),
		FVector3d( 0, -1, -T), FVector3d( 0,  1, -T),
		FVector3d( T,  0, -1), FVector3d( T,  0,  1),
		FVector3d(-T,  0, -1), FVector3d(-T,  0,  1),
	};
	for (FVector3d& V : Verts) { V.Normalize(); }

	TArray<FIntVector> Faces =
	{
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
		{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
		{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
	};

	for (int32 S = 0; S < Subdivisions; ++S)
	{
		TMap<uint64, int32> EdgeMidCache;
		TArray<FIntVector> NewFaces;
		NewFaces.Reserve(Faces.Num() * 4);

		auto GetMid = [&](int32 A, int32 B) -> int32
		{
			uint64 Key = (A < B) ? ((uint64)A << 32 | (uint32)B)
			                     : ((uint64)B << 32 | (uint32)A);
			if (int32* Cached = EdgeMidCache.Find(Key))
			{
				return *Cached;
			}
			FVector3d Mid = (Verts[A] + Verts[B]) * 0.5;
			Mid.Normalize();
			int32 Idx = Verts.Add(Mid);
			EdgeMidCache.Add(Key, Idx);
			return Idx;
		};

		for (const FIntVector& F : Faces)
		{
			int32 m01 = GetMid(F.X, F.Y);
			int32 m12 = GetMid(F.Y, F.Z);
			int32 m20 = GetMid(F.Z, F.X);
			NewFaces.Add({F.X, m01, m20});
			NewFaces.Add({F.Y, m12, m01});
			NewFaces.Add({F.Z, m20, m12});
			NewFaces.Add({m01, m12, m20});
		}
		Faces = MoveTemp(NewFaces);
	}

	return Verts; 
}


static void LaplacianSmooth(FDynamicMesh3& Mesh, int32 Iterations)
{
	const int32 MaxVID = Mesh.MaxVertexID();
	TArray<FVector3d> NewPos;
	NewPos.SetNumUninitialized(MaxVID);

	for (int32 Iter = 0; Iter < Iterations; ++Iter)
	{
		for (int32 VID : Mesh.VertexIndicesItr())
		{
			FVector3d Avg = FVector3d::Zero();
			int32     Count = 0;
			for (int32 NID : Mesh.VtxVerticesItr(VID))
			{
				Avg += Mesh.GetVertex(NID);
				++Count;
			}
			if (Count == 0)
			{
				NewPos[VID] = Mesh.GetVertex(VID);
				continue;
			}
			NewPos[VID] = Avg / (double)Count;
		}
		for (int32 VID : Mesh.VertexIndicesItr())
		{
			Mesh.SetVertex(VID, NewPos[VID]);
		}
	}
}

static bool FlipTextureSourceMipV(UTexture2D* Texture, int32 BlockIndex, int32 LayerIndex, int32 MipIndex)
{
	FTextureSource& Source = Texture->Source;
	FTextureSourceBlock Block;
	Source.GetBlock(BlockIndex, Block);

	const int32 MipSizeX = FMath::Max(Block.SizeX >> MipIndex, 1);
	const int32 MipSizeY = FMath::Max(Block.SizeY >> MipIndex, 1);
	const int32 MipSlices = Source.GetMippedNumSlices(Block.NumSlices, MipIndex);

	if (MipSizeY <= 1 || MipSlices <= 0)
	{
		return false;
	}

	const int64 BytesPerPixel = Source.GetBytesPerPixel(LayerIndex);
	const int64 RowStride64 = (int64)MipSizeX * BytesPerPixel;
	const int64 SliceStride64 = RowStride64 * MipSizeY;
	if (BytesPerPixel <= 0 || RowStride64 <= 0 || RowStride64 > MAX_int32)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("FlipSelectedTexturesV: unsupported source format on '%s' (Layer=%d, Mip=%d)."),
			*Texture->GetName(), LayerIndex, MipIndex);
		return false;
	}

	uint8* MipData = Source.LockMip(BlockIndex, LayerIndex, MipIndex);
	if (!MipData)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("FlipSelectedTexturesV: could not lock source mip on '%s' (Block=%d, Layer=%d, Mip=%d)."),
			*Texture->GetName(), BlockIndex, LayerIndex, MipIndex);
		return false;
	}

	TArray<uint8> TempRow;
	const int32 RowStride = (int32)RowStride64;
	TempRow.SetNumUninitialized(RowStride);

	for (int32 SliceIndex = 0; SliceIndex < MipSlices; ++SliceIndex)
	{
		uint8* SliceData = MipData + (SliceStride64 * SliceIndex);
		for (int32 Y = 0; Y < MipSizeY / 2; ++Y)
		{
			uint8* TopRow = SliceData + ((int64)Y * RowStride);
			uint8* BottomRow = SliceData + ((int64)(MipSizeY - 1 - Y) * RowStride);
			FMemory::Memcpy(TempRow.GetData(), TopRow, RowStride);
			FMemory::Memcpy(TopRow, BottomRow, RowStride);
			FMemory::Memcpy(BottomRow, TempRow.GetData(), RowStride);
		}
	}

	Source.UnlockMip(BlockIndex, LayerIndex, MipIndex);
	return true;
}

static bool FlipTextureSourceV(UTexture2D* Texture)
{
	if (!IsValid(Texture))
	{
		return false;
	}

	FTextureSource& Source = Texture->Source;
	if (!Source.IsValid())
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("FlipSelectedTexturesV: '%s' has no valid source data."), *Texture->GetName());
		return false;
	}

	Texture->Modify();
	Texture->PreEditChange(nullptr);

	bool bModified = false;
	const int32 NumBlocks = Source.GetNumBlocks();
	const int32 NumLayers = Source.GetNumLayers();
	for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
	{
		FTextureSourceBlock Block;
		Source.GetBlock(BlockIndex, Block);

		for (int32 LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
		{
			for (int32 MipIndex = 0; MipIndex < Block.NumMips; ++MipIndex)
			{
				bModified |= FlipTextureSourceMipV(Texture, BlockIndex, LayerIndex, MipIndex);
			}
		}
	}

	if (bModified)
	{
		Texture->MarkPackageDirty();
	}

	Texture->PostEditChange();

	if (bModified)
	{
		const UTexture::EUpdateResourceFlags RebuildFlags =
			(UTexture::EUpdateResourceFlags)((uint32)UTexture::EUpdateResourceFlags::ForceRebuild
				| (uint32)UTexture::EUpdateResourceFlags::Synchronous);
		Texture->UpdateResourceWithParams(RebuildFlags);
		Texture->BlockOnAnyAsyncBuild();
	}

	return bModified;
}


static FDynamicMesh3 BuildWrapMesh(const FDynamicMesh3& OrigMesh,
                                    float WrapOffset,
                                    int32 SmoothIterations,
                                    int32 VoxelCount)
{
	FDynamicMesh3 SourceCopy(OrigMesh);
	
	FAxisAlignedBox3d Bounds = SourceCopy.GetBounds(true);
	const double Pad = FMath::Max((double)WrapOffset * 2.0, 1.0);
	Bounds.Expand(Pad);

	const int32  SafeVoxels = FMath::Max(VoxelCount, 8);
	const double CellSize   = Bounds.MaxDim() / (double)SafeVoxels;
	
	const double Band = FMath::Max((double)WrapOffset, CellSize * 1.5);
	
	FDynamicMeshAABBTree3 SourceTree(&SourceCopy, /*bAutoBuild=*/true);

	IMeshSpatial::FQueryOptions QOpts;
	QOpts.MaxDistance = Band * 2.0; // early-out for cells far outside the band
	
	FMarchingCubes MC;
	MC.Bounds   = Bounds;
	MC.CubeSize = CellSize;
	MC.IsoValue = 0.0;
	MC.RootMode = ERootfindingModes::LerpSteps;
	MC.RootModeSteps = 3;
	
	MC.Implicit = [&SourceTree, &QOpts, Band](const FVector3d& P) -> double
	{
		double NearDistSq = TNumericLimits<double>::Max();
		const int32 TID = SourceTree.FindNearestTriangle(P, NearDistSq, QOpts);
		if (TID == IndexConstants::InvalidID)
		{
			return Band;
		}
		return FMath::Sqrt(NearDistSq) - Band;
	};

	MC.Generate();
	MC.Implicit = nullptr; 
	
	FDynamicMesh3 Wrap;
	Wrap.EnableTriangleGroups();
	for (const FVector3d& V : MC.Vertices)
	{
		Wrap.AppendVertex(V);
	}
	for (const FIndex3i& T : MC.Triangles)
	{
		Wrap.AppendTriangle(T);
	}

	if (Wrap.TriangleCount() == 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("BuildWrapMesh: marching cubes produced an empty mesh "
			     "(VoxelCount=%d, Band=%.3f, BoundsDiag=%.3f). "
			     "Falling back to the original mesh."),
			VoxelCount, Band, Bounds.DiagonalLength());
		return SourceCopy;
	}

	LaplacianSmooth(Wrap, SmoothIterations);
	return Wrap;
}

static double RaycastAllDirections(const TMeshAABBTree3<FDynamicMesh3>& Tree,
                                    const FDynamicMesh3& WrapMesh,
                                    const FVector3d& Origin,
                                    const TArray<FVector3d>& RayDirs,
                                    double MaxDist,
                                    bool& bOutHit)
{
	bOutHit = false;
	double BestSignDist = TNumericLimits<double>::Max();

	for (const FVector3d& Dir : RayDirs)
	{
		FRay3d Ray(Origin, Dir, false);
		int32 HitTriID = Tree.FindNearestHitTriangle(Ray, MaxDist);
		if (HitTriID == IndexConstants::InvalidID)
		{
			continue;
		}

		// Get hit triangle vertices
		FIndex3i TriIdx = WrapMesh.GetTriangle(HitTriID);
		FVector3d V0 = WrapMesh.GetVertex(TriIdx.A);
		FVector3d V1 = WrapMesh.GetVertex(TriIdx.B);
		FVector3d V2 = WrapMesh.GetVertex(TriIdx.C);

		// Ray-triangle intersection to get exact hit point
		FIntrRay3Triangle3d Intr(Ray, FTriangle3d(V0, V1, V2));
		if (!Intr.Find())
		{
			continue;
		}

		FVector3d HitPos = Ray.PointAt(Intr.RayParameter);
		double Dist      = (HitPos - Origin).Length();
		
		FVector3d FaceNormal = VectorUtil::NormalDirection(V0, V1, V2);
		double    DotVal     = Dir.Dot(FaceNormal);
		double    SignedDist = FMath::Sign(DotVal) * Dist; 
		SignedDist = -SignedDist;

		if (!bOutHit || FMath::Abs(SignedDist) < FMath::Abs(BestSignDist))
		{
			BestSignDist = SignedDist;
			bOutHit      = true;
		}
	}

	return BestSignDist;
}

struct FMeshProcessingFilter
{
	TSet<FPolygonGroupID> ProcessablePolygonGroups;
	int32 ProcessableTriangleCount = 0;
};

static bool FindBlacklistedParentMaterialName(const UMaterialInterface* MaterialInterface,
	const TArray<FName>& MaterialIDNameBlacklist, FName& OutMatchedName)
{
	OutMatchedName = NAME_None;

	const UMaterialInstance* MaterialInstance = Cast<const UMaterialInstance>(MaterialInterface);
	if (!MaterialInstance || MaterialIDNameBlacklist.Num() == 0)
	{
		return false;
	}

	TSet<const UMaterialInterface*> VisitedParents;
	const UMaterialInterface* ParentMaterial = ToRawPtr(MaterialInstance->Parent);
	while (ParentMaterial)
	{
		if (VisitedParents.Contains(ParentMaterial))
		{
			break;
		}
		VisitedParents.Add(ParentMaterial);

		const FName ParentMaterialName = ParentMaterial->GetFName();
		if (MaterialIDNameBlacklist.Contains(ParentMaterialName))
		{
			OutMatchedName = ParentMaterialName;
			return true;
		}

		const UMaterialInstance* ParentMaterialInstance = Cast<const UMaterialInstance>(ParentMaterial);
		ParentMaterial = ParentMaterialInstance ? ToRawPtr(ParentMaterialInstance->Parent) : nullptr;
	}

	return false;
}

static bool IsMaterialIDNameBlacklisted(const UStaticMesh& StaticMesh, FName ImportedMaterialSlotName,
	const TArray<FName>& MaterialIDNameBlacklist, FName& OutMatchedName)
{
	OutMatchedName = NAME_None;

	if (MaterialIDNameBlacklist.Num() == 0)
	{
		return false;
	}

	if (MaterialIDNameBlacklist.Contains(ImportedMaterialSlotName))
	{
		OutMatchedName = ImportedMaterialSlotName;
		return true;
	}

	const int32 MaterialIndex = StaticMesh.GetMaterialIndexFromImportedMaterialSlotName(ImportedMaterialSlotName);
	const TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
	if (!StaticMaterials.IsValidIndex(MaterialIndex))
	{
		return false;
	}

	const FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
	if (MaterialIDNameBlacklist.Contains(StaticMaterial.MaterialSlotName))
	{
		OutMatchedName = StaticMaterial.MaterialSlotName;
		return true;
	}

	if (MaterialIDNameBlacklist.Contains(StaticMaterial.ImportedMaterialSlotName))
	{
		OutMatchedName = StaticMaterial.ImportedMaterialSlotName;
		return true;
	}

	if (FindBlacklistedParentMaterialName(ToRawPtr(StaticMaterial.MaterialInterface),
		MaterialIDNameBlacklist, OutMatchedName))
	{
		return true;
	}

	return false;
}

static FMeshProcessingFilter BuildMeshProcessingFilter(const UStaticMesh& StaticMesh,
	const FMeshDescription& MeshDesc, const TArray<FName>& MaterialIDNameBlacklist,
	int32 MinTriangleCount, int32 LODIndex)
{
	FMeshProcessingFilter Filter;

	const int32 EffectiveMinTriangleCount = FMath::Max(0, MinTriangleCount);
	const bool bHasMaterialSlotNames =
		MeshDesc.PolygonGroupAttributes().HasAttribute(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	const FStaticMeshConstAttributes Attributes(MeshDesc);

	for (const FPolygonGroupID& PolygonGroupID : MeshDesc.PolygonGroups().GetElementIDs())
	{
		const int32 GroupTriangleCount = MeshDesc.GetPolygonGroupTriangles(PolygonGroupID).Num();
		if (GroupTriangleCount <= 0)
		{
			continue;
		}

		FName ImportedMaterialSlotName = NAME_None;
		if (bHasMaterialSlotNames)
		{
			ImportedMaterialSlotName = Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroupID];
		}

		FName MatchedBlacklistName;
		if (IsMaterialIDNameBlacklisted(StaticMesh, ImportedMaterialSlotName, MaterialIDNameBlacklist,
			MatchedBlacklistName))
		{
			UE_LOG(ArResourceProcessor, Log,
				TEXT("  -> Skipping LOD%d material '%s' on '%s': matched material blacklist entry '%s'."),
				LODIndex, *ImportedMaterialSlotName.ToString(), *StaticMesh.GetName(),
				*MatchedBlacklistName.ToString());
			continue;
		}

		if (EffectiveMinTriangleCount > 0 && GroupTriangleCount < EffectiveMinTriangleCount)
		{
			UE_LOG(ArResourceProcessor, Log,
				TEXT("  -> Skipping LOD%d material '%s' on '%s': triangle count %d is below %d."),
				LODIndex, *ImportedMaterialSlotName.ToString(), *StaticMesh.GetName(), GroupTriangleCount,
				EffectiveMinTriangleCount);
			continue;
		}

		Filter.ProcessablePolygonGroups.Add(PolygonGroupID);
		Filter.ProcessableTriangleCount += GroupTriangleCount;
	}

	return Filter;
}

void UArtResourceToolsBPLibrary::BakeSDFAOToVertexColorAlpha(UStaticMesh* StaticMesh,
	const TArray<FName>& MaterialIDNameBlacklist, float WrapOffset, int32 SmoothIterations,
	int32 IcoSubdivisions, int32 VoxelCount, float AOPower, bool bInvertAO, int32 MinTriangleCount)
{
	if (!IsValid(StaticMesh))
	{
		UE_LOG(ArResourceProcessor, Warning, TEXT("BakeSDFAOToVertexColorAlpha: StaticMesh is null."));
		return;
	}

	const int32 NumLODs = StaticMesh->GetNumSourceModels();
	if (NumLODs <= 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("BakeSDFAOToVertexColorAlpha: '%s' has no source models."),
			*StaticMesh->GetName());
		return;
	}

	UE_LOG(ArResourceProcessor, Log,
		TEXT("BakeSDFAOToVertexColorAlpha: Starting bake on '%s' (NumLODs=%d, WrapOffset=%.3f, SmoothIter=%d, IcoSub=%d, VoxelCount=%d, MinTriangles=%d, Blacklist=%d)"),
		*StaticMesh->GetName(), NumLODs, WrapOffset, SmoothIterations, IcoSubdivisions, VoxelCount,
		MinTriangleCount, MaterialIDNameBlacklist.Num());

	const TArray<FVector3d> RayDirs = BuildIcosphereDirections(IcoSubdivisions);
	UE_LOG(ArResourceProcessor, Log, TEXT("  Ray directions count: %d"), RayDirs.Num());

	StaticMesh->Modify();

	int32 SuccessLODCount = 0;

	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMeshDescription* MeshDesc = StaticMesh->GetMeshDescription(LODIndex);
		if (!MeshDesc)
		{
			UE_LOG(ArResourceProcessor, Warning,
				TEXT("  -> Skipping LOD%d on '%s': no MeshDescription."),
				LODIndex, *StaticMesh->GetName());
			continue;
		}

		const FMeshProcessingFilter ProcessingFilter = BuildMeshProcessingFilter(
			*StaticMesh, *MeshDesc, MaterialIDNameBlacklist, MinTriangleCount, LODIndex);
		if (ProcessingFilter.ProcessableTriangleCount <= 0)
		{
			UE_LOG(ArResourceProcessor, Warning,
				TEXT("  -> Skipping LOD%d on '%s': no material group passed the filters."),
				LODIndex, *StaticMesh->GetName());
			continue;
		}

		UE_LOG(ArResourceProcessor, Log,
			TEXT("  -> Baking LOD%d (%d filtered triangles) ..."),
			LODIndex, ProcessingFilter.ProcessableTriangleCount);

		FDynamicMesh3 OrigDynMesh;
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(MeshDesc, OrigDynMesh);
		}
		const int32 MaxVID = OrigDynMesh.MaxVertexID();

		FDynamicMesh3 WrapDynMesh = BuildWrapMesh(OrigDynMesh, WrapOffset, SmoothIterations, VoxelCount);

		TMeshAABBTree3<FDynamicMesh3> AABBTree(&WrapDynMesh, /*bBuild=*/true);
		
		FAxisAlignedBox3d WrapBounds = WrapDynMesh.GetBounds(true);
		const double RayLength = WrapBounds.DiagonalLength();

		TArray<double> SignDists;
		SignDists.SetNumZeroed(MaxVID);
		TArray<bool> VertHit;
		VertHit.Init(false, MaxVID);
		
		ParallelFor(MaxVID, [&](int32 VID)
		{
			if (!OrigDynMesh.IsVertex(VID))
			{
				return;
			}

			const FVector3d VPos = OrigDynMesh.GetVertex(VID);
			bool bHit = false;
			const double SD = RaycastAllDirections(AABBTree, WrapDynMesh, VPos, RayDirs, RayLength, bHit);
			if (bHit)
			{
				SignDists[VID] = SD;
				VertHit[VID]   = true;
			}
		});

		double MinDist =  TNumericLimits<double>::Max();
		double MaxDist = -TNumericLimits<double>::Max();
		for (int32 VID = 0; VID < MaxVID; ++VID)
		{
			if (VertHit[VID])
			{
				MinDist = FMath::Min(MinDist, SignDists[VID]);
				MaxDist = FMath::Max(MaxDist, SignDists[VID]);
			}
		}

		// Prevent divide-by-zero
		const double Range = (MaxDist - MinDist);
		if (FMath::IsNearlyZero(Range))
		{
			UE_LOG(ArResourceProcessor, Warning,
				TEXT("  -> LOD%d SDF range is near zero – skipping."), LODIndex);
			continue;
		}
		
		const double NeutralAO = bInvertAO ? 1.0 : 0.0;
		const double SafePower = FMath::Max((double)AOPower, 0.01);
		for (int32 VID = 0; VID < MaxVID; ++VID)
		{
			if (!VertHit[VID])
			{
				SignDists[VID] = NeutralAO;
				continue;
			}

			double N = (SignDists[VID] - MinDist) / Range;
			N = FMath::Clamp(N, 0.0, 1.0);
			if (bInvertAO)
			{
				N = 1.0 - N;
			}
			SignDists[VID] = FMath::Pow(N, SafePower);
		}

		{
			FStaticMeshAttributes Attributes(*MeshDesc);
			Attributes.Register(/*bKeepExistingAttribute*/ true);

			TVertexInstanceAttributesRef<FVector4f> Colors =
				Attributes.GetVertexInstanceColors();

			if (!Colors.IsValid())
			{
				UE_LOG(ArResourceProcessor, Warning,
					TEXT("  -> LOD%d: could not get vertex instance color attribute – skipping."),
					LODIndex);
				continue;
			}

			int32 UpdatedVertexInstanceCount = 0;
			for (const FTriangleID& TriangleID : MeshDesc->Triangles().GetElementIDs())
			{
				if (!ProcessingFilter.ProcessablePolygonGroups.Contains(MeshDesc->GetTrianglePolygonGroup(TriangleID)))
				{
					continue;
				}

				for (const FVertexInstanceID& VIID : MeshDesc->GetTriangleVertexInstances(TriangleID))
				{
					const FVertexID VertID = MeshDesc->GetVertexInstanceVertex(VIID);
					const int32     VID    = VertID.GetValue();
					if (VID < MaxVID && OrigDynMesh.IsVertex(VID))
					{
						FVector4f C = Colors[VIID];
						C.W = (float)SignDists[VID]; // write to Alpha channel
						Colors[VIID] = C;
						++UpdatedVertexInstanceCount;
					}
				}
			}

			if (UpdatedVertexInstanceCount == 0)
			{
				UE_LOG(ArResourceProcessor, Warning,
					TEXT("  -> LOD%d: no vertex instances were updated after filtering."),
					LODIndex);
				continue;
			}
		}

		StaticMesh->CommitMeshDescription(LODIndex);
		++SuccessLODCount;

		UE_LOG(ArResourceProcessor, Log, TEXT("  <- LOD%d done."), LODIndex);
	}

	if (SuccessLODCount == 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("BakeSDFAOToVertexColorAlpha: no LOD was baked on '%s'."),
			*StaticMesh->GetName());
		return;
	}
	
	StaticMesh->PostEditChange();

	UE_LOG(ArResourceProcessor, Log,
		TEXT("BakeSDFAOToVertexColorAlpha: Done. Wrote AO to vertex color Alpha on '%s' (%d/%d LODs)."),
		*StaticMesh->GetName(), SuccessLODCount, NumLODs);
}

void UArtResourceToolsBPLibrary::TransferWrapMeshNormals(UStaticMesh* StaticMesh,
	const TArray<FName>& MaterialIDNameBlacklist, float WrapOffset, int32 SmoothIterations, int32 VoxelCount,
	int32 MinTriangleCount)
{
	if (!IsValid(StaticMesh))
	{
		UE_LOG(ArResourceProcessor, Warning, TEXT("TransferWrapMeshNormals: StaticMesh is null."));
		return;
	}

	const int32 NumLODs = StaticMesh->GetNumSourceModels();
	if (NumLODs <= 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("TransferWrapMeshNormals: '%s' has no source models."), *StaticMesh->GetName());
		return;
	}

	UE_LOG(ArResourceProcessor, Log,
		TEXT("TransferWrapMeshNormals: Starting on '%s' (NumLODs=%d, WrapOffset=%.3f, SmoothIter=%d, VoxelCount=%d, MinTriangles=%d, Blacklist=%d)"),
		*StaticMesh->GetName(), NumLODs, WrapOffset, SmoothIterations, VoxelCount, MinTriangleCount,
		MaterialIDNameBlacklist.Num());

	StaticMesh->Modify();

	int32 SuccessLODCount = 0;

	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMeshDescription* MeshDesc = StaticMesh->GetMeshDescription(LODIndex);
		if (!MeshDesc)
		{
			UE_LOG(ArResourceProcessor, Warning,
				TEXT("  -> Skipping LOD%d on '%s': no MeshDescription."),
				LODIndex, *StaticMesh->GetName());
			continue;
		}

		const FMeshProcessingFilter ProcessingFilter = BuildMeshProcessingFilter(
			*StaticMesh, *MeshDesc, MaterialIDNameBlacklist, MinTriangleCount, LODIndex);
		if (ProcessingFilter.ProcessableTriangleCount <= 0)
		{
			UE_LOG(ArResourceProcessor, Warning,
				TEXT("  -> Skipping LOD%d on '%s': no material group passed the filters."),
				LODIndex, *StaticMesh->GetName());
			continue;
		}

		UE_LOG(ArResourceProcessor, Log,
			TEXT("  -> Processing LOD%d (%d filtered triangles) ..."),
			LODIndex, ProcessingFilter.ProcessableTriangleCount);

		FDynamicMesh3 OrigDynMesh;
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(MeshDesc, OrigDynMesh);
		}
		const int32 MaxVID = OrigDynMesh.MaxVertexID();
		
		FDynamicMesh3 WrapDynMesh = BuildWrapMesh(OrigDynMesh, WrapOffset, SmoothIterations, VoxelCount);
		
		const double SignedVolume = TMeshQueries<FDynamicMesh3>::GetVolumeNonWatertight(WrapDynMesh, 1.0);
		if (SignedVolume < 0.0)
		{
			WrapDynMesh.ReverseOrientation();
			UE_LOG(ArResourceProcessor, Log,
				TEXT("  -> LOD%d wrap winding was inward (vol=%.3f) – reversed to face outward."),
				LODIndex, SignedVolume);
		}

		// Soft per-vertex normals on the (now outward-oriented) wrap surface.
		FMeshNormals WrapNormals(&WrapDynMesh);
		WrapNormals.ComputeVertexNormals();
		const TArray<FVector3d>& WrapNormalArr = WrapNormals.GetNormals();

		TMeshAABBTree3<FDynamicMesh3> AABBTree(&WrapDynMesh, /*bBuild=*/true);
		
		TArray<FVector3f> TransferredNormals;
		TransferredNormals.Init(FVector3f::ZeroVector, MaxVID);
		TArray<bool> VertHit;
		VertHit.Init(false, MaxVID);

		ParallelFor(MaxVID, [&](int32 VID)
		{
			if (!OrigDynMesh.IsVertex(VID))
			{
				return;
			}

			const FVector3d VPos = OrigDynMesh.GetVertex(VID);

			double NearDistSq = TNumericLimits<double>::Max();
			const int32 TID = AABBTree.FindNearestTriangle(VPos, NearDistSq);
			if (TID == IndexConstants::InvalidID)
			{
				return;
			}

			const FIndex3i Tri = WrapDynMesh.GetTriangle(TID);
			const FVector3d V0 = WrapDynMesh.GetVertex(Tri.A);
			const FVector3d V1 = WrapDynMesh.GetVertex(Tri.B);
			const FVector3d V2 = WrapDynMesh.GetVertex(Tri.C);

			// Barycentric coords of the closest point on the triangle.
			FDistPoint3Triangle3d DistQuery(VPos, FTriangle3d(V0, V1, V2));
			DistQuery.Get();
			const FVector3d Bary = DistQuery.TriangleBaryCoords;

			FVector3d N = Bary.X * WrapNormalArr[Tri.A]
			            + Bary.Y * WrapNormalArr[Tri.B]
			            + Bary.Z * WrapNormalArr[Tri.C];
			if (!N.Normalize())
			{
				// Degenerate interpolation – fall back to the face normal.
				N = VectorUtil::NormalDirection(V0, V1, V2);
			}

			TransferredNormals[VID] = (FVector3f)N;
			VertHit[VID]            = true;
		});

		{
			FStaticMeshAttributes Attributes(*MeshDesc);
			Attributes.Register(/*bKeepExistingAttribute*/ true);

			TVertexInstanceAttributesRef<FVector3f> Normals =
				Attributes.GetVertexInstanceNormals();

			if (!Normals.IsValid())
			{
				UE_LOG(ArResourceProcessor, Warning,
					TEXT("  -> LOD%d: could not get vertex instance normal attribute – skipping."),
					LODIndex);
				continue;
			}

			int32 UpdatedVertexInstanceCount = 0;
			for (const FTriangleID& TriangleID : MeshDesc->Triangles().GetElementIDs())
			{
				if (!ProcessingFilter.ProcessablePolygonGroups.Contains(MeshDesc->GetTrianglePolygonGroup(TriangleID)))
				{
					continue;
				}

				for (const FVertexInstanceID& VIID : MeshDesc->GetTriangleVertexInstances(TriangleID))
				{
					const FVertexID VertID = MeshDesc->GetVertexInstanceVertex(VIID);
					const int32     VID    = VertID.GetValue();
					if (VID < MaxVID && VertHit[VID])
					{
						Normals[VIID] = TransferredNormals[VID];
						++UpdatedVertexInstanceCount;
					}
				}
			}

			if (UpdatedVertexInstanceCount == 0)
			{
				UE_LOG(ArResourceProcessor, Warning,
					TEXT("  -> LOD%d: no vertex instances were updated after filtering."),
					LODIndex);
				continue;
			}
		}
		// Keep our custom normals: don't let the build recompute them, but do
		// recompute tangents so the tangent basis stays consistent.
		FStaticMeshSourceModel& SrcModel = StaticMesh->GetSourceModel(LODIndex);
		SrcModel.BuildSettings.bRecomputeNormals  = false;
		SrcModel.BuildSettings.bRecomputeTangents = true;

		StaticMesh->CommitMeshDescription(LODIndex);
		++SuccessLODCount;

		UE_LOG(ArResourceProcessor, Log, TEXT("  <- LOD%d done."), LODIndex);
	}

	if (SuccessLODCount == 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("TransferWrapMeshNormals: no LOD was processed on '%s'."),
			*StaticMesh->GetName());
		return;
	}

	StaticMesh->PostEditChange();

	UE_LOG(ArResourceProcessor, Log,
		TEXT("TransferWrapMeshNormals: Done. Transferred wrap normals on '%s' (%d/%d LODs)."),
		*StaticMesh->GetName(), SuccessLODCount, NumLODs);
}

int32 UArtResourceToolsBPLibrary::FlipSelectedTexturesV()
{
	TArray<FAssetData> SelectedAssets;
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	if (SelectedAssets.Num() == 0)
	{
		UE_LOG(ArResourceProcessor, Warning, TEXT("FlipSelectedTexturesV: no assets selected."));
		return 0;
	}

	TArray<UTexture2D*> SelectedTextures;
	for (const FAssetData& AssetData : SelectedAssets)
	{
		UTexture2D* Texture = Cast<UTexture2D>(AssetData.GetAsset());
		if (Texture)
		{
			SelectedTextures.Add(Texture);
		}
	}

	if (SelectedTextures.Num() == 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("FlipSelectedTexturesV: selected assets do not include Texture2D assets."));
		return 0;
	}

	return FlipTexturesV(SelectedTextures);
}

int32 UArtResourceToolsBPLibrary::FlipTexturesV(const TArray<UTexture2D*>& Textures)
{
	if (Textures.Num() == 0)
	{
		UE_LOG(ArResourceProcessor, Warning,
			TEXT("FlipTexturesV: no Texture2D assets supplied."));
		return 0;
	}

	int32 ValidTextureCount = 0;
	int32 ModifiedTextureCount = 0;
	FScopedTransaction Transaction(
		TEXT("ArtResourceTools"),
		NSLOCTEXT("ArtResourceTools", "FlipTexturesV", "Flip Textures V"),
		nullptr);

	for (UTexture2D* Texture : Textures)
	{
		if (!IsValid(Texture))
		{
			continue;
		}

		++ValidTextureCount;
		if (FlipTextureSourceV(Texture))
		{
			++ModifiedTextureCount;
			UE_LOG(ArResourceProcessor, Log,
				TEXT("FlipTexturesV: flipped '%s'."), *Texture->GetPathName());
		}
	}

	if (ModifiedTextureCount == 0)
	{
		Transaction.Cancel();
	}

	UE_LOG(ArResourceProcessor, Log,
		TEXT("FlipTexturesV: Done. Modified %d/%d Texture2D assets."),
		ModifiedTextureCount, ValidTextureCount);
	return ModifiedTextureCount;
}

#endif // WITH_EDITOR
