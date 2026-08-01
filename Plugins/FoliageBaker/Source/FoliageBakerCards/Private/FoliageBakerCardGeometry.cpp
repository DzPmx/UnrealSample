#include "FoliageBakerCardGeometry.h"

#include "Engine/StaticMesh.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshReductionSettings.h"
#include "Modules/ModuleManager.h"
#include "OverlappingCorners.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

namespace UE::FoliageBaker::Cards::Geometry
{
	namespace
	{
		FName MakeUniqueMaterialSlotName(
			const UMaterialInterface& Material,
			const int32 SourceMaterialIndex,
			TSet<FName>& InOutUsedSlotNames)
		{
			const FString BaseSlotName = !Material.GetName().IsEmpty()
				? Material.GetName()
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
			const TArray<PlaneCover::FSourceTriangle>& TrunkTriangles,
			UMaterialInterface& ProxyMaterial,
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
			for (const PlaneCover::FSourceTriangle& Triangle : TrunkTriangles)
			{
				UVChannelCount = FMath::Max(UVChannelCount, Triangle.NumUVChannels);
			}
			UVChannelCount = FMath::Clamp(
				UVChannelCount,
				1,
				PlaneCover::MaxSourceMeshUVChannels);
			VertexInstanceUVs.SetNumChannels(UVChannelCount);

			OutMeshDescription.ReserveNewVertices(TrunkTriangles.Num() * 3);
			OutMeshDescription.ReserveNewVertexInstances(TrunkTriangles.Num() * 3);
			OutMeshDescription.ReserveNewTriangles(TrunkTriangles.Num());

			const TArray<FStaticMaterial>& StaticMaterials = StaticMesh.GetStaticMaterials();
			TMap<int32, FPolygonGroupID> PolygonGroupByMaterialIndex;
			TMap<FVector3f, FVertexID> VertexByPosition;
			TSet<FName> UsedMaterialSlotNames;
			UsedMaterialSlotNames.Add(TEXT("BillboardProxy"));
			if (!ProxyMaterial.GetFName().IsNone())
			{
				UsedMaterialSlotNames.Add(ProxyMaterial.GetFName());
			}

			for (const PlaneCover::FSourceTriangle& Triangle : TrunkTriangles)
			{
				FPolygonGroupID PolygonGroupID = INDEX_NONE;
				if (PolygonGroupByMaterialIndex.Contains(Triangle.MaterialIndex))
				{
					PolygonGroupID =
						PolygonGroupByMaterialIndex.FindChecked(
							Triangle.MaterialIndex);
				}
				else
				{
					const TObjectPtr<UMaterialInterface> Material =
						StaticMaterials.IsValidIndex(Triangle.MaterialIndex)
							? StaticMaterials[Triangle.MaterialIndex].MaterialInterface
							: UMaterial::GetDefaultMaterial(MD_Surface);
					check(Material);
					const FName MaterialSlotName = MakeUniqueMaterialSlotName(
						*Material,
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
					if (VertexByPosition.Contains(Position))
					{
						VertexID = VertexByPosition.FindChecked(Position);
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
	}

	bool AppendReducedTrunk(
		const UStaticMesh& StaticMesh,
		const TArray<PlaneCover::FSourceTriangle>& TrunkTriangles,
		const float TrianglePercentage,
		UMaterialInterface& ProxyMaterial,
		FMeshDescription& InOutMeshDescription,
		FRetainedTrunkResult& OutResult,
		FString& OutError)
	{
		OutResult = FRetainedTrunkResult();
		if (TrunkTriangles.IsEmpty())
		{
			return true;
		}

		FMeshDescription TrunkMeshDescription;
		TArray<FFoliageBakerMeshMaterialSlot> TrunkMaterialSlots;
		if (!BuildRetainedTrunkMeshDescription(
			StaticMesh,
			TrunkTriangles,
			ProxyMaterial,
			TrunkMeshDescription,
			TrunkMaterialSlots,
			OutError))
		{
			return false;
		}

		OutResult.OriginalTriangleCount = TrunkMeshDescription.Triangles().Num();
		FMeshDescription ReducedTrunkMeshDescription(TrunkMeshDescription);
		const float ClampedTrianglePercentage = FMath::Clamp(TrianglePercentage, 0.05f, 1.0f);
		if (ClampedTrianglePercentage < 1.0f && TrunkMeshDescription.Triangles().Num() > 2)
		{
			IMeshReductionManagerModule* MeshReductionModule =
				FModuleManager::Get().LoadModulePtr<IMeshReductionManagerModule>("MeshReductionInterface");
			IMeshReduction* MeshReduction = MeshReductionModule
				? MeshReductionModule->GetStaticMeshReductionInterface()
				: nullptr;
			if (!MeshReduction || !MeshReduction->IsSupported())
			{
				OutError = TEXT("MultiBillboard could not load a supported Unreal Static Mesh "
					"reduction interface for the retained trunk.");
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
			ReductionSettings.PercentTriangles = ClampedTrianglePercentage;
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
		OutResult.ReducedTriangleCount = ReducedTrunkMeshDescription.Triangles().Num();
		OutResult.UVChannelCount =
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
		for (int32 UVChannel = 0;
			UVChannel < FStaticMeshOperations::FAppendSettings::MAX_NUM_UV_CHANNELS;
			++UVChannel)
		{
			AppendSettings.bMergeUVChannels[UVChannel] = true;
		}
		FStaticMeshOperations::AppendMeshDescription(
			ReducedTrunkMeshDescription,
			InOutMeshDescription,
			AppendSettings);
		if (InOutMeshDescription.NeedsCompact())
		{
			FElementIDRemappings Remappings;
			InOutMeshDescription.Compact(Remappings);
		}
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(InOutMeshDescription);
		OutResult.MaterialSlots = MoveTemp(TrunkMaterialSlots);
		return true;
	}

	bool BuildDoublePlanesTwoViewsOutput(
		const FVector& OutputNormal,
		const PlaneCover::FPlaneProxySettings& PlaneSettings,
		const FFoliageBakerProxyGeometry& CaptureGeometry,
		FMeshDescription& OutMeshDescription,
		PlaneCover::FPlaneProxyMeshStats& OutStats,
		FString& OutError)
	{
		if (CaptureGeometry.PlaneInfos.Num() != 2)
		{
			OutError = FString::Printf(
				TEXT("Double Planes - Two Views Billboard requires exactly two captured planes, but %d were generated."),
				CaptureGeometry.PlaneInfos.Num());
			return false;
		}

		const FVector Normal = OutputNormal.GetSafeNormal();
		const FVector AxisU = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();
		if (Normal.IsNearlyZero() || AxisU.IsNearlyZero())
		{
			OutError = TEXT("Double Planes - Two Views Billboard could not construct a horizontal output plane frame.");
			return false;
		}

		// Runtime UV payloads are derived from a copy so capture metadata remains
		// stable for atlas tiles, mip generation, and L1 visibility reconstruction.
		TArray<PlaneCover::FPlaneProxyPlaneInfo> OutputPlaneInfos = CaptureGeometry.PlaneInfos;
		for (int32 PlaneIndex = 0; PlaneIndex < OutputPlaneInfos.Num(); ++PlaneIndex)
		{
			PlaneCover::FPlaneProxyPlaneInfo& OutputPlane = OutputPlaneInfos[PlaneIndex];
			const FVector CaptureNormal = OutputPlane.Normal.GetSafeNormal();
			if (CaptureNormal.IsNearlyZero())
			{
				OutError = FString::Printf(
					TEXT("Double Planes - Two Views Billboard capture plane %d has an invalid direction."),
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

			OutputPlane.Normal = Normal;
			OutputPlane.Rho = 0.0;
			OutputPlane.AxisU = AxisU;
			OutputPlane.AxisV = FVector::UpVector;
			OutputPlane.ShadingNormal = Normal;
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

		OutStats = CaptureGeometry.Stats;
		if (!PlaneCover::RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			OutputPlaneInfos,
			PlaneSettings,
			OutMeshDescription,
			OutStats,
			OutError))
		{
			return false;
		}
		OutStats.AveragePlaneToShadingNormalDot = 1.0;
		OutStats.AveragePlaneToShadingNormalAngleDegrees = 0.0;
		return true;
	}

	bool BuildSinglePlaneTwoViewsOutput(
		const PlaneCover::FPlaneProxySettings& PlaneSettings,
		const FFoliageBakerProxyGeometry& CaptureGeometry,
		FMeshDescription& OutMeshDescription,
		PlaneCover::FPlaneProxyMeshStats& OutStats,
		FString& OutError)
	{
		if (CaptureGeometry.PlaneInfos.Num() != 2)
		{
			OutError = FString::Printf(
				TEXT("Single Plane - Two Views Billboard requires exactly two captured planes, but %d were generated."),
				CaptureGeometry.PlaneInfos.Num());
			return false;
		}

		const PlaneCover::FPlaneProxyPlaneInfo& PrimaryCapture =
			CaptureGeometry.PlaneInfos[0];
		const PlaneCover::FPlaneProxyPlaneInfo& SecondaryCapture =
			CaptureGeometry.PlaneInfos[1];
		const FVector PrimaryCaptureNormal = PrimaryCapture.Normal.GetSafeNormal();
		if (PrimaryCaptureNormal.IsNearlyZero())
		{
			OutError = TEXT("Single Plane - Two Views Billboard has an invalid primary capture direction.");
			return false;
		}

		TArray<PlaneCover::FPlaneProxyPlaneInfo> OutputPlaneInfos;
		PlaneCover::FPlaneProxyPlaneInfo& OutputPlane =
			OutputPlaneInfos.Add_GetRef(PrimaryCapture);
		for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
		{
			OutputPlane.BackAtlasUVs[CornerIndex] =
				SecondaryCapture.AtlasUVs[CornerIndex];
		}
		OutputPlane.bHasBackFaceAtlas = false;
		OutputPlane.bUseCustomAuxiliaryUV = true;
		OutputPlane.AuxiliaryUV = FVector2f(
			static_cast<float>(PrimaryCaptureNormal.X),
			static_cast<float>(PrimaryCaptureNormal.Y));

		OutStats = CaptureGeometry.Stats;
		return PlaneCover::RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			OutputPlaneInfos,
			PlaneSettings,
			OutMeshDescription,
			OutStats,
			OutError);
	}

	bool BuildMultiBillboardOutput(
		const TArray<int32>& PlaneGroupIndices,
		const TArray<FVector>& ClusterCenters,
		const PlaneCover::FPlaneProxySettings& PlaneSettings,
		const FFoliageBakerProxyGeometry& CaptureGeometry,
		FMeshDescription& OutMeshDescription,
		PlaneCover::FPlaneProxyMeshStats& OutStats,
		FString& OutError)
	{
		if (CaptureGeometry.PlaneInfos.IsEmpty())
		{
			OutError = TEXT("MultiBillboard has no generated planes to prepare for runtime rotation.");
			return false;
		}
		if (PlaneGroupIndices.Num() != CaptureGeometry.PlaneInfos.Num())
		{
			OutError = TEXT("MultiBillboard cluster mapping does not match generated plane count.");
			return false;
		}

		TArray<PlaneCover::FPlaneProxyPlaneInfo> OutputPlaneInfos = CaptureGeometry.PlaneInfos;
		for (int32 PlaneIndex = 0; PlaneIndex < OutputPlaneInfos.Num(); ++PlaneIndex)
		{
			PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo = OutputPlaneInfos[PlaneIndex];
			const FVector AxisU = PlaneInfo.AxisU.GetSafeNormal();
			const FVector AxisV = PlaneInfo.AxisV.GetSafeNormal();
			const FVector Normal = PlaneInfo.Normal.GetSafeNormal();
			const int32 ClusterIndex = PlaneGroupIndices[PlaneIndex];
			if (AxisU.IsNearlyZero()
				|| AxisV.IsNearlyZero()
				|| Normal.IsNearlyZero()
				|| !ClusterCenters.IsValidIndex(ClusterIndex))
			{
				OutError = TEXT("MultiBillboard generated a plane with an invalid local frame.");
				return false;
			}
			const FVector ClusterCenter = ClusterCenters[ClusterIndex];
			const FVector PlaneCenter =
				(PlaneInfo.Corners[0]
					+ PlaneInfo.Corners[1]
					+ PlaneInfo.Corners[2]
					+ PlaneInfo.Corners[3]) * 0.25;
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				const FVector CenterRelativePosition = PlaneInfo.Corners[CornerIndex] - ClusterCenter;
				PlaneInfo.BackAtlasUVs[CornerIndex] = FVector2f(
					static_cast<float>(FVector::DotProduct(CenterRelativePosition, AxisU)),
					static_cast<float>(FVector::DotProduct(CenterRelativePosition, AxisV)));
			}
			PlaneInfo.bUseCustomAuxiliaryUV = true;
			PlaneInfo.AuxiliaryUV = FVector2f(
				static_cast<float>(FVector::DotProduct(PlaneCenter - ClusterCenter, Normal)),
				0.0f);
		}

		OutStats = CaptureGeometry.Stats;
		return PlaneCover::RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			OutputPlaneInfos,
			PlaneSettings,
			OutMeshDescription,
			OutStats,
			OutError);
	}
}
