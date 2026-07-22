#include "FoliageBakerCardBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerL1Visibility.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedAtlasBake.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "MeshReductionSettings.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "Modules/ModuleManager.h"
#include "OverlappingCorners.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerCards, Log, All);

namespace
{
	bool UsesDoublePlanesBillboard(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			&& Request.BillboardPlaneMode == EFoliageBakerBillboardPlaneMode::DoublePlanes;
	}

	bool UsesSeparateOneSidedCrossFaces(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			&& Request.CrossCardGeometryMode == EFoliageBakerCrossCardGeometryMode::SeparateOneSidedFaces;
	}

	bool UsesMultiBillboard(const FFoliageBakerCardBakeRequest& Request)
	{
		return Request.Mode == EFoliageBakerCardBakeMode::MultiBillboard;
	}

	int32 GetDesiredCardUVChannelCount(const FFoliageBakerCardBakeRequest& Request)
	{
		if (UsesDoublePlanesBillboard(Request) || UsesMultiBillboard(Request))
		{
			return 3;
		}
		return UsesSeparateOneSidedCrossFaces(Request) ? 1 : 2;
	}

	bool ComputeSourceTriangleBounds(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		FBoxSphereBounds& OutBounds)
	{
		FBox Bounds(ForceInit);
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Bounds += Vertex;
			}
		}
		if (!Bounds.IsValid)
		{
			OutBounds = FBoxSphereBounds(ForceInitToZero);
			return false;
		}

		OutBounds = FBoxSphereBounds(Bounds);
		return true;
	}

	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const FFoliageBakerCardBakeRequest& Request)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		const int32 ClampedTextureResolution = FMath::Clamp(Request.TextureResolution, 256, 4096);
		Settings.TextureAtlasResolution = Request.bTrimUnusedAtlasSpace
			? ClampedTextureResolution
			: static_cast<int32>(
				1u << FMath::FloorLog2(static_cast<uint32>(ClampedTextureResolution)));
		Settings.DoubleSidedBakeMode = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes
			: UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
		Settings.bEmitBackFaceGeometry = UsesSeparateOneSidedCrossFaces(Request);
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
		Settings.TrunkCardAtlasScale = 1.0;
		FBoxSphereBounds SourceBounds(ForceInitToZero);
		ComputeSourceTriangleBounds(SourceTriangles, SourceBounds);
		Settings.ErrorTolerance = FMath::Max(0.01, static_cast<double>(SourceBounds.SphereRadius) * 1.0e-6);
		Settings.bEnableAlphaAwareTileCrop = true;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
		return Settings;
	}

	struct FTrunkLeafClassification
	{
		int32 MatchedMaterialCount = 0;
		int32 TrunkTriangleCount = 0;
		int32 LeafTriangleCount = 0;
		int32 LeafComponentCount = 0;
		int32 GeneratedClusterCount = 0;
		int32 GeneratedBillboardCount = 0;
	};

	FTrunkLeafClassification ClassifyTrianglesForTrunkLeafMask(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<FString>& RawKeywords)
	{
		FTrunkLeafClassification Classification;
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(StaticMesh, RawKeywords);
		Classification.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;

		for (UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : SourceTriangles)
		{
			const bool bIsTrunk = MaterialMatches.IsMatch(Triangle.MaterialIndex);
			Triangle.bIsTrunk = bIsTrunk;
			if (bIsTrunk)
			{
				++Classification.TrunkTriangleCount;
			}
		}

		return Classification;
	}

	struct FQuantizedPositionKey
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;

		bool operator==(const FQuantizedPositionKey& Other) const
		{
			return X == Other.X && Y == Other.Y && Z == Other.Z;
		}

		friend uint32 GetTypeHash(const FQuantizedPositionKey& Key)
		{
			return HashCombineFast(
				HashCombineFast(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)),
				::GetTypeHash(Key.Z));
		}
	};

	struct FMultiBillboardComponent
	{
		TArray<int32> TriangleIndices;
		FVector Center = FVector::ZeroVector;
		double Area = 0.0;
	};

	struct FMultiBillboardCluster
	{
		TArray<int32> ComponentIndices;
		FVector Center = FVector::ZeroVector;
		double Area = 0.0;
	};

	struct FMultiBillboardLayer
	{
		TArray<int32> TriangleIndices;
		double Rho = 0.0;
		double Area = 0.0;
	};

	int32 FindComponentRoot(TArray<int32>& Parents, int32 Index)
	{
		int32 Root = Index;
		while (Parents[Root] != Root)
		{
			Root = Parents[Root];
		}
		while (Parents[Index] != Index)
		{
			const int32 Parent = Parents[Index];
			Parents[Index] = Root;
			Index = Parent;
		}
		return Root;
	}

	void UnionComponents(TArray<int32>& Parents, TArray<uint8>& Ranks, int32 A, int32 B)
	{
		int32 RootA = FindComponentRoot(Parents, A);
		int32 RootB = FindComponentRoot(Parents, B);
		if (RootA == RootB)
		{
			return;
		}
		if (Ranks[RootA] < Ranks[RootB])
		{
			Swap(RootA, RootB);
		}
		Parents[RootB] = RootA;
		if (Ranks[RootA] == Ranks[RootB])
		{
			++Ranks[RootA];
		}
	}

	TArray<FMultiBillboardComponent> BuildConnectedLeafComponents(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const double PositionTolerance)
	{
		TArray<FMultiBillboardComponent> Components;
		if (Triangles.IsEmpty())
		{
			return Components;
		}

		TArray<int32> Parents;
		TArray<uint8> Ranks;
		Parents.SetNumUninitialized(Triangles.Num());
		Ranks.SetNumZeroed(Triangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			Parents[TriangleIndex] = TriangleIndex;
		}

		const double SafeTolerance = FMath::Max(PositionTolerance, 1.0e-6);
		const double InverseTolerance = 1.0 / SafeTolerance;
		TMap<FQuantizedPositionKey, int32> FirstTriangleByPosition;
		FirstTriangleByPosition.Reserve(Triangles.Num() * 3);
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			for (const FVector& Vertex : Triangles[TriangleIndex].Vertices)
			{
				const FQuantizedPositionKey Key{
					FMath::RoundToInt64(Vertex.X * InverseTolerance),
					FMath::RoundToInt64(Vertex.Y * InverseTolerance),
					FMath::RoundToInt64(Vertex.Z * InverseTolerance)
				};
				if (const int32* ExistingTriangle = FirstTriangleByPosition.Find(Key))
				{
					UnionComponents(Parents, Ranks, TriangleIndex, *ExistingTriangle);
				}
				else
				{
					FirstTriangleByPosition.Add(Key, TriangleIndex);
				}
			}
		}

		TMap<int32, int32> ComponentIndexByRoot;
		ComponentIndexByRoot.Reserve(Triangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			const int32 Root = FindComponentRoot(Parents, TriangleIndex);
			int32 ComponentIndex = INDEX_NONE;
			if (const int32* ExistingComponentIndex = ComponentIndexByRoot.Find(Root))
			{
				ComponentIndex = *ExistingComponentIndex;
			}
			else
			{
				ComponentIndex = Components.AddDefaulted();
				ComponentIndexByRoot.Add(Root, ComponentIndex);
			}
			FMultiBillboardComponent& Component = Components[ComponentIndex];
			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector TriangleCenter =
				(Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
			const double TriangleArea = FMath::Max(Triangle.Area, UE_DOUBLE_SMALL_NUMBER);
			Component.TriangleIndices.Add(TriangleIndex);
			Component.Center += TriangleCenter * TriangleArea;
			Component.Area += TriangleArea;
		}

		for (FMultiBillboardComponent& Component : Components)
		{
			Component.Center /= FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Components;
	}

	TArray<FMultiBillboardCluster> ClusterLeafComponents(
		const TArray<FMultiBillboardComponent>& Components,
		const int32 RequestedClusterCount)
	{
		TArray<FMultiBillboardCluster> Clusters;
		if (Components.IsEmpty())
		{
			return Clusters;
		}

		const int32 ClusterCount = FMath::Clamp(RequestedClusterCount, 1, Components.Num());
		TArray<FVector> Centers;
		Centers.Reserve(ClusterCount);
		int32 FirstCenterIndex = 0;
		for (int32 ComponentIndex = 1; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			if (Components[ComponentIndex].Area > Components[FirstCenterIndex].Area)
			{
				FirstCenterIndex = ComponentIndex;
			}
		}
		Centers.Add(Components[FirstCenterIndex].Center);

		while (Centers.Num() < ClusterCount)
		{
			int32 FarthestComponentIndex = INDEX_NONE;
			double FarthestDistanceSquared = -1.0;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				double NearestDistanceSquared = TNumericLimits<double>::Max();
				for (const FVector& Center : Centers)
				{
					NearestDistanceSquared = FMath::Min(
						NearestDistanceSquared,
						FVector::DistSquared(Components[ComponentIndex].Center, Center));
				}
				if (NearestDistanceSquared > FarthestDistanceSquared)
				{
					FarthestDistanceSquared = NearestDistanceSquared;
					FarthestComponentIndex = ComponentIndex;
				}
			}
			if (FarthestComponentIndex == INDEX_NONE)
			{
				break;
			}
			Centers.Add(Components[FarthestComponentIndex].Center);
		}

		TArray<int32> Assignments;
		Assignments.Init(INDEX_NONE, Components.Num());
		constexpr int32 ClusteringIterations = 32;
		for (int32 Iteration = 0; Iteration < ClusteringIterations; ++Iteration)
		{
			bool bAssignmentChanged = false;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				int32 NearestClusterIndex = 0;
				double NearestDistanceSquared =
					FVector::DistSquared(Components[ComponentIndex].Center, Centers[0]);
				for (int32 ClusterIndex = 1; ClusterIndex < Centers.Num(); ++ClusterIndex)
				{
					const double DistanceSquared =
						FVector::DistSquared(Components[ComponentIndex].Center, Centers[ClusterIndex]);
					if (DistanceSquared < NearestDistanceSquared)
					{
						NearestDistanceSquared = DistanceSquared;
						NearestClusterIndex = ClusterIndex;
					}
				}
				if (Assignments[ComponentIndex] != NearestClusterIndex)
				{
					Assignments[ComponentIndex] = NearestClusterIndex;
					bAssignmentChanged = true;
				}
			}

			TArray<FVector> WeightedCenterSums;
			TArray<double> WeightSums;
			WeightedCenterSums.Init(FVector::ZeroVector, Centers.Num());
			WeightSums.Init(0.0, Centers.Num());
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				const int32 ClusterIndex = Assignments[ComponentIndex];
				const double Weight = FMath::Max(Components[ComponentIndex].Area, UE_DOUBLE_SMALL_NUMBER);
				WeightedCenterSums[ClusterIndex] += Components[ComponentIndex].Center * Weight;
				WeightSums[ClusterIndex] += Weight;
			}
			for (int32 ClusterIndex = 0; ClusterIndex < Centers.Num(); ++ClusterIndex)
			{
				if (WeightSums[ClusterIndex] > 0.0)
				{
					Centers[ClusterIndex] = WeightedCenterSums[ClusterIndex] / WeightSums[ClusterIndex];
				}
			}
			if (!bAssignmentChanged)
			{
				break;
			}
		}

		Clusters.SetNum(Centers.Num());
		for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			FMultiBillboardCluster& Cluster = Clusters[Assignments[ComponentIndex]];
			const FMultiBillboardComponent& Component = Components[ComponentIndex];
			const double Weight = FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
			Cluster.ComponentIndices.Add(ComponentIndex);
			Cluster.Center += Component.Center * Weight;
			Cluster.Area += Weight;
		}
		Clusters.RemoveAll([](const FMultiBillboardCluster& Cluster)
		{
			return Cluster.ComponentIndices.IsEmpty();
		});
		for (FMultiBillboardCluster& Cluster : Clusters)
		{
			Cluster.Center /= FMath::Max(Cluster.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Clusters;
	}

	TArray<FMultiBillboardLayer> BuildMultiBillboardClusterLayers(
		const TArray<FMultiBillboardComponent>& Components,
		const FMultiBillboardCluster& Cluster,
		const FVector& CaptureNormal,
		const int32 RequestedLayerCount)
	{
		TArray<FMultiBillboardLayer> Layers;
		if (Cluster.ComponentIndices.IsEmpty())
		{
			return Layers;
		}

		const int32 LayerCount = FMath::Clamp(
			RequestedLayerCount,
			1,
			Cluster.ComponentIndices.Num());
		double MinDepth = TNumericLimits<double>::Max();
		double MaxDepth = -TNumericLimits<double>::Max();
		for (const int32 ComponentIndex : Cluster.ComponentIndices)
		{
			const double Depth = FVector::DotProduct(Components[ComponentIndex].Center, CaptureNormal);
			MinDepth = FMath::Min(MinDepth, Depth);
			MaxDepth = FMath::Max(MaxDepth, Depth);
		}

		const double DepthRange = MaxDepth - MinDepth;
		const double DepthEpsilon = 1.0e-6;
		const int32 EffectiveLayerCount = DepthRange > DepthEpsilon ? LayerCount : 1;
		Layers.SetNum(EffectiveLayerCount);
		TArray<double> WeightedDepthSums;
		WeightedDepthSums.Init(0.0, EffectiveLayerCount);
		for (const int32 ComponentIndex : Cluster.ComponentIndices)
		{
			const FMultiBillboardComponent& Component = Components[ComponentIndex];
			const double Depth = FVector::DotProduct(Component.Center, CaptureNormal);
			const double NormalizedDepth = DepthRange > DepthEpsilon
				? FMath::Clamp((Depth - MinDepth) / DepthRange, 0.0, 1.0)
				: 0.0;
			const int32 LayerIndex = FMath::Min(
				FMath::FloorToInt(NormalizedDepth * static_cast<double>(EffectiveLayerCount)),
				EffectiveLayerCount - 1);
			FMultiBillboardLayer& Layer = Layers[LayerIndex];
			const double Weight = FMath::Max(Component.Area, UE_DOUBLE_SMALL_NUMBER);
			Layer.TriangleIndices.Append(Component.TriangleIndices);
			Layer.Area += Weight;
			WeightedDepthSums[LayerIndex] += Depth * Weight;
		}

		for (int32 LayerIndex = Layers.Num() - 1; LayerIndex >= 0; --LayerIndex)
		{
			FMultiBillboardLayer& Layer = Layers[LayerIndex];
			if (Layer.TriangleIndices.IsEmpty())
			{
				Layers.RemoveAt(LayerIndex);
				WeightedDepthSums.RemoveAt(LayerIndex);
				continue;
			}
			Layer.Rho =
				WeightedDepthSums[LayerIndex]
				/ FMath::Max(Layer.Area, UE_DOUBLE_SMALL_NUMBER);
		}
		return Layers;
	}

	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;

	int32 MergeDoublePlaneTileCrops(
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop>& TileCrops)
	{
		if (TileCrops.Num() != 2 || !TileCrops[0].bEnabled || !TileCrops[1].bEnabled)
		{
			for (UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop& Crop : TileCrops)
			{
				Crop = UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop();
			}
			return 0;
		}

		UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop SharedCrop;
		SharedCrop.bEnabled = true;
		SharedCrop.MinUFraction = FMath::Min(TileCrops[0].MinUFraction, TileCrops[1].MinUFraction);
		SharedCrop.MaxUFraction = FMath::Max(TileCrops[0].MaxUFraction, TileCrops[1].MaxUFraction);
		SharedCrop.MinVFraction = FMath::Min(TileCrops[0].MinVFraction, TileCrops[1].MinVFraction);
		SharedCrop.MaxVFraction = FMath::Max(TileCrops[0].MaxVFraction, TileCrops[1].MaxVFraction);

		constexpr double CropEpsilon = 1.0e-5;
		const bool bCropsTile = SharedCrop.MinUFraction > CropEpsilon
			|| SharedCrop.MaxUFraction < 1.0 - CropEpsilon
			|| SharedCrop.MinVFraction > CropEpsilon
			|| SharedCrop.MaxVFraction < 1.0 - CropEpsilon;
		if (!bCropsTile
			|| SharedCrop.MaxUFraction <= SharedCrop.MinUFraction
			|| SharedCrop.MaxVFraction <= SharedCrop.MinVFraction)
		{
			for (UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop& Crop : TileCrops)
			{
				Crop = UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop();
			}
			return 0;
		}

		TileCrops[0] = SharedCrop;
		TileCrops[1] = SharedCrop;
		return 2;
	}

	int32 MergeGroupedTileCrops(
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop>& TileCrops,
		const TArray<int32>& PlaneGroupIndices)
	{
		if (TileCrops.Num() != PlaneGroupIndices.Num())
		{
			return 0;
		}

		TMap<int32, UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> SharedCrops;
		TSet<int32> GroupsWithoutCrop;
		for (int32 PlaneIndex = 0; PlaneIndex < TileCrops.Num(); ++PlaneIndex)
		{
			const int32 GroupIndex = PlaneGroupIndices[PlaneIndex];
			const UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop& Crop = TileCrops[PlaneIndex];
			if (!Crop.bEnabled)
			{
				GroupsWithoutCrop.Add(GroupIndex);
				continue;
			}
			UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop& SharedCrop =
				SharedCrops.FindOrAdd(GroupIndex);
			if (!SharedCrop.bEnabled)
			{
				SharedCrop = Crop;
			}
			else
			{
				SharedCrop.MinUFraction = FMath::Min(SharedCrop.MinUFraction, Crop.MinUFraction);
				SharedCrop.MaxUFraction = FMath::Max(SharedCrop.MaxUFraction, Crop.MaxUFraction);
				SharedCrop.MinVFraction = FMath::Min(SharedCrop.MinVFraction, Crop.MinVFraction);
				SharedCrop.MaxVFraction = FMath::Max(SharedCrop.MaxVFraction, Crop.MaxVFraction);
			}
		}

		constexpr double CropEpsilon = 1.0e-5;
		int32 CroppedPlaneCount = 0;
		for (int32 PlaneIndex = 0; PlaneIndex < TileCrops.Num(); ++PlaneIndex)
		{
			const int32 GroupIndex = PlaneGroupIndices[PlaneIndex];
			if (GroupsWithoutCrop.Contains(GroupIndex))
			{
				TileCrops[PlaneIndex] =
					UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop();
				continue;
			}
			const UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop* SharedCrop =
				SharedCrops.Find(GroupIndex);
			const bool bValidCrop = SharedCrop
				&& SharedCrop->bEnabled
				&& SharedCrop->MaxUFraction > SharedCrop->MinUFraction
				&& SharedCrop->MaxVFraction > SharedCrop->MinVFraction
				&& (SharedCrop->MinUFraction > CropEpsilon
					|| SharedCrop->MaxUFraction < 1.0 - CropEpsilon
					|| SharedCrop->MinVFraction > CropEpsilon
					|| SharedCrop->MaxVFraction < 1.0 - CropEpsilon);
			if (!bValidCrop)
			{
				TileCrops[PlaneIndex] =
					UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop();
				continue;
			}
			TileCrops[PlaneIndex] = *SharedCrop;
			++CroppedPlaneCount;
		}
		return CroppedPlaneCount;
	}

	using FAtlasBakeStats = UE::FoliageBaker::ProjectedAtlasBake::FStats;

	bool BakeCardAtlasOrthographic(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats& ProxyStats,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const FAtlasOutputSelection& OutputSelection,
		const bool bConvertNormalsToCaptureFrame,
		const bool bCaptureSourceDepth,
		TArray<FColor>& OutPixels,
		TArray<FColor>& OutNormalPixels,
		TArray<FColor>& OutMixPixels,
		TArray<FColor>& OutSourceTriangleIdAndDepth,
		FAtlasBakeStats& OutStats,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FRequest Request;
		Request.SourceStaticMesh = &SourceStaticMesh;
		Request.SourceLODBounds = SourceLODBounds;
		Request.Triangles = &Triangles;
		Request.PlaneInfos = &PlaneInfos;
		Request.ProxyStats = &ProxyStats;
		Request.Settings = &Settings;
		Request.OutputSelection = OutputSelection;
		Request.NormalAlphaMode =
			UE::FoliageBaker::ProjectedAtlasBake::ENormalAlphaMode::TrunkLeafClassification;
		Request.bConvertNormalsToCaptureFrame = bConvertNormalsToCaptureFrame;
		Request.bCaptureSourceTriangleIdAndDepth = bCaptureSourceDepth;
		Request.DiagnosticName = TEXT("Card atlas");
		Request.MaterialAlphaPolicyDetails =
			TEXT("\n    card BaseColor/source-triangle-id/normal/Mix=per-tile source masked shader with shared GPU depth; all materials compete in one depth buffer; no CPU material-property fallback");

		UE::FoliageBaker::ProjectedAtlasBake::FResult Result;
		if (!UE::FoliageBaker::ProjectedAtlasBake::Bake(Request, Result, OutError))
		{
			return false;
		}

		OutPixels = MoveTemp(Result.BaseColorOpacityPixels);
		OutNormalPixels = MoveTemp(Result.NormalPixels);
		OutMixPixels = MoveTemp(Result.MixPixels);
		OutSourceTriangleIdAndDepth =
			MoveTemp(Result.SourceTriangleIdAndDepthPixels);
		OutStats = MoveTemp(Result.Stats);
		return true;
	}

	enum class EAtlasOuterCropMode : uint8
	{
		TightBlockAligned,
		PowerOfTwoUsedBounds
	};

	bool CropAtlasToUsedSpace(
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FMeshDescription& MeshDescription,
		TArray<FColor>& AtlasPixels,
		TArray<FColor>& NormalAtlasPixels,
		TArray<FColor>& MixAtlasPixels,
		TArray<FColor>& SourceTriangleIdAndDepthPixels,
		FAtlasBakeStats& AtlasStats,
		const EAtlasOuterCropMode CropMode,
		FString& OutError)
	{
		const int32 OldWidth = AtlasStats.Width;
		const int32 OldHeight = AtlasStats.Height;
		if (OldWidth <= 0 || OldHeight <= 0 || PlaneInfos.IsEmpty())
		{
			return true;
		}

		int32 UsedMinX = OldWidth;
		int32 UsedMinY = OldHeight;
		int32 UsedMaxX = 0;
		int32 UsedMaxY = 0;
		auto AccumulateTileBounds = [&](const FIntPoint& PixelMin, const FIntPoint& TileSize, const int32 Padding)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			const int32 SafePadding = FMath::Max(0, Padding);
			UsedMinX = FMath::Min(UsedMinX, FMath::Max(0, PixelMin.X - SafePadding));
			UsedMinY = FMath::Min(UsedMinY, FMath::Max(0, PixelMin.Y - SafePadding));
			UsedMaxX = FMath::Max(UsedMaxX, FMath::Min(OldWidth, PixelMin.X + TileSize.X + SafePadding));
			UsedMaxY = FMath::Max(UsedMaxY, FMath::Min(OldHeight, PixelMin.Y + TileSize.Y + SafePadding));
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateTileBounds(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateTileBounds(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels);
			}
		}
		if (UsedMaxX <= UsedMinX || UsedMaxY <= UsedMinY)
		{
			OutError = TEXT("Could not determine the used atlas bounds for outer-space cropping.");
			return false;
		}

		constexpr int32 TextureBlockSize = 4;
		int32 CropMinX = 0;
		int32 CropMinY = 0;
		int32 NewWidth = OldWidth;
		int32 NewHeight = OldHeight;
		if (CropMode == EAtlasOuterCropMode::PowerOfTwoUsedBounds)
		{
			const int32 UsedWidth = UsedMaxX - UsedMinX;
			const int32 UsedHeight = UsedMaxY - UsedMinY;
			NewWidth = FMath::Min(
				OldWidth,
				static_cast<int32>(FMath::RoundUpToPowerOfTwo(
					static_cast<uint32>(FMath::Max(TextureBlockSize, UsedWidth)))));
			NewHeight = FMath::Min(
				OldHeight,
				static_cast<int32>(FMath::RoundUpToPowerOfTwo(
					static_cast<uint32>(FMath::Max(TextureBlockSize, UsedHeight)))));
			CropMinX = FMath::Clamp(UsedMinX, 0, OldWidth - NewWidth);
			CropMinY = FMath::Clamp(UsedMinY, 0, OldHeight - NewHeight);
		}
		else
		{
			CropMinX = FMath::Clamp(
				(UsedMinX / TextureBlockSize) * TextureBlockSize,
				0,
				OldWidth - 1);
			CropMinY = FMath::Clamp(
				(UsedMinY / TextureBlockSize) * TextureBlockSize,
				0,
				OldHeight - 1);
			const int32 CropMaxX = FMath::Clamp(
				((UsedMaxX + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize,
				CropMinX + 1,
				OldWidth);
			const int32 CropMaxY = FMath::Clamp(
				((UsedMaxY + TextureBlockSize - 1) / TextureBlockSize) * TextureBlockSize,
				CropMinY + 1,
				OldHeight);
			NewWidth = CropMaxX - CropMinX;
			NewHeight = CropMaxY - CropMinY;
		}
		if (CropMinX == 0 && CropMinY == 0 && NewWidth == OldWidth && NewHeight == OldHeight)
		{
			return true;
		}

		auto BuildCroppedPixels = [&](const TArray<FColor>& SourcePixels, TArray<FColor>& OutCroppedPixels) -> bool
		{
			OutCroppedPixels.Reset();
			if (SourcePixels.IsEmpty())
			{
				return true;
			}
			if (SourcePixels.Num() != OldWidth * OldHeight)
			{
				return false;
			}
			OutCroppedPixels.SetNumUninitialized(NewWidth * NewHeight);
			for (int32 Y = 0; Y < NewHeight; ++Y)
			{
				const FColor* SourceRow = SourcePixels.GetData() + (CropMinY + Y) * OldWidth + CropMinX;
				FColor* DestinationRow = OutCroppedPixels.GetData() + Y * NewWidth;
				FMemory::Memcpy(DestinationRow, SourceRow, static_cast<SIZE_T>(NewWidth) * sizeof(FColor));
			}
			return true;
		};

		TArray<FColor> CroppedAtlasPixels;
		TArray<FColor> CroppedNormalAtlasPixels;
		TArray<FColor> CroppedMixAtlasPixels;
		TArray<FColor> CroppedSourceTriangleIdAndDepthPixels;
		if (!BuildCroppedPixels(AtlasPixels, CroppedAtlasPixels)
			|| !BuildCroppedPixels(NormalAtlasPixels, CroppedNormalAtlasPixels)
			|| !BuildCroppedPixels(MixAtlasPixels, CroppedMixAtlasPixels)
			|| !BuildCroppedPixels(
				SourceTriangleIdAndDepthPixels,
				CroppedSourceTriangleIdAndDepthPixels))
		{
			OutError = TEXT("Atlas pixel count did not match the atlas dimensions during outer-space cropping.");
			return false;
		}

		FStaticMeshAttributes MeshAttributes(MeshDescription);
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();
		if (VertexInstanceUVs.GetNumChannels() < 2)
		{
			OutError = TEXT("Generated card mesh does not contain UV0 and UV1 for atlas cropping.");
			return false;
		}
		auto RemapAtlasUV = [&](const FVector2f& OldUV)
		{
			return FVector2f(
				(static_cast<float>(OldUV.X) * static_cast<float>(OldWidth) - static_cast<float>(CropMinX)) / static_cast<float>(NewWidth),
				(static_cast<float>(OldUV.Y) * static_cast<float>(OldHeight) - static_cast<float>(CropMinY)) / static_cast<float>(NewHeight));
		};
		for (const FVertexInstanceID VertexInstanceID : MeshDescription.VertexInstances().GetElementIDs())
		{
			VertexInstanceUVs.Set(VertexInstanceID, 0, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 0)));
			VertexInstanceUVs.Set(VertexInstanceID, 1, RemapAtlasUV(VertexInstanceUVs.Get(VertexInstanceID, 1)));
		}

		for (UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			PlaneInfo.AtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PlaneInfo.BackAtlasPixelMin -= FIntPoint(CropMinX, CropMinY);
			}
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PlaneInfo.AtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.AtlasUVs[CornerIndex]);
				PlaneInfo.BackAtlasUVs[CornerIndex] = RemapAtlasUV(PlaneInfo.BackAtlasUVs[CornerIndex]);
			}
		}

		AtlasPixels = MoveTemp(CroppedAtlasPixels);
		NormalAtlasPixels = MoveTemp(CroppedNormalAtlasPixels);
		MixAtlasPixels = MoveTemp(CroppedMixAtlasPixels);
		SourceTriangleIdAndDepthPixels =
			MoveTemp(CroppedSourceTriangleIdAndDepthPixels);
		AtlasStats.Width = NewWidth;
		AtlasStats.Height = NewHeight;

		int64 PackedPaddedTilePixels = 0;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const int32 Padding = FMath::Max(0, PlaneInfo.AtlasTilePaddingPixels);
			PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.AtlasTileSize.X + Padding * 2)
				* static_cast<int64>(PlaneInfo.AtlasTileSize.Y + Padding * 2);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackedPaddedTilePixels += static_cast<int64>(PlaneInfo.BackAtlasTileSize.X + Padding * 2)
					* static_cast<int64>(PlaneInfo.BackAtlasTileSize.Y + Padding * 2);
			}
		}
		const int64 NewAtlasPixelCount = static_cast<int64>(NewWidth) * static_cast<int64>(NewHeight);
		AtlasStats.PackedTileUtilizationPercent = NewAtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(NewAtlasPixelCount)
			: 0.0;
		return true;
	}

	UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest
	MakeCardAtlasTextureAssetRequest(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const FString& AssetNameSuffix)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request;
		Request.OutputFolderName = EditorSettings.TextureOutputFolderName;
		Request.OutputPackagePathOverride = OutputPackagePathOverride;
		Request.AssetNamePrefix = EditorSettings.TextureNamePrefix;
		Request.AssetNameSuffix = AssetNameSuffix;
		Request.CompressionSettings = TC_BC7;
		return Request;
	}

	UTexture2D* CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.BaseColorOpacityTextureSuffix);
		Request.LODGroup = TEXTUREGROUP_World;
		Request.bSRGB = true;
		Request.SemanticMaskMipCoverageThreshold =
			EditorSettings.bPreserveAlphaMaskValues
				? EditorSettings.MipMaskCoverageThreshold
				: 0.0f;
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
			PlaneInfos,
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.NormalDepthTextureSuffix);
		Request.MipBackgroundColor = FColor(128, 128, 255, 0);
		Request.LODGroup = TEXTUREGROUP_WorldNormalMap;
		Request.bSRGB = false;
		Request.EmptyPixelsError = TEXT("No normal atlas pixels were generated.");
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
			PlaneInfos,
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.MixTextureSuffix);
		Request.MipBackgroundColor = FColor(255, 128, 0, 0);
		Request.LODGroup = TEXTUREGROUP_WorldSpecular;
		Request.bSRGB = false;
		Request.EmptyPixelsError = TEXT("No mix atlas pixels were generated.");
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			Pixels,
			AtlasStats,
			PlaneInfos,
			OutError);
	}

	bool ResizeTileIsolatedAtlas(
		const TArray<FColor>& SourcePixels,
		const FAtlasBakeStats& SourceStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& SourcePlaneInfos,
		const int32 RequestedMaximumDimension,
		const FColor BackgroundColor,
		TArray<FColor>& OutPixels,
		FAtlasBakeStats& OutStats,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& OutPlaneInfos,
		FString& OutError)
	{
		int32 OutputWidth = 0;
		int32 OutputHeight = 0;
		if (!UE::FoliageBaker::Atlas::ResizeTileIsolated(
				SourcePixels,
				SourceStats.Width,
				SourceStats.Height,
				SourcePlaneInfos,
				FMath::Clamp(RequestedMaximumDimension, 64, 1024),
				BackgroundColor,
				OutPixels,
				OutputWidth,
				OutputHeight,
				OutPlaneInfos,
				OutError))
		{
			return false;
		}

		OutStats = SourceStats;
		OutStats.Width = OutputWidth;
		OutStats.Height = OutputHeight;
		OutStats.TileResolution = FMath::Max(
			1,
			FMath::RoundToInt(
				SourceStats.TileResolution
					* FMath::Min(
						static_cast<double>(OutputWidth) / SourceStats.Width,
						static_cast<double>(OutputHeight) / SourceStats.Height)));
		int64 PackedTilePixels = 0;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo
			: OutPlaneInfos)
		{
			PackedTilePixels +=
				static_cast<int64>(PlaneInfo.AtlasTileSize.X)
				* PlaneInfo.AtlasTileSize.Y;
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				PackedTilePixels +=
					static_cast<int64>(PlaneInfo.BackAtlasTileSize.X)
					* PlaneInfo.BackAtlasTileSize.Y;
			}
		}
		const int64 TargetPixelCount =
			static_cast<int64>(OutputWidth) * OutputHeight;
		OutStats.PackedTileUtilizationPercent = TargetPixelCount > 0
			? 100.0 * static_cast<double>(PackedTilePixels)
				/ static_cast<double>(TargetPixelCount)
			: 0.0;
		return true;
	}

	UTexture2D* CreateUpperHemisphereL1VisibilityTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		TArray<FColor> ResizedPixels;
		FAtlasBakeStats ResizedStats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> ResizedPlaneInfos;
		if (!ResizeTileIsolatedAtlas(
				Pixels,
				AtlasStats,
				PlaneInfos,
				EditorSettings.UpperHemisphereL1TextureResolution,
				FColor(128, 128, 128, 255),
				ResizedPixels,
				ResizedStats,
				ResizedPlaneInfos,
				OutError))
		{
			return nullptr;
		}

		UE::FoliageBaker::ProjectedAtlasBake::FTextureAssetRequest Request =
			MakeCardAtlasTextureAssetRequest(
				EditorSettings,
				OutputPackagePathOverride,
				EditorSettings.UpperHemisphereL1VisibilityTextureSuffix);
		Request.MipBackgroundColor = FColor(128, 128, 128, 255);
		Request.LODGroup = TEXTUREGROUP_WorldSpecular;
		Request.bSRGB = false;
		Request.EmptyPixelsError =
			TEXT("No upper-hemisphere L1 visibility pixels were generated.");
		return UE::FoliageBaker::ProjectedAtlasBake::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Request,
			ResizedPixels,
			ResizedStats,
			ResizedPlaneInfos,
			OutError);
	}

	const TCHAR* GetMeshOutputModeText(const EFoliageBakerMeshAssetOutputMode OutputMode)
	{
		switch (OutputMode)
		{
		case EFoliageBakerMeshAssetOutputMode::AddToSourceMeshLOD:
			return TEXT("added to source mesh LODs");
		case EFoliageBakerMeshAssetOutputMode::InsertIntoSourceMeshLOD:
			return TEXT("inserted source mesh LOD");
		case EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD:
			return TEXT("replaced source mesh LOD");
		case EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset:
		default:
			return TEXT("separate mesh asset");
		}
	}

	struct FProxyPlaneCoverBuildData
	{
		int32 SourceLODIndex = INDEX_NONE;
		FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> RetainedTrunkTriangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		FTrunkLeafClassification TrunkLeafClassification;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
		TArray<int32> MultiBillboardPlaneClusterIndices;
		TArray<FVector> MultiBillboardClusterCenters;
	};

	struct FProxyMeshBuildData
	{
		FMeshDescription MeshDescription;
		UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats Stats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> PlaneInfos;
		TArray<FFoliageBakerMeshMaterialSlot> AdditionalMaterialSlots;
		int32 OriginalTrunkTriangleCount = 0;
		int32 ReducedTrunkTriangleCount = 0;
		int32 RetainedTrunkUVChannelCount = 0;
	};

	struct FProxyTextureBuildData
	{
		FAtlasOutputSelection OutputSelection;
		TArray<FColor> AtlasPixels;
		TArray<FColor> NormalAtlasPixels;
		TArray<FColor> MixAtlasPixels;
		TArray<FColor> SourceTriangleIdAndDepthPixels;
		TArray<FColor> UpperHemisphereL1VisibilityPixels;
		FAtlasBakeStats AtlasStats;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UTexture2D* UpperHemisphereL1VisibilityTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	bool ResolveMultiBillboardPlaneGroups(
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		TArray<int32>& OutPlaneGroupIndices,
		FString& OutError)
	{
		OutPlaneGroupIndices.Reset();
		OutPlaneGroupIndices.Reserve(MeshData.PlaneInfos.Num());
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo :
			MeshData.PlaneInfos)
		{
			if (!CoverData.MultiBillboardPlaneClusterIndices.IsValidIndex(
				PlaneInfo.SourcePlaneIndex))
			{
				OutError = FString::Printf(
					TEXT("MultiBillboard plane %d has no valid cluster mapping."),
					PlaneInfo.SourcePlaneIndex);
				return false;
			}
			const int32 ClusterIndex =
				CoverData.MultiBillboardPlaneClusterIndices[PlaneInfo.SourcePlaneIndex];
			if (!CoverData.MultiBillboardClusterCenters.IsValidIndex(ClusterIndex))
			{
				OutError = FString::Printf(
					TEXT("MultiBillboard plane %d references invalid cluster %d."),
					PlaneInfo.SourcePlaneIndex,
					ClusterIndex);
				return false;
			}
			OutPlaneGroupIndices.Add(ClusterIndex);
		}
		return true;
	}

	struct FProxyAssetBuildResult
	{
		bool bSucceeded = false;
		bool bCancelled = false;
		FString Report;
		UStaticMesh* ProxyMesh = nullptr;
		EFoliageBakerMeshAssetOutputMode MeshOutputMode = EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset;
		int32 SourceMeshLODIndex = INDEX_NONE;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UTexture2D* UpperHemisphereL1VisibilityTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *StaticMesh.GetName(), *Error);
		UE_LOG(LogFoliageBakerCards, Warning, TEXT("%s"), *Result.Report);
		return Result;
	}

	FProxyAssetBuildResult MakeProxyBuildCancelled(const UStaticMesh& StaticMesh)
	{
		FProxyAssetBuildResult Result;
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled after bake: no mesh output was selected and no generated assets were committed."),
			*StaticMesh.GetName());
		return Result;
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const FFoliageBakerCardBakeRequest& Settings)
	{
		FAtlasOutputSelection Selection;
		Selection.bBaseColorOpacity = Settings.bBakeBaseColorOpacity;
		Selection.bNormalMask = Settings.bBakeNormalDepth;
		Selection.bMix = Settings.bBakeMix;
		Selection.bMaterialScalarAverages = !Settings.bBakeMix;
		return Selection;
	}

	FVector ResolveSingleCaptureNormal(const EFoliageBakerCaptureAxis Axis)
	{
		switch (Axis)
		{
		case EFoliageBakerCaptureAxis::NegativeX: return FVector(-1.0, 0.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveY: return FVector(0.0, 1.0, 0.0);
		case EFoliageBakerCaptureAxis::NegativeY: return FVector(0.0, -1.0, 0.0);
		case EFoliageBakerCaptureAxis::PositiveX:
		default: return FVector(1.0, 0.0, 0.0);
		}
	}

	FVector RotateHorizontalNormal90Degrees(const FVector& Normal)
	{
		return FVector(-Normal.Y, Normal.X, 0.0).GetSafeNormal();
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const FFoliageBakerCardBakeRequest& Request,
		const FFoliageBakerMeshOutputSelection& OutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = OutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = OutputSelection.ReplaceLODIndex;
		Params.RequestedInsertAfterLODIndex = OutputSelection.InsertAfterLODIndex;
		Params.SourceLODIndex = Request.SourceLODIndex;
		Params.DesiredUVChannelCount = GetDesiredCardUVChannelCount(Request);
		Params.RebuildLODMetadataKey = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? FName(TEXT("FoliageBaker.CrossCardsLOD"))
			: UsesMultiBillboard(Request)
				? FName(TEXT("FoliageBaker.MultiBillboardLOD"))
				: UsesDoublePlanesBillboard(Request)
					? FName(TEXT("FoliageBaker.DoublePlanesBillboardLOD"))
					: FName(TEXT("FoliageBaker.SingleBillboardLOD"));
		return Params;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		OutData.SourceLODIndex = EditorSettings.SourceLODIndex;
		if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			OutData.SourceLODIndex,
			OutData.Triangles,
			OutError))
		{
			return false;
		}

		if (OutData.Triangles.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Source LOD %d contains no bakeable triangles."), OutData.SourceLODIndex);
			return false;
		}
		if (!ComputeSourceTriangleBounds(OutData.Triangles, OutData.SourceLODBounds))
		{
			OutError = FString::Printf(TEXT("Source LOD %d has no valid bounds."), OutData.SourceLODIndex);
			return false;
		}

		if (UsesMultiBillboard(EditorSettings))
		{
			const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult LeafMaterialMatches =
				UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(
					StaticMesh,
					EditorSettings.LeafMaterialKeywords);
			OutData.TrunkLeafClassification.MatchedMaterialCount =
				LeafMaterialMatches.MatchedMaterialCount;
			if (!LeafMaterialMatches.bEnabled)
			{
				OutError = TEXT("MultiBillboard requires at least one Leaf Material Keyword.");
				return false;
			}
			if (LeafMaterialMatches.MatchedMaterialCount == 0)
			{
				OutError = TEXT("No source material instance or parent material name matched the configured Leaf Material Keywords.");
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> LeafTriangles;
			LeafTriangles.Reserve(OutData.Triangles.Num());
			if (EditorSettings.bIncludeReducedTrunk)
			{
				OutData.RetainedTrunkTriangles.Reserve(OutData.Triangles.Num());
			}
			for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : OutData.Triangles)
			{
				if (LeafMaterialMatches.IsMatch(Triangle.MaterialIndex))
				{
					LeafTriangles.Add(Triangle);
				}
				else
				{
					++OutData.TrunkLeafClassification.TrunkTriangleCount;
					if (EditorSettings.bIncludeReducedTrunk)
					{
						OutData.RetainedTrunkTriangles.Add(Triangle);
					}
				}
			}
			OutData.Triangles = MoveTemp(LeafTriangles);
			if (OutData.Triangles.IsEmpty())
			{
				OutError = TEXT("The matched leaf materials contain no bakeable triangles in the selected Source LOD.");
				return false;
			}
			OutData.TrunkLeafClassification.LeafTriangleCount = OutData.Triangles.Num();
		}
		else
		{
			OutData.TrunkLeafClassification = ClassifyTrianglesForTrunkLeafMask(
				StaticMesh,
				OutData.Triangles,
				EditorSettings.TrunkMaterialKeywords);
		}

		OutData.Settings = BuildSettingsForMesh(OutData.Triangles, EditorSettings);
		OutData.ProxyResult.SourceTriangleCount = OutData.Triangles.Num();
		OutData.ProxyResult.CoveredTriangleCount = OutData.Triangles.Num();
		TArray<int32> AllTriangleIndices;
		AllTriangleIndices.Reserve(OutData.Triangles.Num());
		double SourceArea = 0.0;
		for (int32 TriangleIndex = 0; TriangleIndex < OutData.Triangles.Num(); ++TriangleIndex)
		{
			AllTriangleIndices.Add(TriangleIndex);
			SourceArea += OutData.Triangles[TriangleIndex].Area;
		}
		OutData.ProxyResult.SourceArea = SourceArea;
		OutData.ProxyResult.CoveredArea = SourceArea;

		const FVector PrimaryCaptureNormal =
			ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis);
		if (UsesMultiBillboard(EditorSettings))
		{
			const double PositionTolerance = FMath::Max(
				0.001,
				static_cast<double>(OutData.SourceLODBounds.SphereRadius) * 1.0e-6);
			const TArray<FMultiBillboardComponent> Components =
				BuildConnectedLeafComponents(OutData.Triangles, PositionTolerance);
			OutData.TrunkLeafClassification.LeafComponentCount = Components.Num();
			const TArray<FMultiBillboardCluster> Clusters =
				ClusterLeafComponents(Components, EditorSettings.MultiBillboardClusterCount);
			if (Clusters.IsEmpty())
			{
				OutError = TEXT("MultiBillboard could not build any connected leaf clusters.");
				return false;
			}

			FVector AxisU = FVector::CrossProduct(FVector::UpVector, PrimaryCaptureNormal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}
			for (const FMultiBillboardCluster& Cluster : Clusters)
			{
				const TArray<FMultiBillboardLayer> Layers =
					BuildMultiBillboardClusterLayers(
						Components,
						Cluster,
						PrimaryCaptureNormal,
						EditorSettings.MultiBillboardsPerCluster);
				if (Layers.IsEmpty())
				{
					continue;
				}
				const int32 GeneratedClusterIndex =
					OutData.MultiBillboardClusterCenters.Add(Cluster.Center);
				for (const FMultiBillboardLayer& Layer : Layers)
				{
					UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane =
						OutData.ProxyResult.Planes.AddDefaulted_GetRef();
					Plane.Normal = PrimaryCaptureNormal;
					Plane.Rho = Layer.Rho;
					Plane.Score = Layer.Area;
					Plane.CoveredArea = Layer.Area;
					Plane.TriangleIndices = Layer.TriangleIndices;
					Plane.bIsTrunkCard = false;
					Plane.bUseFixedPlaneFrame = true;
					Plane.FixedAxisU = AxisU;
					Plane.FixedAxisV = FVector::UpVector;
					OutData.MultiBillboardPlaneClusterIndices.Add(GeneratedClusterIndex);
				}
			}
			OutData.TrunkLeafClassification.GeneratedClusterCount =
				OutData.MultiBillboardClusterCenters.Num();
			OutData.TrunkLeafClassification.GeneratedBillboardCount =
				OutData.ProxyResult.Planes.Num();
			if (OutData.ProxyResult.Planes.IsEmpty())
			{
				OutError = TEXT("MultiBillboard could not generate any non-empty depth layers.");
				return false;
			}
			return true;
		}

		const int32 PlaneCount = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
			? (UsesDoublePlanesBillboard(EditorSettings) ? 2 : 1)
			: FMath::Clamp(EditorSettings.CrossCardPlaneCount, 2, 5);
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneCount; ++PlaneIndex)
		{
			const FVector Normal = EditorSettings.Mode == EFoliageBakerCardBakeMode::SingleBillboard
				? PlaneIndex == 0
					? PrimaryCaptureNormal
					: RotateHorizontalNormal90Degrees(PrimaryCaptureNormal)
				: FVector(
					FMath::Cos(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
					FMath::Sin(static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(PlaneCount)),
					0.0);
			FVector AxisU = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane = OutData.ProxyResult.Planes.AddDefaulted_GetRef();
			Plane.Normal = Normal;
			Plane.Rho = 0.0;
			Plane.Score = SourceArea;
			Plane.CoveredArea = SourceArea;
			Plane.TriangleIndices = AllTriangleIndices;
			Plane.bIsTrunkCard = true;
			Plane.bUseFixedPlaneFrame = true;
			Plane.FixedAxisU = AxisU;
			Plane.FixedAxisV = FVector::UpVector;
		}
		return true;
	}

	bool BuildProxyMeshData(
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& OutData,
		FString& OutError)
	{
		return UE::FoliageBaker::PlaneCover::BuildPlaneProxyMeshDescription(
			CoverData.Triangles,
			CoverData.ProxyResult,
			CoverData.Settings,
			OutData.MeshDescription,
			OutData.Stats,
			OutError,
			&OutData.PlaneInfos);
	}

	FName MakeUniqueRetainedMaterialSlotName(
		UMaterialInterface* Material,
		const int32 SourceMaterialIndex,
		TSet<FName>& InOutUsedSlotNames)
	{
		const FString BaseSlotName = Material && !Material->GetName().IsEmpty()
			? Material->GetName()
			: FString::Printf(TEXT("RetainedMaterial_%d"), SourceMaterialIndex);
		FName Candidate(*BaseSlotName);
		for (int32 Suffix = 1; InOutUsedSlotNames.Contains(Candidate); ++Suffix)
		{
			Candidate = FName(*FString::Printf(TEXT("%s_%d"), *BaseSlotName, Suffix));
		}
		InOutUsedSlotNames.Add(Candidate);
		return Candidate;
	}

	bool BuildRetainedTrunkMeshDescription(
		const UStaticMesh& StaticMesh,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& TrunkTriangles,
		UMaterialInterface* ProxyMaterial,
		FMeshDescription& OutMeshDescription,
		TArray<FFoliageBakerMeshMaterialSlot>& OutMaterialSlots,
		FString& OutError)
	{
		OutMeshDescription.Empty();
		OutMaterialSlots.Reset();
		if (TrunkTriangles.IsEmpty())
		{
			return true;
		}

		FStaticMeshAttributes Attributes(OutMeshDescription);
		Attributes.Register();
		Attributes.RegisterTriangleNormalAndTangentAttributes();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		int32 UVChannelCount = 1;
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : TrunkTriangles)
		{
			UVChannelCount = FMath::Max(UVChannelCount, Triangle.NumUVChannels);
		}
		UVChannelCount = FMath::Clamp(
			UVChannelCount,
			1,
			UE::FoliageBaker::PlaneCover::MaxSourceMeshUVChannels);
		VertexInstanceUVs.SetNumChannels(UVChannelCount);

		OutMeshDescription.ReserveNewVertices(TrunkTriangles.Num() * 3);
		OutMeshDescription.ReserveNewVertexInstances(TrunkTriangles.Num() * 3);
		OutMeshDescription.ReserveNewTriangles(TrunkTriangles.Num());

		const TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
		TMap<int32, FPolygonGroupID> PolygonGroupByMaterialIndex;
		TMap<FVector3f, FVertexID> VertexByPosition;
		TSet<FName> UsedMaterialSlotNames;
		UsedMaterialSlotNames.Add(TEXT("BillboardProxy"));
		if (ProxyMaterial && !ProxyMaterial->GetFName().IsNone())
		{
			UsedMaterialSlotNames.Add(ProxyMaterial->GetFName());
		}

		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : TrunkTriangles)
		{
			FPolygonGroupID PolygonGroupID = INDEX_NONE;
			if (const FPolygonGroupID* ExistingGroup = PolygonGroupByMaterialIndex.Find(Triangle.MaterialIndex))
			{
				PolygonGroupID = *ExistingGroup;
			}
			else
			{
				UMaterialInterface* Material = StaticMaterials.IsValidIndex(Triangle.MaterialIndex)
					? StaticMaterials[Triangle.MaterialIndex].MaterialInterface
					: nullptr;
				if (!Material)
				{
					Material = UMaterial::GetDefaultMaterial(MD_Surface);
				}
				const FName MaterialSlotName = MakeUniqueRetainedMaterialSlotName(
					Material,
					Triangle.MaterialIndex,
					UsedMaterialSlotNames);
				PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
				MaterialSlotNames[PolygonGroupID] = MaterialSlotName;
				PolygonGroupByMaterialIndex.Add(Triangle.MaterialIndex, PolygonGroupID);
				FFoliageBakerMeshMaterialSlot& MaterialSlot = OutMaterialSlots.AddDefaulted_GetRef();
				MaterialSlot.MaterialSlotName = MaterialSlotName;
				MaterialSlot.Material = Material;
			}

			TArray<FVertexInstanceID, TInlineAllocator<3>> VertexInstanceIDs;
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const FVector3f Position(Triangle.Vertices[CornerIndex]);
				FVertexID VertexID = INDEX_NONE;
				if (const FVertexID* ExistingVertexID = VertexByPosition.Find(Position))
				{
					VertexID = *ExistingVertexID;
				}
				else
				{
					VertexID = OutMeshDescription.CreateVertex();
					VertexPositions[VertexID] = Position;
					VertexByPosition.Add(Position, VertexID);
				}

				const FVertexInstanceID VertexInstanceID =
					OutMeshDescription.CreateVertexInstance(VertexID);
				VertexInstanceNormals[VertexInstanceID] = FVector3f(Triangle.VertexNormals[CornerIndex]);
				VertexInstanceTangents[VertexInstanceID] = FVector3f(Triangle.VertexTangents[CornerIndex]);
				VertexInstanceBinormalSigns[VertexInstanceID] = Triangle.BinormalSigns[CornerIndex];
				VertexInstanceColors[VertexInstanceID] = Triangle.VertexColors[CornerIndex];
				for (int32 UVChannel = 0; UVChannel < UVChannelCount; ++UVChannel)
				{
					VertexInstanceUVs.Set(
						VertexInstanceID,
						UVChannel,
						Triangle.UVChannels[UVChannel][CornerIndex]);
				}
				VertexInstanceIDs.Add(VertexInstanceID);
			}

			OutMeshDescription.CreateTriangle(PolygonGroupID, VertexInstanceIDs);
		}

		if (OutMeshDescription.Triangles().Num() == 0)
		{
			OutError = TEXT("MultiBillboard retained trunk geometry contains no valid triangles.");
			return false;
		}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutMeshDescription);
		FStaticMeshOperations::DetermineEdgeHardnessesFromVertexInstanceNormals(OutMeshDescription);
		return true;
	}

	bool AppendReducedMultiBillboardTrunk(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		UMaterialInterface* ProxyMaterial,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesMultiBillboard(EditorSettings)
			|| !EditorSettings.bIncludeReducedTrunk
			|| CoverData.RetainedTrunkTriangles.IsEmpty())
		{
			return true;
		}

		FMeshDescription TrunkMeshDescription;
		TArray<FFoliageBakerMeshMaterialSlot> TrunkMaterialSlots;
		if (!BuildRetainedTrunkMeshDescription(
			StaticMesh,
			CoverData.RetainedTrunkTriangles,
			ProxyMaterial,
			TrunkMeshDescription,
			TrunkMaterialSlots,
			OutError))
		{
			return false;
		}

		MeshData.OriginalTrunkTriangleCount = TrunkMeshDescription.Triangles().Num();
		FMeshDescription ReducedTrunkMeshDescription(TrunkMeshDescription);
		const float TrianglePercentage = FMath::Clamp(
			EditorSettings.TrunkTrianglePercentage,
			0.05f,
			1.0f);
		if (TrianglePercentage < 1.0f && TrunkMeshDescription.Triangles().Num() > 2)
		{
			IMeshReductionManagerModule* MeshReductionModule =
				FModuleManager::Get().LoadModulePtr<IMeshReductionManagerModule>("MeshReductionInterface");
			IMeshReduction* MeshReduction = MeshReductionModule
				? MeshReductionModule->GetStaticMeshReductionInterface()
				: nullptr;
			if (!MeshReduction || !MeshReduction->IsSupported())
			{
				OutError = TEXT("MultiBillboard could not load a supported Unreal Static Mesh reduction interface for the retained trunk.");
				return false;
			}

			FOverlappingCorners OverlappingCorners;
			FStaticMeshOperations::FindOverlappingCorners(
				OverlappingCorners,
				TrunkMeshDescription,
				1.0e-5f);
			FMeshReductionSettings ReductionSettings;
			ReductionSettings.TerminationCriterion =
				EStaticMeshReductionTerimationCriterion::Triangles;
			ReductionSettings.PercentTriangles = TrianglePercentage;
			ReductionSettings.PercentVertices = 1.0f;
			ReductionSettings.SilhouetteImportance = EMeshFeatureImportance::High;
			ReductionSettings.TextureImportance = EMeshFeatureImportance::High;
			ReductionSettings.ShadingImportance = EMeshFeatureImportance::High;
			ReductionSettings.VertexColorImportance = EMeshFeatureImportance::High;
			float MaxDeviation = 0.0f;
			MeshReduction->ReduceMeshDescription(
				ReducedTrunkMeshDescription,
				MaxDeviation,
				TrunkMeshDescription,
				OverlappingCorners,
				ReductionSettings);
		}

		if (ReducedTrunkMeshDescription.Triangles().Num() == 0)
		{
			OutError = TEXT("MultiBillboard trunk reduction produced an empty mesh.");
			return false;
		}
		MeshData.ReducedTrunkTriangleCount = ReducedTrunkMeshDescription.Triangles().Num();
		MeshData.RetainedTrunkUVChannelCount =
			FStaticMeshConstAttributes(ReducedTrunkMeshDescription)
				.GetVertexInstanceUVs()
				.GetNumChannels();

		TSet<FName> RetainedSlotNames;
		const TPolygonGroupAttributesConstRef<FName> ReducedMaterialSlotNames =
			FStaticMeshConstAttributes(ReducedTrunkMeshDescription).GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID PolygonGroupID :
			ReducedTrunkMeshDescription.PolygonGroups().GetElementIDs())
		{
			RetainedSlotNames.Add(ReducedMaterialSlotNames[PolygonGroupID]);
		}
		TrunkMaterialSlots.RemoveAll(
			[&RetainedSlotNames](const FFoliageBakerMeshMaterialSlot& MaterialSlot)
			{
				return !RetainedSlotNames.Contains(MaterialSlot.MaterialSlotName);
			});

		FStaticMeshOperations::FAppendSettings AppendSettings;
		FStaticMeshOperations::AppendMeshDescription(
			ReducedTrunkMeshDescription,
			MeshData.MeshDescription,
			AppendSettings);
		if (MeshData.MeshDescription.NeedsCompact())
		{
			FElementIDRemappings Remappings;
			MeshData.MeshDescription.Compact(Remappings);
		}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MeshData.MeshDescription);
		MeshData.AdditionalMaterialSlots = MoveTemp(TrunkMaterialSlots);
		return true;
	}

	bool BuildDoublePlanesOutputMesh(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& PlaneSettings,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesDoublePlanesBillboard(EditorSettings))
		{
			return true;
		}
		if (MeshData.PlaneInfos.Num() != 2)
		{
			OutError = FString::Printf(
				TEXT("Double Planes Billboard requires exactly two captured planes, but %d were generated."),
				MeshData.PlaneInfos.Num());
			return false;
		}

		const FVector OutputNormal =
			ResolveSingleCaptureNormal(EditorSettings.SingleCaptureAxis).GetSafeNormal();
		FVector OutputAxisU = FVector::CrossProduct(FVector::UpVector, OutputNormal).GetSafeNormal();
		if (OutputAxisU.IsNearlyZero())
		{
			OutError = TEXT("Double Planes Billboard could not construct a horizontal output plane frame.");
			return false;
		}

		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> OutputPlaneInfos =
			MeshData.PlaneInfos;
		for (int32 PlaneIndex = 0; PlaneIndex < OutputPlaneInfos.Num(); ++PlaneIndex)
		{
			UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& OutputPlane =
				OutputPlaneInfos[PlaneIndex];
			const FVector CaptureNormal = OutputPlane.Normal.GetSafeNormal();
			if (CaptureNormal.IsNearlyZero())
			{
				OutError = FString::Printf(
					TEXT("Double Planes Billboard capture plane %d has an invalid direction."),
					PlaneIndex);
				return false;
			}

			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				OutputPlane.BackAtlasUVs[CornerIndex] =
					FVector2f(static_cast<float>(CaptureNormal.X), static_cast<float>(CaptureNormal.Y));
			}
			OutputPlane.bUseCustomAuxiliaryUV = true;
			OutputPlane.AuxiliaryUV = FVector2f(static_cast<float>(PlaneIndex), 0.0f);

			OutputPlane.Normal = OutputNormal;
			OutputPlane.Rho = 0.0;
			OutputPlane.AxisU = OutputAxisU;
			OutputPlane.AxisV = FVector::UpVector;
			OutputPlane.ShadingNormal = OutputNormal;
			const FVector PlaneOrigin = OutputPlane.Normal * OutputPlane.Rho;
			OutputPlane.Corners[0] =
				PlaneOrigin + OutputPlane.AxisU * OutputPlane.MinU + OutputPlane.AxisV * OutputPlane.MinV;
			OutputPlane.Corners[1] =
				PlaneOrigin + OutputPlane.AxisU * OutputPlane.MaxU + OutputPlane.AxisV * OutputPlane.MinV;
			OutputPlane.Corners[2] =
				PlaneOrigin + OutputPlane.AxisU * OutputPlane.MaxU + OutputPlane.AxisV * OutputPlane.MaxV;
			OutputPlane.Corners[3] =
				PlaneOrigin + OutputPlane.AxisU * OutputPlane.MinU + OutputPlane.AxisV * OutputPlane.MaxV;
		}

		if (!UE::FoliageBaker::PlaneCover::RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			OutputPlaneInfos,
			PlaneSettings,
			MeshData.MeshDescription,
			MeshData.Stats,
			OutError))
		{
			return false;
		}
		MeshData.Stats.AveragePlaneToShadingNormalDot = 1.0;
		MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees = 0.0;
		return true;
	}

	bool BuildMultiBillboardOutputMesh(
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		const TArray<int32>& PlaneGroupIndices,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& PlaneSettings,
		FProxyMeshBuildData& MeshData,
		FString& OutError)
	{
		if (!UsesMultiBillboard(EditorSettings))
		{
			return true;
		}
		if (MeshData.PlaneInfos.IsEmpty())
		{
			OutError = TEXT("MultiBillboard has no generated planes to prepare for runtime rotation.");
			return false;
		}
		if (PlaneGroupIndices.Num() != MeshData.PlaneInfos.Num())
		{
			OutError = TEXT("MultiBillboard cluster mapping does not match generated plane count.");
			return false;
		}

		for (int32 PlaneIndex = 0; PlaneIndex < MeshData.PlaneInfos.Num(); ++PlaneIndex)
		{
			UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo =
				MeshData.PlaneInfos[PlaneIndex];
			const FVector AxisU = PlaneInfo.AxisU.GetSafeNormal();
			const FVector AxisV = PlaneInfo.AxisV.GetSafeNormal();
			const FVector Normal = PlaneInfo.Normal.GetSafeNormal();
			const int32 ClusterIndex = PlaneGroupIndices[PlaneIndex];
			if (AxisU.IsNearlyZero()
				|| AxisV.IsNearlyZero()
				|| Normal.IsNearlyZero()
				|| !CoverData.MultiBillboardClusterCenters.IsValidIndex(ClusterIndex))
			{
				OutError = TEXT("MultiBillboard generated a plane with an invalid local frame.");
				return false;
			}
			const FVector ClusterCenter =
				CoverData.MultiBillboardClusterCenters[ClusterIndex];
			const FVector PlaneCenter =
				(PlaneInfo.Corners[0]
					+ PlaneInfo.Corners[1]
					+ PlaneInfo.Corners[2]
					+ PlaneInfo.Corners[3]) * 0.25;
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				const FVector CenterRelativePosition =
					PlaneInfo.Corners[CornerIndex] - ClusterCenter;
				PlaneInfo.BackAtlasUVs[CornerIndex] = FVector2f(
					static_cast<float>(FVector::DotProduct(CenterRelativePosition, AxisU)),
					static_cast<float>(FVector::DotProduct(CenterRelativePosition, AxisV)));
			}
			PlaneInfo.bUseCustomAuxiliaryUV = true;
			PlaneInfo.AuxiliaryUV = FVector2f(
				static_cast<float>(
					FVector::DotProduct(PlaneCenter - ClusterCenter, Normal)),
				0.0f);
		}

		if (!UE::FoliageBaker::PlaneCover::RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			MeshData.PlaneInfos,
			PlaneSettings,
			MeshData.MeshDescription,
			MeshData.Stats,
			OutError))
		{
			return false;
		}
		return true;
	}

	bool BuildProxyTextureData(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		const FFoliageBakerGeneratedAssetOutputFolders& OutputFolders,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput()
			&& !EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			OutError = TEXT("No atlas outputs selected. Enable BaseColor/Opacity, Normal/TrunkLeafMask, Mix, or Upper Hemisphere L1 Visibility.");
			return false;
		}
		UMaterialInstanceConstant* TemplateMaterialInstance = EditorSettings.MaterialTemplate;
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("A parent Material Instance Constant must be selected in the current tool settings.");
			return false;
		}
		TArray<int32> MultiBillboardPlaneGroupIndices;
		if (UsesMultiBillboard(EditorSettings))
		{
			if (!ResolveMultiBillboardPlaneGroups(
				CoverData,
				MeshData,
				MultiBillboardPlaneGroupIndices,
				OutError))
			{
				return false;
			}
			if (!UE::FoliageBaker::PlaneCover::ApplyGroupedPlaneProxyBoundsAndRebuildMeshDescription(
				MeshData.PlaneInfos,
				MultiBillboardPlaneGroupIndices,
				CoverData.Settings,
				MeshData.MeshDescription,
				MeshData.Stats,
				OutError))
			{
				return false;
			}
		}
		if (UsesDoublePlanesBillboard(EditorSettings)
			&& !UE::FoliageBaker::PlaneCover::ApplySharedPlaneProxyBoundsAndRebuildMeshDescription(
				MeshData.PlaneInfos,
				CoverData.Settings,
				MeshData.MeshDescription,
				MeshData.Stats,
				OutError))
		{
			return false;
		}

		auto BakeFeatureAtlas = [&](const FAtlasOutputSelection& OutputSelection,
			const bool bCaptureSourceDepth,
			TArray<FColor>& AtlasPixels,
			TArray<FColor>& NormalPixels,
			TArray<FColor>& MixPixels,
			TArray<FColor>& SourceTriangleIdAndDepthPixels,
			FAtlasBakeStats& AtlasStats) -> bool
		{
			return BakeCardAtlasOrthographic(
				StaticMesh,
				CoverData.SourceLODBounds,
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				OutputSelection,
				UsesDoublePlanesBillboard(EditorSettings),
				bCaptureSourceDepth,
				AtlasPixels,
				NormalPixels,
				MixPixels,
				SourceTriangleIdAndDepthPixels,
				AtlasStats,
				OutError);
		};

		int32 AlphaAwareCroppedPlaneCount = 0;
		const int32 EffectiveAlphaCropGuardPixels =
			CoverData.Settings.AlphaAwareTileCropGuardPixels;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			constexpr uint8 AlphaCropThreshold = 1;
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;
			CropOutputSelection.bNormalMask = false;
			CropOutputSelection.bMix = false;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			TArray<FColor> CropSourceTriangleIdAndDepthPixels;
			FAtlasBakeStats CropStats;
			if (!BakeFeatureAtlas(
				CropOutputSelection,
				false,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropSourceTriangleIdAndDepthPixels,
				CropStats))
			{
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				EffectiveAlphaCropGuardPixels,
				AlphaCropThreshold,
				TileCrops);
			if (UsesDoublePlanesBillboard(EditorSettings))
			{
				AlphaAwareCroppedPlaneCount = MergeDoublePlaneTileCrops(TileCrops);
			}
			else if (UsesMultiBillboard(EditorSettings))
			{
				AlphaAwareCroppedPlaneCount = MergeGroupedTileCrops(
					TileCrops,
					MultiBillboardPlaneGroupIndices);
			}

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::FoliageBaker::PlaneCover::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					TileCrops,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError,
					UsesDoublePlanesBillboard(EditorSettings)))
				{
					return false;
				}
			}
		}

		if (!BakeFeatureAtlas(
			OutData.OutputSelection,
			EditorSettings.bBakeUpperHemisphereL1Visibility,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.SourceTriangleIdAndDepthPixels,
			OutData.AtlasStats))
		{
			return false;
		}
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? EffectiveAlphaCropGuardPixels
			: 0;
		if (!CropAtlasToUsedSpace(
				MeshData.PlaneInfos,
				MeshData.MeshDescription,
				OutData.AtlasPixels,
				OutData.NormalAtlasPixels,
				OutData.MixAtlasPixels,
				OutData.SourceTriangleIdAndDepthPixels,
				OutData.AtlasStats,
				EditorSettings.bTrimUnusedAtlasSpace
					? EAtlasOuterCropMode::TightBlockAligned
					: EAtlasOuterCropMode::PowerOfTwoUsedBounds,
				OutError))
		{
			return false;
		}
		MeshData.Stats.AtlasWidth = OutData.AtlasStats.Width;
		MeshData.Stats.AtlasHeight = OutData.AtlasStats.Height;
		if (EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			if (!UE::FoliageBaker::L1Visibility::BakeUpperHemisphere(
					StaticMesh,
					CoverData.SourceLODBounds,
					CoverData.Triangles,
					MeshData.PlaneInfos,
					CoverData.Settings,
					OutData.SourceTriangleIdAndDepthPixels,
					OutData.AtlasStats.Width,
					OutData.AtlasStats.Height,
					EditorSettings.UpperHemisphereL1SampleCount,
					EditorSettings.UpperHemisphereL1ShadowMapResolution,
					OutData.UpperHemisphereL1VisibilityPixels,
					OutError))
			{
				return false;
			}
		}
		if (!BuildDoublePlanesOutputMesh(
			EditorSettings,
			CoverData.Settings,
			MeshData,
			OutError))
		{
			return false;
		}
		if (!BuildMultiBillboardOutputMesh(
			EditorSettings,
			CoverData,
			MultiBillboardPlaneGroupIndices,
			CoverData.Settings,
			MeshData,
			OutError))
		{
			return false;
		}

		if (OutData.OutputSelection.bBaseColorOpacity)
		{
			OutData.AtlasTexture = CreateAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.AtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.AtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bNormalMask)
		{
			OutData.NormalAtlasTexture = CreateNormalAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.NormalAtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.NormalAtlasTexture)
			{
				return false;
			}
		}

		if (OutData.OutputSelection.bMix)
		{
			OutData.MixAtlasTexture = CreateMixAtlasTextureAsset(
				StaticMesh,
				AssetTransaction,
				EditorSettings,
				OutputFolders.TexturePackagePath,
				OutData.MixAtlasPixels,
				OutData.AtlasStats,
				MeshData.PlaneInfos,
				OutError);
			if (!OutData.MixAtlasTexture)
			{
				return false;
			}
		}

		FFoliageBakerMaterialInstanceAssetParams MaterialParams;
		MaterialParams.OutputFolderName = EditorSettings.MaterialOutputFolderName;
		MaterialParams.OutputPackagePathOverride = OutputFolders.MaterialPackagePath;
		MaterialParams.AssetNamePrefix = EditorSettings.MaterialInstanceNamePrefix;
		MaterialParams.AssetNameSuffix = EditorSettings.MaterialInstanceNameSuffix;
		MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MaterialParams.BaseColorOpacityTextureParameterName = EditorSettings.BaseColorOpacityTextureParameterName;
		MaterialParams.NormalDepthTextureParameterName = EditorSettings.NormalDepthTextureParameterName;
		MaterialParams.MixTextureParameterName = EditorSettings.MixTextureParameterName;
		if (OutData.OutputSelection.bMaterialScalarAverages)
		{
			const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
				ParameterNames = {
					EditorSettings.LeafRoughnessParameterName,
					EditorSettings.LeafSpecularParameterName,
					EditorSettings.TrunkRoughnessParameterName,
					EditorSettings.TrunkSpecularParameterName,
				};
			if (!UE::FoliageBaker::MaterialResolver::ResolveTrunkLeafMaterialScalarParameters(
					OutData.AtlasStats.MaterialAverages,
					ParameterNames,
					MaterialParams.ScalarParameterValues,
					OutError))
			{
				return false;
			}
		}
		if (EditorSettings.bBakeUpperHemisphereL1Visibility)
		{
			OutData.UpperHemisphereL1VisibilityTexture =
				CreateUpperHemisphereL1VisibilityTextureAsset(
					StaticMesh,
					AssetTransaction,
					EditorSettings,
					OutputFolders.TexturePackagePath,
					OutData.UpperHemisphereL1VisibilityPixels,
					OutData.AtlasStats,
					MeshData.PlaneInfos,
					OutError);
			if (!OutData.UpperHemisphereL1VisibilityTexture)
			{
				return false;
			}
		}
		if (OutData.UpperHemisphereL1VisibilityTexture)
		{
			FFoliageBakerMaterialInstanceAssetParams::FTextureParameterValue&
				L1VisibilityParameter =
					MaterialParams.AdditionalTextureParameterValues.AddDefaulted_GetRef();
			L1VisibilityParameter.ParameterName =
				EditorSettings.UpperHemisphereL1VisibilityTextureParameterName;
			L1VisibilityParameter.Texture =
				OutData.UpperHemisphereL1VisibilityTexture;
		}
		if (EditorSettings.Mode == EFoliageBakerCardBakeMode::CrossCards)
		{
			MaterialParams.TwoSidedOverride = !UsesSeparateOneSidedCrossFaces(EditorSettings);
		}
		else if (UsesMultiBillboard(EditorSettings))
		{
			MaterialParams.TwoSidedOverride = true;
		}
		OutData.Material = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
			StaticMesh,
			AssetTransaction,
			MaterialParams,
			TemplateMaterialInstance,
			OutData.AtlasTexture,
			OutData.NormalAtlasTexture,
			OutData.MixAtlasTexture,
			OutError);
		return OutData.Material != nullptr;
	}

	bool CreateProxyMeshAssetBundle(
		UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = MeshOutputSelection.OutputMode;
		const int32 OutputUVChannelCount = FMath::Clamp(
			FStaticMeshConstAttributes(MeshData.MeshDescription)
				.GetVertexInstanceUVs()
				.GetNumChannels(),
			1,
			UE::FoliageBaker::PlaneCover::MaxSourceMeshUVChannels);

		if (MeshOutputSelection.OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix = EditorSettings.Mode == EFoliageBakerCardBakeMode::CrossCards
				? TEXT("_CrossCards")
				: UsesMultiBillboard(EditorSettings)
					? TEXT("_MultiBillboard")
				: UsesDoublePlanesBillboard(EditorSettings)
					? TEXT("_DoubleBillboard")
					: TEXT("_Billboard");
			MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
			MeshParams.DesiredUVChannelCount = OutputUVChannelCount;
			MeshParams.AdditionalMaterialSlots = MeshData.AdditionalMaterialSlots;
			OutResult.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
				StaticMesh,
				AssetTransaction,
				MeshParams,
				MeshData.MeshDescription,
				TextureData.Material,
				OutError);
			if (!OutResult.ProxyMesh)
			{
				return false;
			}
		}
		else
		{
			int32 InstalledLODIndex = INDEX_NONE;
			FFoliageBakerSourceLODAssetParams LODParams =
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection);
			LODParams.DesiredUVChannelCount = OutputUVChannelCount;
			LODParams.AdditionalMaterialSlots = MeshData.AdditionalMaterialSlots;
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				LODParams,
				MeshData.MeshDescription,
				TextureData.Material,
				InstalledLODIndex,
				OutError))
			{
				return false;
			}

			OutResult.ProxyMesh = &StaticMesh;
			OutResult.SourceMeshLODIndex = InstalledLODIndex;
		}

		OutResult.AtlasTexture = TextureData.AtlasTexture;
		OutResult.NormalAtlasTexture = TextureData.NormalAtlasTexture;
		OutResult.MixAtlasTexture = TextureData.MixAtlasTexture;
		OutResult.UpperHemisphereL1VisibilityTexture =
			TextureData.UpperHemisphereL1VisibilityTexture;
		OutResult.Material = TextureData.Material;
		return true;
	}

	FString BuildProxySuccessReport(
		const UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& Request,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);
		const FString CrossFaceDetails = Request.Mode != EFoliageBakerCardBakeMode::CrossCards
			? FString()
			: FString::Printf(
				TEXT("\n  cross face mode: %s"),
				UsesSeparateOneSidedCrossFaces(Request)
					? TEXT("separate one-sided front/back quads, generated material Two Sided=false")
					: TEXT("one two-sided quad per direction, generated material Two Sided=true"));
		const TCHAR* AtlasUVDetails = UsesDoublePlanesBillboard(Request)
			? TEXT("UV0 stores each plane's baked tile; UV1.xy stores its local capture direction; UV2.x stores plane selector 0 or 1")
			: UsesMultiBillboard(Request)
				? TEXT("UV0 stores each depth layer's baked tile; UV1.xy stores the vertex U/V offset from the shared cluster center and UV2.x stores the signed layer-depth offset in centimeters")
				: Request.Mode == EFoliageBakerCardBakeMode::SingleBillboard
					? TEXT("UV0 stores the single baked tile")
					: UsesSeparateOneSidedCrossFaces(Request)
						? TEXT("each physical face stores its own front/back tile in UV0; generated mesh keeps one UV channel")
						: TEXT("UV0 stores the front-side tile and UV1 stores the back-side tile");
		const TCHAR* WindingDetails = UsesDoublePlanesBillboard(Request)
			? TEXT("two overlapping parallel quads in the primary capture frame; dedicated material controls camera-facing rotation, Dither weights, and spacing")
			: UsesMultiBillboard(Request)
				? TEXT("multiple parallel two-sided quads per spatial leaf cluster; the material rotates every layer stack around its reconstructed shared cluster center")
				: UsesSeparateOneSidedCrossFaces(Request)
					? TEXT("opposed UE front-face orders with opposed source-facing normals")
					: TEXT("reversed UE front-face order with source-facing normals");
		const TCHAR* FeatureName = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? TEXT("Cross Cards")
			: UsesMultiBillboard(Request)
				? TEXT("MultiBillboard")
				: UsesDoublePlanesBillboard(Request)
					? TEXT("Double Planes Billboard")
					: TEXT("Single Plane Billboard");
		const TCHAR* CaptureDetails = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
			? TEXT("equally spaced over 180 degrees, front and back baked")
			: UsesMultiBillboard(Request)
				? TEXT("connected leaf components clustered in source-local 3D space, then split into parallel fixed-axis depth layers inside every cluster")
				: UsesDoublePlanesBillboard(Request)
					? TEXT("primary selected axis plus a second local horizontal axis rotated +90 degrees, one baked side per view")
					: TEXT("one selected axis, one baked side");

		const FString ClassificationDetails = UsesMultiBillboard(Request)
			? FString::Printf(
				TEXT("leaf selection: material/parent keyword rule, matched materials=%d, leaf triangles=%d, non-leaf triangles=%d, connected components=%d, generated clusters=%d, generated billboards=%d"),
				CoverData.TrunkLeafClassification.MatchedMaterialCount,
				CoverData.TrunkLeafClassification.LeafTriangleCount,
				CoverData.TrunkLeafClassification.TrunkTriangleCount,
				CoverData.TrunkLeafClassification.LeafComponentCount,
				CoverData.TrunkLeafClassification.GeneratedClusterCount,
				CoverData.TrunkLeafClassification.GeneratedBillboardCount)
			: FString::Printf(
				TEXT("trunk/leaf classification: shared material/parent keyword rule, matched materials=%d, trunk triangles=%d"),
				CoverData.TrunkLeafClassification.MatchedMaterialCount,
				CoverData.TrunkLeafClassification.TrunkTriangleCount);
		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm\n  feature: %s, capture=%s, selected-LOD projected bounds, per-angle alpha crop\n  %s%s"),
			*StaticMesh.GetName(),
			CoverData.SourceLODIndex,
			CoverData.SourceLODBounds.SphereRadius,
			FeatureName,
			CaptureDetails,
			*ClassificationDetails,
			*CrossFaceDetails);
		const FString BaseAtlasPath = TextureData.AtlasTexture ? TextureData.AtlasTexture->GetPathName() : TEXT("disabled");
		const FString NormalAtlasPath = TextureData.NormalAtlasTexture ? TextureData.NormalAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MixAtlasPath = TextureData.MixAtlasTexture ? TextureData.MixAtlasTexture->GetPathName() : TEXT("disabled");
		const FString MeshOutputDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? FString::Printf(TEXT("%s: %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), *AssetResult.ProxyMesh->GetPathName())
			: FString::Printf(TEXT("%s %d on %s"), GetMeshOutputModeText(AssetResult.MeshOutputMode), AssetResult.SourceMeshLODIndex, *AssetResult.ProxyMesh->GetPathName());
		const TCHAR* MeshBuildPathDetails = AssetResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			? TEXT("BuildFromMeshDescriptions full build path")
			: TEXT("source StaticMesh LOD MeshDescription commit");
		const FString MaterialParameterDetails = FString::Printf(
			TEXT("BaseColor/Opacity=%s, Normal/TrunkLeafMask=%s, Mix=%s"),
			*Request.BaseColorOpacityTextureParameterName.ToString(),
			*Request.NormalDepthTextureParameterName.ToString(),
			*Request.MixTextureParameterName.ToString());
		const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
			MaterialScalarParameterNames = {
				Request.LeafRoughnessParameterName,
				Request.LeafSpecularParameterName,
				Request.TrunkRoughnessParameterName,
				Request.TrunkSpecularParameterName,
			};
		const FString MaterialScalarDetails =
			UE::FoliageBaker::MaterialResolver::BuildTrunkLeafMaterialAveragesReport(
				!Request.bBakeMix,
				TextureData.AtlasStats.MaterialAverages,
				MaterialScalarParameterNames);

		FString Report = FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, rasterized refs=%d, masked refs=%d, shooting=%s, resolve=%s\n  base/color opacity atlas: %s, RGB=BaseColor, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  normal/trunk-leaf atlas: %s, RGB=%s, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission\n  material scalar averages: %s\n  atlas UVs: %s\n  material instance: %s (child of the Editor Preferences parent; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision off, lightmap UV generation off, distance fields on\n  proxy winding: %s"),
			*TechniqueSummary,
			*AlphaPolicyDetails,
			*MeshOutputDetails,
			MeshData.Stats.PlaneCount,
			MeshData.Stats.QuadCount,
			MeshData.Stats.TriangleCount,
			TextureData.AtlasStats.Width,
			TextureData.AtlasStats.Height,
			TextureData.AtlasStats.TileResolution,
			TextureData.AtlasStats.PackedTileUtilizationPercent,
			TextureData.AtlasStats.FrontTileCount,
			TextureData.AtlasStats.BackTileCount,
			TextureData.AtlasStats.PaintedPixels,
			TextureData.AtlasStats.AlphaAwareCroppedPlanes,
			TextureData.AtlasStats.AlphaAwareTileCropGuardPixels,
			TextureData.AtlasStats.RasterizedTriangleReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			Request.Mode == EFoliageBakerCardBakeMode::CrossCards
				? TEXT("dedicated fixed-angle orthographic capture, front and back per plane, all selected-LOD triangles, WPO disabled")
				: UsesMultiBillboard(Request)
					? TEXT("dedicated fixed-axis orthographic capture per clustered leaf group, matched leaf triangles only, WPO disabled")
					: UsesDoublePlanesBillboard(Request)
						? TEXT("two dedicated fixed-axis orthographic captures separated by 90 degrees, all selected-LOD triangles, WPO disabled")
						: TEXT("dedicated fixed-axis orthographic capture, all selected-LOD triangles, WPO disabled"),
			UsesDoublePlanesBillboard(Request)
				? TEXT("one shared per-tile masked RDG depth winner supplies BaseColor, source object normal, source triangle ID, and packed Mix; each view normal is re-expressed in its capture Facing/Right/Up frame before atlas storage")
				: TEXT("one shared per-tile masked RDG depth winner supplies BaseColor, object normal, source triangle ID, and packed Mix; no CPU material-property fallback"),
			*BaseAtlasPath,
			*NormalAtlasPath,
			UsesDoublePlanesBillboard(Request)
				? TEXT("per-view capture-frame normal")
				: TEXT("object/local-space normal"),
			*MixAtlasPath,
			*MaterialScalarDetails,
			AtlasUVDetails,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails,
			WindingDetails);
		if (TextureData.UpperHemisphereL1VisibilityTexture)
		{
			Report += FString::Printf(
				TEXT("\n  upper-hemisphere L1 visibility atlas: %s, size=%dx%d, configured maximum dimension=%d, RGB=object/local-space signed Cxyz remapped to 0..1, A=C0, samples=%d, internal shadow resolution=%d, material parameter=%s"),
				*TextureData.UpperHemisphereL1VisibilityTexture->GetPathName(),
				TextureData.UpperHemisphereL1VisibilityTexture->GetSizeX(),
				TextureData.UpperHemisphereL1VisibilityTexture->GetSizeY(),
				Request.UpperHemisphereL1TextureResolution,
				Request.UpperHemisphereL1SampleCount,
				Request.UpperHemisphereL1ShadowMapResolution,
				*Request.UpperHemisphereL1VisibilityTextureParameterName.ToString());
		}
		if (UsesMultiBillboard(Request))
		{
			Report += Request.bIncludeReducedTrunk
				? FString::Printf(
					TEXT("\n  retained trunk: enabled, source triangles=%d, reduced triangles=%d, requested percentage=%.1f%%, material slots=%d, UV channels=%d, source UVs/materials preserved"),
					MeshData.OriginalTrunkTriangleCount,
					MeshData.ReducedTrunkTriangleCount,
					Request.TrunkTrianglePercentage * 100.0f,
					MeshData.AdditionalMaterialSlots.Num(),
					MeshData.RetainedTrunkUVChannelCount)
				: TEXT("\n  retained trunk: disabled");
		}
		return Report;
	}

	FProxyAssetBuildResult BuildCardProxyAsset(
		UStaticMesh& StaticMesh,
		const FFoliageBakerCardBakeRequest& EditorSettings)
	{
		FString Error;
		FProxyPlaneCoverBuildData CoverData;
		if (!BuildProxyPlaneCoverData(StaticMesh, EditorSettings, CoverData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyMeshBuildData MeshData;
		if (!BuildProxyMeshData(CoverData, MeshData, Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
			FFoliageBakerMeshOutputDialog::OpenAfterBake(StaticMesh, EditorSettings.SourceLODIndex);
		if (!MeshOutputSelection.IsSet())
		{
			return MakeProxyBuildCancelled(StaticMesh);
		}
		if (MeshOutputSelection->OutputMode != EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
			&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
				StaticMesh,
				BuildSourceLODAssetParams(EditorSettings, *MeshOutputSelection),
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FFoliageBakerGeneratedAssetOutputFolders OutputFolders;
		if (EditorSettings.bPlaceGeneratedAssetsNearReplacedLODAssets
			&& MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
		{
			OutputFolders = FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
				StaticMesh,
				MeshOutputSelection->ReplaceLODIndex);
		}

		FProxyTextureBuildData TextureData;
		FFoliageBakerAssetTransaction AssetTransaction;
		if (!BuildProxyTextureData(
				StaticMesh,
				EditorSettings,
				AssetTransaction,
				CoverData,
				MeshData,
				OutputFolders,
				TextureData,
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
		if (!AppendReducedMultiBillboardTrunk(
			StaticMesh,
			EditorSettings,
			CoverData,
			TextureData.Material,
			MeshData,
			Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(
			StaticMesh,
			EditorSettings,
			*MeshOutputSelection,
			AssetTransaction,
			MeshData,
			TextureData,
			Result,
			Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
		AssetTransaction.Commit();

		Result.bSucceeded = true;
		Result.Report = BuildProxySuccessReport(StaticMesh, EditorSettings, CoverData, MeshData, TextureData, Result);
		UE_LOG(LogFoliageBakerCards, Display, TEXT("\n%s"), *Result.Report);
		return Result;
	}

	void AppendCardCreatedAssets(const FProxyAssetBuildResult& BuildResult, TArray<UObject*>& OutCreatedAssets)
	{
		if (BuildResult.MeshOutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset && BuildResult.ProxyMesh)
		{
			OutCreatedAssets.Add(BuildResult.ProxyMesh);
		}
		if (BuildResult.AtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.AtlasTexture);
		}
		if (BuildResult.NormalAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.NormalAtlasTexture);
		}
		if (BuildResult.MixAtlasTexture)
		{
			OutCreatedAssets.Add(BuildResult.MixAtlasTexture);
		}
		if (BuildResult.UpperHemisphereL1VisibilityTexture)
		{
			OutCreatedAssets.Add(BuildResult.UpperHemisphereL1VisibilityTexture);
		}
		if (BuildResult.Material)
		{
			OutCreatedAssets.Add(BuildResult.Material);
		}
	}
}


FFoliageBakerCardBakeResult FFoliageBakerCardBaker::Bake(const FFoliageBakerCardBakeRequest& Request)
{
	FFoliageBakerCardBakeResult OutResult;
	if (!Request.SourceStaticMesh)
	{
		OutResult.Report = TEXT("Foliage Baker failed: source Static Mesh is null.");
		return OutResult;
	}
	if (Request.SourceLODIndex < 0 || Request.SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: source LOD index %d is outside the supported range 0-%d."),
			*Request.SourceStaticMesh->GetName(),
			Request.SourceLODIndex,
			MAX_STATIC_MESH_LODS - 1);
		return OutResult;
	}
	if (!Request.MaterialTemplate)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: a parent Material Instance Constant must be selected in the current tool settings."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	if (!Request.bBakeBaseColorOpacity
		&& !Request.bBakeNormalDepth
		&& !Request.bBakeMix
		&& !Request.bBakeUpperHemisphereL1Visibility)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: no texture output is enabled."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	if (Request.bBakeUpperHemisphereL1Visibility
		&& Request.Mode != EFoliageBakerCardBakeMode::SingleBillboard)
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: Upper Hemisphere L1 Visibility is currently supported only by Billboard modes."),
			*Request.SourceStaticMesh->GetName());
		return OutResult;
	}
	TSet<FName> UsedTextureParameterNames;
	FString TextureParameterError;
	auto ValidateTextureParameterName = [&](const bool bEnabled, const FName ParameterName, const TCHAR* OutputLabel) -> bool
	{
		if (!bEnabled)
		{
			return true;
		}
		if (ParameterName.IsNone())
		{
			TextureParameterError = FString::Printf(
				TEXT("%s output is enabled, but its Material texture parameter name is None."),
				OutputLabel);
			return false;
		}
		if (UsedTextureParameterNames.Contains(ParameterName))
		{
			TextureParameterError = FString::Printf(
				TEXT("Material texture parameter '%s' is assigned to more than one enabled output."),
				*ParameterName.ToString());
			return false;
		}
		UsedTextureParameterNames.Add(ParameterName);
		return true;
	};
	if (!ValidateTextureParameterName(Request.bBakeBaseColorOpacity, Request.BaseColorOpacityTextureParameterName, TEXT("BaseColor/Opacity"))
		|| !ValidateTextureParameterName(Request.bBakeNormalDepth, Request.NormalDepthTextureParameterName, TEXT("Normal/TrunkLeafMask"))
		|| !ValidateTextureParameterName(Request.bBakeMix, Request.MixTextureParameterName, TEXT("Mix"))
		|| !ValidateTextureParameterName(
			Request.bBakeUpperHemisphereL1Visibility,
			Request.UpperHemisphereL1VisibilityTextureParameterName,
			TEXT("Upper Hemisphere L1 Visibility")))
	{
		OutResult.Report = FString::Printf(
			TEXT("%s\n  failed: %s"),
			*Request.SourceStaticMesh->GetName(),
			*TextureParameterError);
		return OutResult;
	}
	if (Request.TextureResolution < 256 || Request.TextureResolution > 4096)
	{
		OutResult.Report = FString::Printf(TEXT("%s\n  failed: texture resolution must be between 256 and 4096."), *Request.SourceStaticMesh->GetName());
		return OutResult;
	}

	FFoliageBakerCardBakeRequest SanitizedRequest = Request;
	SanitizedRequest.SourceLODIndex = Request.SourceLODIndex;
	SanitizedRequest.CrossCardPlaneCount = FMath::Clamp(Request.CrossCardPlaneCount, 2, 5);
	SanitizedRequest.MultiBillboardClusterCount =
		FMath::Clamp(Request.MultiBillboardClusterCount, 1, 128);
	SanitizedRequest.MultiBillboardsPerCluster =
		FMath::Clamp(Request.MultiBillboardsPerCluster, 2, 8);
	SanitizedRequest.AlphaCropGuardPixels = FMath::Clamp(Request.AlphaCropGuardPixels, 2, 16);
	SanitizedRequest.MipMaskCoverageThreshold =
		FMath::Clamp(Request.MipMaskCoverageThreshold, 0.01f, 1.0f);
	SanitizedRequest.UpperHemisphereL1TextureResolution =
		FMath::Clamp(Request.UpperHemisphereL1TextureResolution, 64, 1024);
	SanitizedRequest.UpperHemisphereL1SampleCount =
		FMath::Clamp(Request.UpperHemisphereL1SampleCount, 4, 32);
	SanitizedRequest.UpperHemisphereL1ShadowMapResolution =
		FMath::Clamp(Request.UpperHemisphereL1ShadowMapResolution, 64, 1024);
	const FString FeatureSuffix = Request.Mode == EFoliageBakerCardBakeMode::CrossCards
		? TEXT("_Cross")
		: UsesMultiBillboard(Request)
			? TEXT("_MultiBillboard")
		: UsesDoublePlanesBillboard(Request)
			? TEXT("_DoubleBillboard")
			: TEXT("_Billboard");
	SanitizedRequest.BaseColorOpacityTextureSuffix = FeatureSuffix + Request.BaseColorOpacityTextureSuffix;
	SanitizedRequest.NormalDepthTextureSuffix = FeatureSuffix + Request.NormalDepthTextureSuffix;
	SanitizedRequest.MixTextureSuffix = FeatureSuffix + Request.MixTextureSuffix;
	SanitizedRequest.UpperHemisphereL1VisibilityTextureSuffix =
		FeatureSuffix + Request.UpperHemisphereL1VisibilityTextureSuffix;
	SanitizedRequest.MaterialInstanceNameSuffix = FeatureSuffix + Request.MaterialInstanceNameSuffix;

	const FProxyAssetBuildResult InternalResult = BuildCardProxyAsset(*Request.SourceStaticMesh, SanitizedRequest);
	OutResult.bSucceeded = InternalResult.bSucceeded;
	OutResult.bCancelled = InternalResult.bCancelled;
	OutResult.ProxyMesh = InternalResult.ProxyMesh;
	OutResult.SourceMeshLODIndex = InternalResult.SourceMeshLODIndex;
	OutResult.ColorOpacityTexture = InternalResult.AtlasTexture;
	OutResult.NormalDepthTexture = InternalResult.NormalAtlasTexture;
	OutResult.MixTexture = InternalResult.MixAtlasTexture;
	OutResult.UpperHemisphereL1VisibilityTexture =
		InternalResult.UpperHemisphereL1VisibilityTexture;
	OutResult.MaterialInstance = InternalResult.Material;
	OutResult.Report = InternalResult.Report;
	AppendCardCreatedAssets(InternalResult, OutResult.CreatedAssets);
	return OutResult;
}
