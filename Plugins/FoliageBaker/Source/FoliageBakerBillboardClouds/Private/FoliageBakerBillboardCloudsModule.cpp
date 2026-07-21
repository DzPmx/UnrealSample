#include "FoliageBakerBillboardCloudsModule.h"

#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerBillboardCloudsSettings.h"
#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerFeatureTool.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerKMeansPlaneCover.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "MeshDescription.h"
#include "MaterialBakingStructures.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"

#define LOCTEXT_NAMESPACE "FFoliageBakerBillboardCloudsModule"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerBillboardClouds, Log, All);

namespace
{
	using UE::FoliageBaker::ProjectedMaterialBake::EncodeObjectSpaceNormalToColor;

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

	UE::FoliageBaker::PlaneCover::FPlaneProxySettings BuildSettingsForMesh(const FBoxSphereBounds& SourceLODBounds, const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		switch (EditorSettings.KMeansCrackReductionMode)
		{
		case EBillboardCloudsCrackReductionMode::ScaledEnvelopeClip:
			Settings.CrackReductionMode = UE::FoliageBaker::PlaneCover::EPlaneProxyCrackReductionMode::ScaledEnvelopeClip;
			break;
		case EBillboardCloudsCrackReductionMode::Off:
		default:
			Settings.CrackReductionMode = UE::FoliageBaker::PlaneCover::EPlaneProxyCrackReductionMode::Off;
			break;
		}
		Settings.CrackReductionProjectionScale = FMath::Clamp(EditorSettings.KMeansCrackReductionProjectionScale, 0.0, 1.0);
		Settings.TextureAtlasResolution = FMath::Clamp(EditorSettings.TextureAtlasResolution, 256, 4096);
		Settings.AtlasVConvention = UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMinV;
		switch (EditorSettings.DoubleSidedBakeMode)
		{
		case EBillboardCloudsDoubleSidedBakeMode::TrunkCardsOnly:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::TrunkCardsOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::BillboardPlanesOnly:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::BillboardPlanesOnly;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::AllPlanes:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::AllPlanes;
			break;
		case EBillboardCloudsDoubleSidedBakeMode::Off:
		default:
			Settings.DoubleSidedBakeMode = UE::FoliageBaker::PlaneCover::EDoubleSidedBakeMode::Off;
			break;
		}
		Settings.ErrorTolerance = FMath::Max(
			FMath::Max(0.0, EditorSettings.MinimumErrorCm),
			SourceLODBounds.SphereRadius * FMath::Max(0.0, EditorSettings.RelativeError));
		switch (EditorSettings.TrunkCardAtlasScale)
		{
		case EBillboardCloudsTrunkCardAtlasScale::HalfX:
			Settings.TrunkCardAtlasScale = 0.5;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::OneX:
			Settings.TrunkCardAtlasScale = 1.0;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::OnePointFiveX:
			Settings.TrunkCardAtlasScale = 1.5;
			break;
		case EBillboardCloudsTrunkCardAtlasScale::TwoX:
			Settings.TrunkCardAtlasScale = 2.0;
			break;
		default:
			Settings.TrunkCardAtlasScale = 1.0;
			break;
		}
		Settings.bEnableAlphaAwareTileCrop = EditorSettings.bEnableAlphaAwareTileCrop;
		Settings.AlphaAwareTileCropGuardPixels = FMath::Clamp(EditorSettings.AlphaAwareTileCropGuardPixels, 2, 16);
		return Settings;
	}

	UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings BuildKMeansSettings(
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings Settings;
		Settings.PlaneCount = FMath::Clamp(EditorSettings.KMeansPlaneCount, 1, 512);
		Settings.MaxIterations = FMath::Clamp(EditorSettings.KMeansMaxIterations, 1, 512);
		return Settings;
	}

	struct FTrunkCardTriangleSplit
	{
		bool bEnabled = false;
		int32 MatchedMaterialCount = 0;
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> BillboardTriangles;
		TArray<int32> BillboardToSourceTriangleIndices;
		TArray<int32> TrunkTriangleIndices;
	};

	FTrunkCardTriangleSplit SplitTrianglesForTrunkCards(
		const UStaticMesh& StaticMesh,
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const bool bEnableTrunkCards,
		const TArray<FString>& RawKeywords)
	{
		FTrunkCardTriangleSplit Split;
		const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult MaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(
				StaticMesh,
				RawKeywords);
		Split.bEnabled = bEnableTrunkCards && MaterialMatches.bEnabled;
		Split.MatchedMaterialCount = MaterialMatches.MatchedMaterialCount;
		Split.BillboardTriangles.Reserve(SourceTriangles.Num());
		Split.BillboardToSourceTriangleIndices.Reserve(SourceTriangles.Num());

		for (int32 SourceTriangleIndex = 0; SourceTriangleIndex < SourceTriangles.Num(); ++SourceTriangleIndex)
		{
			UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = SourceTriangles[SourceTriangleIndex];
			Triangle.bIsTrunk = MaterialMatches.IsMatch(Triangle.MaterialIndex);
			const bool bUseTrunkCards = Split.bEnabled && Triangle.bIsTrunk;
			Triangle.bTrunkCardOnly = bUseTrunkCards;
			if (bUseTrunkCards)
			{
				Split.TrunkTriangleIndices.Add(SourceTriangleIndex);
			}
			else
			{
				Split.BillboardToSourceTriangleIndices.Add(SourceTriangleIndex);
				Split.BillboardTriangles.Add(Triangle);
			}
		}

		return Split;
	}

	UE::FoliageBaker::PlaneCover::FPlaneProxySet RemapPlaneCoverResultToSourceTriangles(
		const UE::FoliageBaker::PlaneCover::FPlaneProxySet& BillboardResult,
		const TArray<int32>& BillboardToSourceTriangleIndices,
		const int32 SourceTriangleCount)
	{
		UE::FoliageBaker::PlaneCover::FPlaneProxySet RemappedResult = BillboardResult;
		RemappedResult.SourceTriangleCount = SourceTriangleCount;
		for (UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane : RemappedResult.Planes)
		{
			TArray<int32> RemappedTriangleIndices;
			RemappedTriangleIndices.Reserve(Plane.TriangleIndices.Num());
			for (const int32 BillboardTriangleIndex : Plane.TriangleIndices)
			{
				if (BillboardToSourceTriangleIndices.IsValidIndex(BillboardTriangleIndex))
				{
					RemappedTriangleIndices.Add(BillboardToSourceTriangleIndices[BillboardTriangleIndex]);
				}
			}
			Plane.TriangleIndices = MoveTemp(RemappedTriangleIndices);
		}
		return RemappedResult;
	}

	int32 AppendTrunkCrossCardPlanes(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& SourceTriangles,
		const TArray<int32>& TrunkTriangleIndices,
		const int32 RequestedTrunkPlaneCount,
		UE::FoliageBaker::PlaneCover::FPlaneProxySet& InOutResult)
	{
		if (TrunkTriangleIndices.IsEmpty())
		{
			return 0;
		}

		FBox TrunkBounds(ForceInit);
		double TrunkArea = 0.0;
		for (const int32 TriangleIndex : TrunkTriangleIndices)
		{
			if (!SourceTriangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle = SourceTriangles[TriangleIndex];
			TrunkArea += Triangle.Area;
			for (const FVector& Vertex : Triangle.Vertices)
			{
				TrunkBounds += Vertex;
			}
		}

		if (!TrunkBounds.IsValid)
		{
			return 0;
		}

		const int32 TrunkPlaneCount = FMath::Clamp(RequestedTrunkPlaneCount, 2, 8);
		for (int32 PlaneIndex = 0; PlaneIndex < TrunkPlaneCount; ++PlaneIndex)
		{
			const double AngleRadians = static_cast<double>(PlaneIndex) * UE_DOUBLE_PI / static_cast<double>(TrunkPlaneCount);
			const FVector Normal(FMath::Cos(AngleRadians), FMath::Sin(AngleRadians), 0.0);
			const FVector AxisV = FVector::UpVector;
			FVector AxisU = FVector::CrossProduct(AxisV, Normal).GetSafeNormal();
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::RightVector;
			}

			UE::FoliageBaker::PlaneCover::FPlaneProxyInput& Plane = InOutResult.Planes.AddDefaulted_GetRef();
			Plane.Normal = Normal;
			Plane.Rho = 0.0;
			Plane.Score = TrunkArea;
			Plane.CoveredArea = TrunkArea;
			Plane.TriangleIndices = TrunkTriangleIndices;
			Plane.bIsTrunkCard = true;
			Plane.bUseFixedPlaneFrame = true;
			Plane.FixedAxisU = AxisU;
			Plane.FixedAxisV = AxisV;
		}

		InOutResult.CoveredTriangleCount += TrunkTriangleIndices.Num();
		InOutResult.CoveredArea += TrunkArea;
		return TrunkPlaneCount;
	}

	FString BuildTrunkCrossCardSummary(const FTrunkCardTriangleSplit& Split, const int32 TrunkPlaneCount, const double TrunkCardAtlasScale)
	{
		if (!Split.bEnabled)
		{
			return TEXT("");
		}

		FString LayoutName = TEXT("off");
		if (TrunkPlaneCount == 2)
		{
			LayoutName = TEXT("cross card");
		}
		else if (TrunkPlaneCount == 3)
		{
			LayoutName = TEXT("three-way star");
		}
		else if (TrunkPlaneCount == 4)
		{
			LayoutName = TEXT("four-way star");
		}
		else if (TrunkPlaneCount > 4)
		{
			LayoutName = FString::Printf(TEXT("%d-way star"), TrunkPlaneCount);
		}

		return FString::Printf(
			TEXT("\n  trunk cards: enabled, matched materials=%d, trunk triangles=%d, billboard input triangles=%d, vertical planes=%d (%s), atlas scale=%.1fx, origin-centered, shooting=horizontal ortho trunk-only"),
			Split.MatchedMaterialCount,
			Split.TrunkTriangleIndices.Num(),
			Split.BillboardTriangles.Num(),
			TrunkPlaneCount,
			*LayoutName,
			FMath::Clamp(TrunkCardAtlasScale, 0.5, 2.0));
	}

	using FAtlasOutputSelection = UE::FoliageBaker::MaterialResolver::FMaterialOutputSelection;

	struct FAtlasBakeStats
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 TileResolution = 0;
		int32 PaintedPixels = 0;
		int32 FrontTileCount = 0;
		int32 BackTileCount = 0;
		double PackedTileUtilizationPercent = 0.0;
		int32 RasterizedTriangleReferences = 0;
		int32 CrackReductionTriangleReferences = 0;
		int32 MaskedMaterialBakeReferences = 0;
		int32 AlphaAwareCroppedPlanes = 0;
		int32 AlphaAwareTileCropGuardPixels = 0;
		FString MaterialAlphaPolicyDetails;
		UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialAverages MaterialAverages;
	};

	int32 GetSourceMeshMaxUVChannelCount(const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles)
	{
		int32 ChannelCount = 1;
		for (const UE::FoliageBaker::PlaneCover::FSourceTriangle& Triangle : Triangles)
		{
			ChannelCount = FMath::Max(ChannelCount, Triangle.NumUVChannels);
		}
		return FMath::Clamp(ChannelCount, 1, UE::FoliageBaker::PlaneCover::MaxMaterialBakeUVChannels);
	}

	TArray<int32> CollectReferencedMaterialIndices(
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<int32>& TriangleIndices,
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection>& CrackReductionProjections)
	{
		TSet<int32> Set;
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (Triangles.IsValidIndex(TriangleIndex))
			{
				Set.Add(FMath::Max(0, Triangles[TriangleIndex].MaterialIndex));
			}
		}
		for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : CrackReductionProjections)
		{
			if (Triangles.IsValidIndex(Projection.TriangleIndex))
			{
				Set.Add(FMath::Max(0, Triangles[Projection.TriangleIndex].MaterialIndex));
			}
		}
		TArray<int32> Result = Set.Array();
		Result.Sort();
		return Result;
	}

	bool BakeBillboardAtlasGPU(
		const UStaticMesh& SourceStaticMesh,
		const FBoxSphereBounds& SourceLODBounds,
		const TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle>& Triangles,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats& ProxyStats,
		const UE::FoliageBaker::PlaneCover::FPlaneProxySettings& Settings,
		const FAtlasOutputSelection& OutputSelection,
		TArray<FColor>& OutPixels,
		TArray<FColor>& OutNormalPixels,
		TArray<FColor>& OutMixPixels,
		FAtlasBakeStats& OutStats,
		FString& OutError)
	{
		OutStats.Width = ProxyStats.AtlasWidth;
		OutStats.Height = ProxyStats.AtlasHeight;
		OutStats.TileResolution = ProxyStats.AtlasTileResolution;

		const int32 AtlasPixelCount = FMath::Max(0, OutStats.Width * OutStats.Height);
		OutPixels.Init(FColor(0, 0, 0, 0), AtlasPixelCount);
		if (OutputSelection.bNormalMask)
		{
			OutNormalPixels.Init(EncodeObjectSpaceNormalToColor(FVector::UpVector, 255), AtlasPixelCount);
		}
		else
		{
			OutNormalPixels.Reset();
		}
		if (OutputSelection.bMix)
		{
			OutMixPixels.Init(FColor(255, 128, 0, 0), AtlasPixelCount);
		}
		else
		{
			OutMixPixels.Reset();
		}

		int64 PackedPaddedTilePixels = 0;
		auto AccumulateAtlasTileStats = [&](const FIntPoint& TileSize, const int32 Padding, const bool bBackFace)
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return;
			}
			PackedPaddedTilePixels += static_cast<int64>(TileSize.X + Padding * 2)
				* static_cast<int64>(TileSize.Y + Padding * 2);
			if (bBackFace)
			{
				++OutStats.BackTileCount;
			}
			else
			{
				++OutStats.FrontTileCount;
			}
		};
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			AccumulateAtlasTileStats(PlaneInfo.AtlasTileSize, PlaneInfo.AtlasTilePaddingPixels, false);
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				AccumulateAtlasTileStats(PlaneInfo.BackAtlasTileSize, PlaneInfo.AtlasTilePaddingPixels, true);
			}
		}
		OutStats.PackedTileUtilizationPercent = AtlasPixelCount > 0
			? 100.0 * static_cast<double>(PackedPaddedTilePixels) / static_cast<double>(AtlasPixelCount)
			: 0.0;

		TBitArray<> AtlasCoverage;
		AtlasCoverage.Init(false, AtlasPixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, AtlasPixelCount);
		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		const int32 NumSourceUVChannels = GetSourceMeshMaxUVChannelCount(Triangles);

		OutStats.MaterialAlphaPolicyDetails =
			TEXT(" source masked-shader coverage controls one shared per-tile RDG depth competition; the winning fragment supplies BaseColor, object normal, source triangle ID, packed Mix, and shared-range depth; no CPU material-property fallback");

		auto BakePlaneAndSide = [&](
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo,
			const FIntPoint& TilePixelMin,
			const FIntPoint& TileSize,
			const FVector& CaptureRayDirection,
			const bool bBackSide) -> bool
		{
			if (TileSize.X <= 0 || TileSize.Y <= 0)
			{
				return true;
			}

			TArray<int32> PrimaryTriangleIndices;
			PrimaryTriangleIndices.Reserve(PlaneInfo.TriangleIndices.Num());
			TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection> CrackReductionProjectionsToBake;
			CrackReductionProjectionsToBake.Reserve(PlaneInfo.CrackReductionProjections.Num());
			TBitArray<> QueuedTriangles;
			QueuedTriangles.Init(false, Triangles.Num());
			for (const int32 TriangleIndex : PlaneInfo.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
				{
					QueuedTriangles[TriangleIndex] = true;
					PrimaryTriangleIndices.Add(TriangleIndex);
				}
			}
			if (!PlaneInfo.bIsTrunkCard)
			{
				for (const UE::FoliageBaker::PlaneCover::FCrackReductionProjection& Projection : PlaneInfo.CrackReductionProjections)
				{
					const int32 TriangleIndex = Projection.TriangleIndex;
					if (Triangles.IsValidIndex(TriangleIndex) && !QueuedTriangles[TriangleIndex])
					{
						QueuedTriangles[TriangleIndex] = true;
						CrackReductionProjectionsToBake.Add(Projection);
						++OutStats.CrackReductionTriangleReferences;
					}
				}
			}
			if (PrimaryTriangleIndices.IsEmpty() && CrackReductionProjectionsToBake.IsEmpty())
			{
				return true;
			}

			const int32 TilePixelCount = TileSize.X * TileSize.Y;
			const TArray<int32> MaterialIndicesUsed = CollectReferencedMaterialIndices(
				Triangles,
				PrimaryTriangleIndices,
				CrackReductionProjectionsToBake);
			{
				struct FDepthCorrectMaterialStorage
				{
					UMaterialInterface* MaterialInterface = nullptr;
					FMeshDescription MeshDescription;
					TArray<FVector2D> CustomTileUVs;
					TArray<int32> RasterSourceTriangleIndices;
					FMeshData MeshSettings;
				};

				TArray<TUniquePtr<FDepthCorrectMaterialStorage>> MaterialStorage;
				MaterialStorage.Reserve(MaterialIndicesUsed.Num());
				FFoliageBakerDepthCorrectTileRequest DepthCorrectRequest;
				DepthCorrectRequest.TextureSize = TileSize;
				DepthCorrectRequest.CaptureRayDirection = CaptureRayDirection;
				DepthCorrectRequest.SourceBounds = SourceLODBounds;
				DepthCorrectRequest.bBakeBaseColor = OutputSelection.bBaseColorOpacity;
				DepthCorrectRequest.bBakeObjectSpaceNormal = OutputSelection.bNormalMask;
				DepthCorrectRequest.bBakePackedMix = OutputSelection.bMix;
				DepthCorrectRequest.bBakeRoughnessSpecular =
					OutputSelection.bMaterialScalarAverages;
				DepthCorrectRequest.Materials.Reserve(MaterialIndicesUsed.Num());
				for (const int32 MaterialIndex : MaterialIndicesUsed)
				{
					TUniquePtr<FDepthCorrectMaterialStorage> Storage =
						MakeUnique<FDepthCorrectMaterialStorage>();
					Storage->MaterialInterface = SourceMaterials.IsValidIndex(MaterialIndex)
						? SourceMaterials[MaterialIndex].MaterialInterface
						: nullptr;
					if (!Storage->MaterialInterface)
					{
						Storage->MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
					}
					const bool bMaterialHasOpacityMask =
						Storage->MaterialInterface->GetBlendMode() == BLEND_Masked;

					UE::FoliageBaker::ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
					ProjectedBakeParams.CaptureRayDirection = CaptureRayDirection;
					ProjectedBakeParams.AtlasVConvention = Settings.AtlasVConvention;
					ProjectedBakeParams.MaterialIndexFilter = MaterialIndex;
					ProjectedBakeParams.NumSourceUVChannels = NumSourceUVChannels;
					ProjectedBakeParams.bBackSide = bBackSide;

					int32 MatchingTriangleCount = 0;
					FString ProjectedInputError;
					const bool bBuiltPlaneSideBakeInputs =
						UE::FoliageBaker::ProjectedMaterialBake::BuildPlaneSideBakeInputs(
							Triangles,
							PrimaryTriangleIndices,
							CrackReductionProjectionsToBake,
							PlaneInfo,
							ProjectedBakeParams,
							Storage->MeshDescription,
							Storage->CustomTileUVs,
							MatchingTriangleCount,
							&ProjectedInputError,
							&Storage->RasterSourceTriangleIndices);
					if (MatchingTriangleCount == 0)
					{
						continue;
					}
					if (!bBuiltPlaneSideBakeInputs)
					{
						OutError = FString::Printf(
							TEXT("BillboardClouds depth-correct material input failed for plane %d (%s), material %d: %s"),
							PlaneInfo.SourcePlaneIndex,
							bBackSide ? TEXT("back") : TEXT("front"),
							MaterialIndex,
							*ProjectedInputError);
						return false;
					}

					OutStats.RasterizedTriangleReferences += MatchingTriangleCount;
					if (bMaterialHasOpacityMask)
					{
						OutStats.MaskedMaterialBakeReferences += MatchingTriangleCount;
					}

					Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
					Storage->MeshSettings.Mesh = &SourceStaticMesh;
					Storage->MeshSettings.MaterialIndices.Add(0);
					Storage->MeshSettings.TextureCoordinateBox =
						FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
					Storage->MeshSettings.TextureCoordinateIndex = 0;
					Storage->MeshSettings.LightMapIndex = 0;
					Storage->MeshSettings.PrimitiveData = FPrimitiveData(SourceLODBounds);
					Storage->MeshSettings.CustomTextureCoordinates = MoveTemp(Storage->CustomTileUVs);
					FDepthCorrectMaterialStorage* StoragePtr = Storage.Get();
					MaterialStorage.Add(MoveTemp(Storage));

					FFoliageBakerDepthCorrectTileMaterialInput& MaterialInput =
						DepthCorrectRequest.Materials.AddDefaulted_GetRef();
					MaterialInput.MaterialInterface = StoragePtr->MaterialInterface;
					MaterialInput.MeshSettings = &StoragePtr->MeshSettings;
					MaterialInput.RasterSourceTriangleIndices =
						&StoragePtr->RasterSourceTriangleIndices;
				}

				if (DepthCorrectRequest.Materials.IsEmpty())
				{
					return true;
				}

				FFoliageBakerDepthCorrectTileResult DepthCorrectResult;
				FString DepthCorrectError;
				if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
						DepthCorrectRequest,
						DepthCorrectResult,
						&DepthCorrectError))
				{
					OutError = FString::Printf(
						TEXT("BillboardClouds depth-correct tile bake failed for plane %d (%s): %s"),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						*DepthCorrectError);
					return false;
				}
				if (DepthCorrectResult.SourceTriangleIdAndDepth.Num() != TilePixelCount
					|| (OutputSelection.bBaseColorOpacity
						&& DepthCorrectResult.BaseColor.Num() != TilePixelCount)
					|| (OutputSelection.bNormalMask
						&& DepthCorrectResult.ObjectSpaceNormal.Num() != TilePixelCount)
					|| (OutputSelection.bMix
						&& DepthCorrectResult.PackedMix.Num() != TilePixelCount)
					|| (OutputSelection.bMaterialScalarAverages
						&& (DepthCorrectResult.Roughness.Num() != TilePixelCount
							|| DepthCorrectResult.Specular.Num() != TilePixelCount)))
				{
					OutError = FString::Printf(
						TEXT("BillboardClouds depth-correct tile returned invalid sizes for plane %d (%s): base=%d, id=%d, normal=%d, mix=%d, roughness=%d, specular=%d, expected=%d."),
						PlaneInfo.SourcePlaneIndex,
						bBackSide ? TEXT("back") : TEXT("front"),
						DepthCorrectResult.BaseColor.Num(),
						DepthCorrectResult.SourceTriangleIdAndDepth.Num(),
						DepthCorrectResult.ObjectSpaceNormal.Num(),
						DepthCorrectResult.PackedMix.Num(),
						DepthCorrectResult.Roughness.Num(),
						DepthCorrectResult.Specular.Num(),
						TilePixelCount);
					return false;
				}

				for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
				{
					const int32 AtlasY = TilePixelMin.Y + LocalY;
					if (AtlasY < 0 || AtlasY >= OutStats.Height)
					{
						continue;
					}
					for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
					{
						const int32 AtlasX = TilePixelMin.X + LocalX;
						if (AtlasX < 0 || AtlasX >= OutStats.Width)
						{
							continue;
						}
						const int32 TilePixelIndex = LocalY * TileSize.X + LocalX;
						const int32 SourceTriangleIndex =
							FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
								DepthCorrectResult.SourceTriangleIdAndDepth[TilePixelIndex]);
						if (SourceTriangleIndex == INDEX_NONE)
						{
							continue;
						}
						if (!Triangles.IsValidIndex(SourceTriangleIndex))
						{
							OutError = FString::Printf(
								TEXT("BillboardClouds depth-correct tile decoded invalid triangle %d at pixel (%d,%d) for plane %d (%s)."),
								SourceTriangleIndex,
								LocalX,
								LocalY,
								PlaneInfo.SourcePlaneIndex,
								bBackSide ? TEXT("back") : TEXT("front"));
							return false;
						}

						const int32 AtlasPixelIndex = AtlasY * OutStats.Width + AtlasX;
						if (OutputSelection.bMaterialScalarAverages)
						{
							OutStats.MaterialAverages.AddSample(
								Triangles[SourceTriangleIndex].bIsTrunk,
								DepthCorrectResult.Roughness[TilePixelIndex].R,
								DepthCorrectResult.Specular[TilePixelIndex].R);
						}

						if (OutputSelection.bBaseColorOpacity)
						{
							FColor Color = DepthCorrectResult.BaseColor[TilePixelIndex];
							Color.A = UE::FoliageBaker::Atlas::EncodeTrunkLeafAlpha(
								Triangles[SourceTriangleIndex].bIsTrunk);
							OutPixels[AtlasPixelIndex] = Color;
						}
						if (AtlasCoverage.IsValidIndex(AtlasPixelIndex))
						{
							AtlasCoverage[AtlasPixelIndex] = true;
						}

						if (OutputSelection.bNormalMask)
						{
							FColor ObjectNormal = DepthCorrectResult.ObjectSpaceNormal[TilePixelIndex];
							ObjectNormal.A = DepthCorrectResult.SourceTriangleIdAndDepth[TilePixelIndex].A;
							OutNormalPixels[AtlasPixelIndex] = ObjectNormal;
							if (NormalCoverage.IsValidIndex(AtlasPixelIndex))
							{
								NormalCoverage[AtlasPixelIndex] = true;
							}
						}

						if (OutputSelection.bMix)
						{
							OutMixPixels[AtlasPixelIndex] = DepthCorrectResult.PackedMix[TilePixelIndex];
						}
						++OutStats.PaintedPixels;
					}
				}
				return true;
			}

		};

		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			if (FMath::IsNearlyEqual(PlaneInfo.MaxU, PlaneInfo.MinU)
				|| FMath::IsNearlyEqual(PlaneInfo.MaxV, PlaneInfo.MinV))
			{
				continue;
			}
			if (!BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.AtlasPixelMin,
					PlaneInfo.AtlasTileSize,
					-PlaneInfo.Normal,
					false))
			{
				return false;
			}
			if (PlaneInfo.bHasBackFaceAtlas
				&& !BakePlaneAndSide(
					PlaneInfo,
					PlaneInfo.BackAtlasPixelMin,
					PlaneInfo.BackAtlasTileSize,
					PlaneInfo.Normal,
					true))
			{
				return false;
			}
		}

		UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
			OutPixels,
			OutStats.Width,
			OutStats.Height,
			PlaneInfos);
		if (OutputSelection.bNormalMask)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				OutNormalPixels,
				OutStats.Width,
				OutStats.Height,
				PlaneInfos,
				&NormalCoverage,
				false);
			for (int32 PixelIndex = 0; PixelIndex < OutNormalPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage.IsValidIndex(PixelIndex) || !NormalCoverage[PixelIndex])
				{
					OutNormalPixels[PixelIndex].A = 255;
				}
			}
			UE::FoliageBaker::Atlas::NormalizeEncodedObjectSpaceNormals(OutNormalPixels);
		}
		if (OutputSelection.bMix)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				OutMixPixels,
				OutStats.Width,
				OutStats.Height,
				PlaneInfos,
				&AtlasCoverage,
				true);
		}
		return true;
	}
	UTexture2D* CreateBillboardTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FString& OutputFolderName,
		const FString& OutputPackagePathOverride,
		const FString& AssetNamePrefix,
		const FString& AssetNameSuffix,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		const FColor MipBackgroundColor,
		const TextureCompressionSettings CompressionSettings,
		const TextureGroup LODGroup,
		const bool bSRGB,
		const float SemanticMaskMipCoverageThreshold,
		const FString& EmptyPixelsError,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = OutputFolderName;
		Params.OutputPackagePathOverride = OutputPackagePathOverride;
		Params.AssetNamePrefix = AssetNamePrefix;
		Params.AssetNameSuffix = AssetNameSuffix;
		Params.Width = AtlasStats.Width;
		Params.Height = AtlasStats.Height;
		Params.CompressionSettings = CompressionSettings;
		Params.LODGroup = LODGroup;
		Params.bSRGB = bSRGB;
		Params.SemanticMaskMipCoverageThreshold = SemanticMaskMipCoverageThreshold;
		Params.MipBackgroundColor = MipBackgroundColor;
		Params.bNormalizeMipNormals = LODGroup == TEXTUREGROUP_WorldNormalMap;
		for (const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			Params.MipTileRects.Add(FIntRect(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasPixelMin + PlaneInfo.AtlasTileSize));
			if (PlaneInfo.bHasBackFaceAtlas)
			{
				Params.MipTileRects.Add(FIntRect(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasPixelMin + PlaneInfo.BackAtlasTileSize));
			}
		}
		Params.EmptyPixelsError = EmptyPixelsError;
		return FFoliageBakerAssetBuilder::CreateTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			Params,
			Pixels,
			OutError);
	}

	UTexture2D* CreateAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			OutputPackagePathOverride,
			EditorSettings.TextureNamePrefix,
			EditorSettings.BaseColorOpacityTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(0, 0, 0, 0),
			TC_BC7,
			TEXTUREGROUP_World,
			true,
			EditorSettings.bPreserveAlphaMaskValues
				? FMath::Clamp(EditorSettings.MipMaskCoverageThreshold, 0.01f, 1.0f)
				: 0.0f,
			TEXT("No atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateNormalAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			OutputPackagePathOverride,
			EditorSettings.TextureNamePrefix,
			EditorSettings.NormalTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(128, 128, 255, 255),
			TC_BC7,
			TEXTUREGROUP_WorldNormalMap,
			false,
			0.0f,
			TEXT("No normal atlas pixels were generated."),
			OutError);
	}

	UTexture2D* CreateMixAtlasTextureAsset(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FString& OutputPackagePathOverride,
		const TArray<FColor>& Pixels,
		const FAtlasBakeStats& AtlasStats,
		const TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
		FString& OutError)
	{
		return CreateBillboardTextureAsset(
			SourceStaticMesh,
			AssetTransaction,
			EditorSettings.TextureOutputFolderName,
			OutputPackagePathOverride,
			EditorSettings.TextureNamePrefix,
			EditorSettings.MixTextureSuffix,
			Pixels,
			AtlasStats,
			PlaneInfos,
			FColor(255, 128, 0, 0),
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			0.0f,
			TEXT("No mix atlas pixels were generated."),
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

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const UFoliageBakerBillboardCloudsSettings& Settings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = MeshOutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = MeshOutputSelection.ReplaceLODIndex;
		Params.RequestedInsertAfterLODIndex = MeshOutputSelection.InsertAfterLODIndex;
		Params.SourceLODIndex = Settings.SourceLODIndex;
		Params.DesiredUVChannelCount = 3;

		Params.RebuildLODMetadataKey = FName(TEXT("FoliageBaker.BillboardCloudsLOD"));
		return Params;
	}

	struct FProxyPlaneCoverBuildData
	{
		int32 SourceLODIndex = INDEX_NONE;
		FBoxSphereBounds SourceLODBounds = FBoxSphereBounds(ForceInitToZero);
		TArray<UE::FoliageBaker::PlaneCover::FSourceTriangle> Triangles;
		UE::FoliageBaker::PlaneCover::FPlaneProxySettings Settings;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverSettings KMeansSettings;
		FTrunkCardTriangleSplit TrunkSplit;
		UE::FoliageBaker::BillboardClouds::FKMeansPlaneCoverResult BillboardResult;
		UE::FoliageBaker::PlaneCover::FPlaneProxySet ProxyResult;
		bool bHasBillboardResult = false;
		int32 TrunkPlaneCount = 0;
	};

	struct FProxyMeshBuildData
	{
		FMeshDescription MeshDescription;
		UE::FoliageBaker::PlaneCover::FPlaneProxyMeshStats Stats;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> PlaneInfos;
	};

	struct FProxyTextureBuildData
	{
		FAtlasOutputSelection OutputSelection;
		TArray<FColor> AtlasPixels;
		TArray<FColor> NormalAtlasPixels;
		TArray<FColor> MixAtlasPixels;
		FAtlasBakeStats AtlasStats;
		UTexture2D* AtlasTexture = nullptr;
		UTexture2D* NormalAtlasTexture = nullptr;
		UTexture2D* MixAtlasTexture = nullptr;
		UMaterialInstanceConstant* Material = nullptr;
	};

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
		UMaterialInstanceConstant* Material = nullptr;
	};

	FProxyAssetBuildResult MakeProxyBuildFailure(const UStaticMesh& StaticMesh, const FString& Error)
	{
		FProxyAssetBuildResult Result;
		const FString MeshName = StaticMesh.GetName();
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *MeshName, *Error);
		UE_LOG(LogFoliageBakerBillboardClouds, Warning, TEXT("%s"), *Result.Report);
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

	FString BuildBillboardCloudsOrTrunkSummary(
		const UStaticMesh& StaticMesh,
		const FProxyPlaneCoverBuildData& CoverData)
	{
		FString Summary;
		if (CoverData.bHasBillboardResult)
		{
			Summary = UE::FoliageBaker::BillboardClouds::SummarizeKMeansPlaneCover(
				StaticMesh.GetName(),
				CoverData.KMeansSettings,
				CoverData.Settings,
				CoverData.BillboardResult);
		}
		else
		{
			Summary = FString::Printf(
				TEXT("%s\n  algorithm: Billboard Clouds skipped; all matched triangles are routed to fixed trunk cross-card planes"),
				*StaticMesh.GetName());
		}

		Summary += BuildTrunkCrossCardSummary(CoverData.TrunkSplit, CoverData.TrunkPlaneCount, CoverData.Settings.TrunkCardAtlasScale);
		return Summary;
	}

	const TCHAR* GetTextureShootingMode()
	{
		return TEXT("GPU bake, per-plane tile UV, K-Means cluster projection");
	}

	FAtlasOutputSelection BuildAtlasOutputSelection(const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		FAtlasOutputSelection OutputSelection;
		OutputSelection.bBaseColorOpacity = EditorSettings.bBakeBaseColorOpacityAtlas;
		OutputSelection.bNormalMask = EditorSettings.bBakeNormalMaskAtlas;
		OutputSelection.bMix = EditorSettings.bBakeMixAtlas;
		OutputSelection.bMaterialScalarAverages = !EditorSettings.bBakeMixAtlas;
		return OutputSelection;
	}

	bool BuildProxyPlaneCoverData(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FProxyPlaneCoverBuildData& OutData,
		FString& OutError)
	{
		OutData.SourceLODIndex = EditorSettings.SourceLODIndex;
		if (OutData.SourceLODIndex < 0 || OutData.SourceLODIndex >= MAX_STATIC_MESH_LODS)
		{
			OutError = FString::Printf(
				TEXT("Source LOD index %d is outside the supported range 0-%d."),
				OutData.SourceLODIndex,
				MAX_STATIC_MESH_LODS - 1);
			return false;
		}
		if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&StaticMesh,
			OutData.SourceLODIndex,
			OutData.Triangles,
			OutError))
		{
			return false;
		}
		if (!ComputeSourceTriangleBounds(OutData.Triangles, OutData.SourceLODBounds))
		{
			OutError = FString::Printf(TEXT("Source LOD %d has no valid bounds."), OutData.SourceLODIndex);
			return false;
		}

		OutData.Settings = BuildSettingsForMesh(OutData.SourceLODBounds, EditorSettings);
		OutData.KMeansSettings = BuildKMeansSettings(EditorSettings);
		const int32 RequestedTrunkPlaneCount = FMath::Clamp(EditorSettings.TrunkCardPlaneCount, 2, 8);
		OutData.TrunkSplit = SplitTrianglesForTrunkCards(StaticMesh, OutData.Triangles, EditorSettings.bEnableTrunkCards, EditorSettings.TrunkCardMaterialKeywords);

		if (!OutData.TrunkSplit.BillboardTriangles.IsEmpty())
		{
			OutData.BillboardResult = UE::FoliageBaker::BillboardClouds::BuildKMeansPlaneCover(
				OutData.TrunkSplit.BillboardTriangles,
				OutData.KMeansSettings);
			OutData.ProxyResult = RemapPlaneCoverResultToSourceTriangles(OutData.BillboardResult, OutData.TrunkSplit.BillboardToSourceTriangleIndices, OutData.Triangles.Num());
			OutData.bHasBillboardResult = true;
		}
		else
		{
			OutData.ProxyResult.SourceTriangleCount = OutData.Triangles.Num();
		}

		OutData.TrunkPlaneCount = AppendTrunkCrossCardPlanes(OutData.Triangles, OutData.TrunkSplit.TrunkTriangleIndices, RequestedTrunkPlaneCount, OutData.ProxyResult);
		if (OutData.ProxyResult.Planes.IsEmpty())
		{
			OutError = TEXT("no Billboard Clouds planes or trunk card planes were generated.");
			return false;
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

	bool BuildProxyTextureData(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyPlaneCoverBuildData& CoverData,
		FProxyMeshBuildData& MeshData,
		const FFoliageBakerGeneratedAssetOutputFolders& OutputFolders,
		FProxyTextureBuildData& OutData,
		FString& OutError)
	{
		OutData.OutputSelection = BuildAtlasOutputSelection(EditorSettings);
		if (!OutData.OutputSelection.HasAnyOutput())
		{
			OutError = TEXT("No atlas outputs selected. Enable BaseColor/Opacity, Normal/Depth, or Mix in the Billboard Clouds tool panel.");
			return false;
		}

		TSet<FName> EnabledTextureParameterNames;
		auto ValidateTextureParameterName = [&EnabledTextureParameterNames, &OutError](bool bOutputEnabled, FName ParameterName, const TCHAR* OutputLabel) -> bool
		{
			if (!bOutputEnabled)
			{
				return true;
			}
			if (ParameterName.IsNone())
			{
				OutError = FString::Printf(TEXT("The %s texture output is enabled, but its material texture parameter name is None."), OutputLabel);
				return false;
			}
			if (EnabledTextureParameterNames.Contains(ParameterName))
			{
				OutError = FString::Printf(TEXT("Material texture parameter name '%s' is assigned to more than one enabled texture output."), *ParameterName.ToString());
				return false;
			}
			EnabledTextureParameterNames.Add(ParameterName);
			return true;
		};
		if (!ValidateTextureParameterName(OutData.OutputSelection.bBaseColorOpacity, EditorSettings.BaseColorOpacityTextureParameterName, TEXT("BaseColor/Opacity"))
			|| !ValidateTextureParameterName(OutData.OutputSelection.bNormalMask, EditorSettings.NormalDepthTextureParameterName, TEXT("Normal/Depth"))
			|| !ValidateTextureParameterName(OutData.OutputSelection.bMix, EditorSettings.MixTextureParameterName, TEXT("Mix")))
		{
			return false;
		}
		UMaterialInstanceConstant* TemplateMaterialInstance =
			EditorSettings.BillboardMaterialTemplate.LoadSynchronous();
		if (!TemplateMaterialInstance)
		{
			OutError = TEXT("Billboard Clouds Parent Material Instance is not selected in the current tool settings.");
			return false;
		}

		int32 AlphaAwareCroppedPlaneCount = 0;
		if (CoverData.Settings.bEnableAlphaAwareTileCrop && !MeshData.PlaneInfos.IsEmpty())
		{
			FAtlasOutputSelection CropOutputSelection;
			CropOutputSelection.bBaseColorOpacity = true;

			TArray<FColor> CropAtlasPixels;
			TArray<FColor> CropNormalPixels;
			TArray<FColor> CropMixPixels;
			FAtlasBakeStats CropStats;
			if (!BakeBillboardAtlasGPU(
				StaticMesh,
				CoverData.SourceLODBounds,
				CoverData.Triangles,
				MeshData.PlaneInfos,
				MeshData.Stats,
				CoverData.Settings,
				CropOutputSelection,
				CropAtlasPixels,
				CropNormalPixels,
				CropMixPixels,
				CropStats,
				OutError))
			{
				return false;
			}

			TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyTileCrop> TileCrops;
			AlphaAwareCroppedPlaneCount = UE::FoliageBaker::Atlas::BuildAlphaAwareTileCrops(
				CropAtlasPixels,
				CropStats.Width,
				CropStats.Height,
				MeshData.PlaneInfos,
				CoverData.Settings.AlphaAwareTileCropGuardPixels,
				1,
				TileCrops);

			if (AlphaAwareCroppedPlaneCount > 0)
			{
				if (!UE::FoliageBaker::PlaneCover::ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
					MeshData.PlaneInfos,
					TileCrops,
					CoverData.Settings,
					MeshData.MeshDescription,
					MeshData.Stats,
					OutError))
				{
					return false;
				}
			}
		}

		if (!BakeBillboardAtlasGPU(
			StaticMesh,
			CoverData.SourceLODBounds,
			CoverData.Triangles,
			MeshData.PlaneInfos,
			MeshData.Stats,
			CoverData.Settings,
			OutData.OutputSelection,
			OutData.AtlasPixels,
			OutData.NormalAtlasPixels,
			OutData.MixAtlasPixels,
			OutData.AtlasStats,
			OutError))
		{
			return false;
		}
		OutData.AtlasStats.AlphaAwareCroppedPlanes = AlphaAwareCroppedPlaneCount;
		OutData.AtlasStats.AlphaAwareTileCropGuardPixels = CoverData.Settings.bEnableAlphaAwareTileCrop
			? CoverData.Settings.AlphaAwareTileCropGuardPixels
			: 0;

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
		MaterialParams.MissingTemplateError = TEXT("Billboard Clouds Parent Material Instance is not selected in the current tool settings.");
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
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		FFoliageBakerAssetTransaction& AssetTransaction,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection,
		FProxyAssetBuildResult& OutResult,
		FString& OutError)
	{
		OutResult.MeshOutputMode = MeshOutputSelection.OutputMode;

		if (MeshOutputSelection.OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
		{
			FFoliageBakerStaticMeshAssetParams MeshParams;
			MeshParams.AssetNameSuffix = TEXT("_BillboardCloudProxy");
			MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
			MeshParams.DesiredUVChannelCount = 3;
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
			if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				StaticMesh,
				AssetTransaction,
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection),
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
		OutResult.Material = TextureData.Material;
		return true;
	}

	FString BuildProxySuccessReport(
		const UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings,
		const FProxyPlaneCoverBuildData& CoverData,
		const FProxyMeshBuildData& MeshData,
		const FProxyTextureBuildData& TextureData,
		const FProxyAssetBuildResult& AssetResult)
	{
		const FString AlphaPolicyDetails = TextureData.AtlasStats.MaterialAlphaPolicyDetails.IsEmpty()
			? FString()
			: FString::Printf(TEXT("\n  alpha policy:%s"), *TextureData.AtlasStats.MaterialAlphaPolicyDetails);

		const FString TechniqueSummary = FString::Printf(
			TEXT("%s\n  source LOD: %d, selected-LOD bounds radius: %.3f cm"),
			*BuildBillboardCloudsOrTrunkSummary(StaticMesh, CoverData),
			CoverData.SourceLODIndex,
			CoverData.SourceLODBounds.SphereRadius);
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
			TEXT("BaseColor/Opacity=%s, Normal/Depth=%s, Mix=%s"),
			*EditorSettings.BaseColorOpacityTextureParameterName.ToString(),
			*EditorSettings.NormalDepthTextureParameterName.ToString(),
			*EditorSettings.MixTextureParameterName.ToString());
		const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
			MaterialScalarParameterNames = {
				EditorSettings.LeafRoughnessParameterName,
				EditorSettings.LeafSpecularParameterName,
				EditorSettings.TrunkRoughnessParameterName,
				EditorSettings.TrunkSpecularParameterName,
			};
		const FString MaterialScalarDetails =
			UE::FoliageBaker::MaterialResolver::BuildTrunkLeafMaterialAveragesReport(
				!EditorSettings.bBakeMixAtlas,
				TextureData.AtlasStats.MaterialAverages,
				MaterialScalarParameterNames);

		return FString::Printf(
			TEXT("%s%s\n  mesh output: %s\n  proxy planes: %d, quads: %d, triangles: %d\n  atlas size: %dx%d, largest tile=%d, tile fill=automatic nearest covered pixel, packed tile usage=%.1f%%, front tiles=%d, back tiles=%d, painted pixels=%d, alpha-aware cropped planes=%d, crop guard=%d px, rasterized refs=%d, crack-reduction refs=%d, masked refs=%d, shooting=%s, resolve=shared per-tile RDG masked depth; primary and crack-reduction geometry compete in the same depth target\n  base/color opacity atlas: %s, RGB=BaseColor, A=background 0, trunk 0.5 (128), leaf 1 (255)\n  normal/depth atlas: %s, RGB=object/local-space normal, A=shared selected-source-LOD bounds linear depth (near 1, far 0, uncovered 1); WPO disabled during material baking\n  mix atlas: %s, RGBA=Occlusion/Roughness/Metallic/Emission, linear masks from the same GPU depth winner\n  material scalar averages: %s\n  trunk/leaf classification: ColorOpacity.A and UV2, trunk alpha=0.5 (128), leaf alpha=1 (255), UV2 trunk=(0,0), billboard/leaf=(1,0)\n  atlas UVs: UV0 front-side tile, UV1 back-side tile; UV1 mirrors UV0 when double-sided bake is off for that plane\n  material instance: %s (child of the Editor Preferences parent; texture parameters: %s)\n  normal bake input triangles: %d / %d\n  proxy normal avg dot(plane, shading): %.3f, angle: %.1f deg\n  proxy build: %s, recompute normals/tangents off, collision generation off, lightmap UV generation off, distance fields on\n  proxy winding: reversed UE front-face order, source-facing normals"),
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
			TextureData.AtlasStats.CrackReductionTriangleReferences,
			TextureData.AtlasStats.MaskedMaterialBakeReferences,
			GetTextureShootingMode(),
			*BaseAtlasPath,
			*NormalAtlasPath,
			*MixAtlasPath,
			*MaterialScalarDetails,
			*TextureData.Material->GetPathName(),
			*MaterialParameterDetails,
			MeshData.Stats.SourceShadingNormalTriangleCount,
			MeshData.Stats.SourceTriangleCount,
			MeshData.Stats.AveragePlaneToShadingNormalDot,
			MeshData.Stats.AveragePlaneToShadingNormalAngleDegrees,
			MeshBuildPathDetails);
	}

	FProxyAssetBuildResult BuildBillboardCloudProxyAsset(
		UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
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
				BuildSourceLODAssetParams(EditorSettings, MeshOutputSelection.GetValue()),
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

		FProxyAssetBuildResult Result;
		if (!CreateProxyMeshAssetBundle(
				StaticMesh,
				EditorSettings,
				AssetTransaction,
				MeshData,
				TextureData,
				MeshOutputSelection.GetValue(),
				Result,
				Error))
		{
			return MakeProxyBuildFailure(StaticMesh, Error);
		}
		AssetTransaction.Commit();

		Result.bSucceeded = true;
		Result.Report = BuildProxySuccessReport(StaticMesh, EditorSettings, CoverData, MeshData, TextureData, Result);
		UE_LOG(LogFoliageBakerBillboardClouds, Display, TEXT("\n%s"), *Result.Report);
		return Result;
	}

	void AppendProxyCreatedAssets(const FProxyAssetBuildResult& BuildResult, TArray<UObject*>& OutCreatedAssets)
	{
		if (BuildResult.ProxyMesh)
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
		if (BuildResult.Material)
		{
			OutCreatedAssets.Add(BuildResult.Material);
		}
	}

	FFoliageBakerFeatureBakeItemResult BuildBillboardCloudProxyFeatureItem(
		UStaticMesh& StaticMesh,
		const UFoliageBakerBillboardCloudsSettings& EditorSettings)
	{
		const FProxyAssetBuildResult BuildResult =
			BuildBillboardCloudProxyAsset(StaticMesh, EditorSettings);
		FFoliageBakerFeatureBakeItemResult ItemResult;
		ItemResult.bSucceeded = BuildResult.bSucceeded;
		ItemResult.bCancelled = BuildResult.bCancelled;
		ItemResult.Report = BuildResult.Report;
		if (BuildResult.bSucceeded)
		{
			AppendProxyCreatedAssets(BuildResult, ItemResult.CreatedAssets);
		}
		return ItemResult;
	}
}

void FFoliageBakerBillboardCloudsModule::StartupModule()
{
	EnsureToolSettings();
}

void FFoliageBakerBillboardCloudsModule::ShutdownModule()
{
	FeatureController.Reset();
	ToolSettings.Reset();
}

void FFoliageBakerBillboardCloudsModule::EnsureToolSettings()
{
	if (!ToolSettings.IsValid())
	{
		FFoliageBakerFeatureTool::EnsureTransientSettings(ToolSettings);
	}
}

TSharedRef<SWidget> FFoliageBakerBillboardCloudsModule::CreateFeaturePanel()
{
	EnsureToolSettings();
	FFoliageBakerFeatureControllerArgs ControllerArgs;
	ControllerArgs.SettingsObject = ToolSettings.Get();
	ControllerArgs.SourceStaticMeshes = &ToolSettings->SourceStaticMeshes;
	ControllerArgs.BakeButtonText =
		LOCTEXT("UnifiedBakeBillboardCloudsButton", "Bake BillboardClouds");
	ControllerArgs.BakeButtonTooltip = LOCTEXT(
		"UnifiedBakeBillboardCloudsTooltip",
		"Bake BillboardClouds assets for every queued Static Mesh.");
	ControllerArgs.RequirementsHint = LOCTEXT(
		"UnifiedBakeRequirementsHint",
		"Select the Parent Material Instance, enable an atlas output, and queue at least one Static Mesh. Editor Preferences provides the initial default.");
	ControllerArgs.AddMeshesTransactionText = LOCTEXT(
		"AddBillboardCloudsSourceMeshesTransaction",
		"Add Foliage Baker BillboardClouds Source Meshes");
	ControllerArgs.ClearMeshesTransactionText = LOCTEXT(
		"ClearBillboardCloudsSourceMeshesTransaction",
		"Clear Foliage Baker BillboardClouds Source Meshes");
	ControllerArgs.bShowDetailsOptions = false;
	ControllerArgs.bShowPropertyMatrixButton = false;
	ControllerArgs.CanBake =
		FFoliageBakerFeaturePredicateDelegate::CreateRaw(
			this,
			&FFoliageBakerBillboardCloudsModule::CanBake);
	ControllerArgs.Bake =
		FFoliageBakerFeatureActionDelegate::CreateRaw(
			this,
			&FFoliageBakerBillboardCloudsModule::Bake);
	FeatureController = FFoliageBakerFeatureController::Create(ControllerArgs);
	return FeatureController->GetWidget();
}

bool FFoliageBakerBillboardCloudsModule::CanBake() const
{
	if (!ToolSettings.IsValid())
	{
		return false;
	}
	return FFoliageBakerFeatureTool::CanBakeFeature(
		!ToolSettings->BillboardMaterialTemplate.IsNull(),
		BuildAtlasOutputSelection(*ToolSettings).HasAnyOutput(),
		ToolSettings->SourceStaticMeshes);
}

void FFoliageBakerBillboardCloudsModule::Bake()
{
	EnsureToolSettings();
	if (ToolSettings->BillboardMaterialTemplate.IsNull()
		|| !ToolSettings->BillboardMaterialTemplate.LoadSynchronous())
	{
		FFoliageBakerFeatureTool::ShowMessage(
			LOCTEXT("MissingBillboardCloudsParentMaterial", "Select the Billboard Clouds Parent Material Instance in the current tool before baking."));
		return;
	}
	if (!BuildAtlasOutputSelection(*ToolSettings).HasAnyOutput())
	{
		FFoliageBakerFeatureTool::ShowMessage(
			LOCTEXT("NoBillboardCloudsAtlasOutputs", "Enable at least one Billboard Clouds atlas output before baking."));
		return;
	}
	if (!FFoliageBakerFeatureTool::HasAnyValidStaticMesh(
			ToolSettings->SourceStaticMeshes))
	{
		FFoliageBakerFeatureTool::ShowMessage(LOCTEXT(
			"NoStaticMeshSelectionForProxy",
			"Add one or more Static Mesh assets to the Billboard Clouds tool panel before clicking Bake."));
		return;
	}

	const FFoliageBakerFeatureBatchResult BatchResult =
		FFoliageBakerFeatureTool::RunBakeBatch(
			ToolSettings->SourceStaticMeshes,
			LOCTEXT(
				"CreatePlaneProxyMeshesSlowTask",
				"Creating Billboard Clouds plane proxy meshes..."),
			false,
			TEXT("\n\n"),
			FFoliageBakerBakeStaticMeshDelegate::CreateLambda(
				[this](UStaticMesh& StaticMesh)
				{
					return BuildBillboardCloudProxyFeatureItem(
						StaticMesh,
						*ToolSettings);
				}));
	FFoliageBakerFeatureTool::SyncCreatedAssetsToContentBrowser(BatchResult.CreatedAssets);
	FFoliageBakerFeatureTool::ShowMessage(FText::FromString(BatchResult.Report));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFoliageBakerBillboardCloudsModule, FoliageBakerBillboardClouds)
