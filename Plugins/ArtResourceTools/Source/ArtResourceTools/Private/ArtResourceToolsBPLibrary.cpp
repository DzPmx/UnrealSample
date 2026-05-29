#include "ArtResourceToolsBPLibrary.h"

// GENERATED_UCLASS_BODY() declares this constructor; it must be defined in ALL
// build configurations (the generated reflection code references it even when
// WITH_EDITOR is 0), otherwise packaged/game builds fail to link.
UArtResourceToolsBPLibrary::UArtResourceToolsBPLibrary(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
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

static TArray<FVector3d> BuildIcosphereDirections(int32 Subdivisions)
{
	// Golden-ratio icosahedron base vertices
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
			// Pack edge key (order-independent)
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

	return Verts; // Each vertex is a unit-length direction
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


static FDynamicMesh3 BuildWrapMesh(const FDynamicMesh3& OrigMesh,
                                    float WrapOffset,
                                    int32 SmoothIterations,
                                    int32 VoxelCount)
{
	// Make a mutable copy because FDynamicMeshAABBTree3 wants a non-const ptr.
	FDynamicMesh3 SourceCopy(OrigMesh);

	// Voxel grid bounds = source bounds expanded by WrapOffset on every side
	// so the produced envelope can extend beyond the original silhouette.
	FAxisAlignedBox3d Bounds = SourceCopy.GetBounds(true);
	const double Pad = FMath::Max((double)WrapOffset * 2.0, 1.0);
	Bounds.Expand(Pad);

	const int32  SafeVoxels = FMath::Max(VoxelCount, 8);
	const double CellSize   = Bounds.MaxDim() / (double)SafeVoxels;

	// Band width = how far from any triangle a voxel is still "inside".
	// This is the dilation distance (≈ Blender's Extrude(0.5) + voxel size).
	// We make sure it spans at least ~1.5 voxel cells so neighbouring cards
	// fuse into one connected envelope after marching cubes.
	const double Band = FMath::Max((double)WrapOffset, CellSize * 1.5);

	// Spatial tree on the original cards
	FDynamicMeshAABBTree3 SourceTree(&SourceCopy, /*bAutoBuild=*/true);

	IMeshSpatial::FQueryOptions QOpts;
	QOpts.MaxDistance = Band * 2.0; // early-out for cells far outside the band

	// ------------------ Marching cubes -------------------------------------
	FMarchingCubes MC;
	MC.Bounds   = Bounds;
	MC.CubeSize = CellSize;
	MC.IsoValue = 0.0;
	MC.RootMode = ERootfindingModes::LerpSteps;
	MC.RootModeSteps = 3;

	// Implicit scalar field: signed distance to the band surface.
	//   < 0  →  inside the dilated mesh (within `Band` of any triangle)
	//   > 0  →  outside
	// Marching cubes extracts the iso-surface at f == 0, i.e. the outer
	// surface of the dilated mesh.  This is the "wrap mesh".
	MC.Implicit = [&SourceTree, &QOpts, Band](const FVector3d& P) -> double
	{
		double NearDistSq = TNumericLimits<double>::Max();
		const int32 TID = SourceTree.FindNearestTriangle(P, NearDistSq, QOpts);
		if (TID == IndexConstants::InvalidID)
		{
			// No triangle within MaxDistance → definitely outside the band
			return Band;
		}
		return FMath::Sqrt(NearDistSq) - Band;
	};

	MC.Generate();
	MC.Implicit = nullptr; // release the lambda capture before SourceTree dies

	// ------------------ Build FDynamicMesh3 from MC output -----------------
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

	// Final smoothing (Blender SMOOTH modifier, default 25 iterations)
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

		// Sign: dot(ray_dir, face_normal) < 0  =>  front-face hit => positive (outside)
		//       dot > 0                         =>  back-face hit  => negative (inside)
		FVector3d FaceNormal = VectorUtil::NormalDirection(V0, V1, V2);
		double    DotVal     = Dir.Dot(FaceNormal);
		double    SignedDist = FMath::Sign(DotVal) * Dist; // positive outside, negative inside
		// Negate to match Blender convention: copysign(dist, -dot(dir, normal))
		SignedDist = -SignedDist;

		if (!bOutHit || FMath::Abs(SignedDist) < FMath::Abs(BestSignDist))
		{
			BestSignDist = SignedDist;
			bOutHit      = true;
		}
	}

	return BestSignDist;
}

void UArtResourceToolsBPLibrary::BakeSDFAOToVertexColorAlpha(UStaticMesh* StaticMesh, float WrapOffset,
	int32 SmoothIterations, int32 IcoSubdivisions, int32 VoxelCount, float AOPower, bool bInvertAO)
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
		TEXT("BakeSDFAOToVertexColorAlpha: Starting bake on '%s' (NumLODs=%d, WrapOffset=%.3f, SmoothIter=%d, IcoSub=%d, VoxelCount=%d)"),
		*StaticMesh->GetName(), NumLODs, WrapOffset, SmoothIterations, IcoSubdivisions, VoxelCount);

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

		UE_LOG(ArResourceProcessor, Log, TEXT("  -> Baking LOD%d ..."), LODIndex);

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

		// Per-vertex raycasting is the expensive part; run it in parallel.
		// The AABB tree is read-only once built, so concurrent queries are safe.
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
		
		// Neutral AO value for vertices that no ray ever hit (treat as unoccluded).
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

			for (const FVertexInstanceID& VIID : MeshDesc->VertexInstances().GetElementIDs())
			{
				const FVertexID VertID = MeshDesc->GetVertexInstanceVertex(VIID);
				const int32     VID    = VertID.GetValue();
				if (VID < MaxVID && OrigDynMesh.IsVertex(VID))
				{
					FVector4f C = Colors[VIID];
					C.W = (float)SignDists[VID]; // write to Alpha channel
					Colors[VIID] = C;
				}
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

void UArtResourceToolsBPLibrary::TransferWrapMeshNormals(UStaticMesh* StaticMesh, float WrapOffset,
	int32 SmoothIterations, int32 VoxelCount)
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
		TEXT("TransferWrapMeshNormals: Starting on '%s' (NumLODs=%d, WrapOffset=%.3f, SmoothIter=%d, VoxelCount=%d)"),
		*StaticMesh->GetName(), NumLODs, WrapOffset, SmoothIterations, VoxelCount);

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

		UE_LOG(ArResourceProcessor, Log, TEXT("  -> Processing LOD%d ..."), LODIndex);

		FDynamicMesh3 OrigDynMesh;
		{
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(MeshDesc, OrigDynMesh);
		}
		const int32 MaxVID = OrigDynMesh.MaxVertexID();

		// Build the smoothed wrap envelope (Mesh->Volume->Mesh + Smooth).
		FDynamicMesh3 WrapDynMesh = BuildWrapMesh(OrigDynMesh, WrapOffset, SmoothIterations, VoxelCount);

		// IMPORTANT: Marching-cubes triangle winding is table-driven and can come
		// out inward-facing, which would flip every transferred normal. Use the
		// signed volume as a deterministic orientation reference: a positive
		// volume means the winding (and therefore FMeshNormals) points outward.
		// If it's negative we reverse the mesh so normals face outward.
		// NOTE: no coordinate-space conversion is needed here — UE static meshes
		// and FDynamicMesh3 share the same local space (left-handed, Z-up); the
		// only thing that matters is consistent outward orientation.
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

		// For every original vertex, find the nearest point on the wrap mesh and
		// barycentrically interpolate the wrap's soft normal there.
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

			for (const FVertexInstanceID& VIID : MeshDesc->VertexInstances().GetElementIDs())
			{
				const FVertexID VertID = MeshDesc->GetVertexInstanceVertex(VIID);
				const int32     VID    = VertID.GetValue();
				if (VID < MaxVID && VertHit[VID])
				{
					Normals[VIID] = TransferredNormals[VID];
				}
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

#endif // WITH_EDITOR


