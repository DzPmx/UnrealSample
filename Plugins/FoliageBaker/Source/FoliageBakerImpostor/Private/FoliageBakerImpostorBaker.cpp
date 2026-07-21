#include "FoliageBakerImpostorBaker.h"

#include "FoliageBakerAssetBuilder.h"
#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerImpostorSettings.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerMaterialResolver.h"
#include "FoliageBakerMeshOutputDialog.h"
#include "FoliageBakerPlaneCover.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "Containers/Set.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialBakingStructures.h"
#include "MaterialShared.h"
#include "Math/RotationMatrix.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageBakerImpostor, Log, All);

namespace
{
	using FSourceTriangle = UE::FoliageBaker::PlaneCover::FSourceTriangle;
	constexpr int32 ImpostorProjectionGuardPixels = 2;

	struct FImpostorCaptureView
	{
		FVector ViewDirection = FVector::ForwardVector;
		FVector CaptureRayDirection = -FVector::ForwardVector;
		FVector AxisU = FVector::RightVector;
		FVector AxisV = FVector::UpVector;
		FIntPoint TilePixelMin = FIntPoint::ZeroValue;
		FIntPoint TileSize = FIntPoint::ZeroValue;
		double ProjectionHalfExtentU = 1.0;
		double ProjectionHalfExtentV = 1.0;
		FIntPoint GridIndex = FIntPoint::ZeroValue;
	};

	struct FImpostorBakeStats
	{
		int32 AtlasWidth = 0;
		int32 AtlasHeight = 0;
		int32 TileResolution = 0;
		int32 ViewCount = 0;
		int32 PaintedPixels = 0;
		int32 RasterizedTriangleReferences = 0;
		int32 MaskedTriangleReferences = 0;
		int32 DepthCorrectTileCount = 0;
		UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialAverages MaterialAverages;
	};

	struct FImpostorBakeData
	{
		FBoxSphereBounds SourceBounds = FBoxSphereBounds(ForceInitToZero);
		double SharedCaptureHalfExtent = 1.0;
		TArray<FVector> SourceVertices;
		TArray<FSourceTriangle> Triangles;
		TArray<FImpostorCaptureView> Views;
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo> TileInfos;
		TArray<FColor> BaseColorPixels;
		TArray<FColor> NormalDepthPixels;
		TArray<FColor> MixPixels;
		TArray<float> CoverageValues;
		FImpostorBakeStats Stats;
	};

	bool ComputeSourceBounds(
		const TArray<FSourceTriangle>& Triangles,
		TArray<FVector>& OutVertices,
		FBoxSphereBounds& OutBounds)
	{
		TSet<FVector> UniqueVertices;
		UniqueVertices.Reserve(Triangles.Num());
		for (const FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				UniqueVertices.Add(Vertex);
			}
		}
		if (UniqueVertices.Num() == 0)
		{
			return false;
		}
		OutVertices.Reset(UniqueVertices.Num());
		for (const FVector& Vertex : UniqueVertices)
		{
			OutVertices.Add(Vertex);
		}
		OutBounds = FBoxSphereBounds(OutVertices.GetData(), static_cast<uint32>(OutVertices.Num()));
		return true;
	}

	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	FVector DecodeObjectSpaceNormal(const FColor& EncodedNormal)
	{
		return FVector(
			static_cast<double>(EncodedNormal.R) / 255.0 * 2.0 - 1.0,
			static_cast<double>(EncodedNormal.G) / 255.0 * 2.0 - 1.0,
			static_cast<double>(EncodedNormal.B) / 255.0 * 2.0 - 1.0)
			.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	}

	FColor EncodeOctahedralObjectSpaceNormal(
		const FVector& InNormal,
		const uint8 TrunkLeafMask,
		const uint8 Depth)
	{
		const FVector Normal = InNormal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const double L1Norm = FMath::Abs(Normal.X)
			+ FMath::Abs(Normal.Y)
			+ FMath::Abs(Normal.Z);
		const FVector Projected = Normal / FMath::Max(L1Norm, UE_DOUBLE_SMALL_NUMBER);
		FVector2D Octahedral(Projected.X, Projected.Y);
		if (Projected.Z < 0.0)
		{
			const double OldX = Octahedral.X;
			Octahedral.X = (1.0 - FMath::Abs(Octahedral.Y))
				* (OldX >= 0.0 ? 1.0 : -1.0);
			Octahedral.Y = (1.0 - FMath::Abs(OldX))
				* (Octahedral.Y >= 0.0 ? 1.0 : -1.0);
		}
		return FColor(
			UnitFloatToByte(static_cast<float>(Octahedral.X * 0.5 + 0.5)),
			UnitFloatToByte(static_cast<float>(Octahedral.Y * 0.5 + 0.5)),
			TrunkLeafMask,
			Depth);
	}

	FVector DecodeHemiOctahedralDirection(const FVector2D& Encoded)
	{
		const FVector2D Octahedron(
			(Encoded.X + Encoded.Y) * 0.5,
			(Encoded.X - Encoded.Y) * 0.5);
		return FVector(
			Octahedron.X,
			Octahedron.Y,
			1.0 - FMath::Abs(Octahedron.X) - FMath::Abs(Octahedron.Y)).GetSafeNormal();
	}

	FVector DecodeFullOctahedralDirection(const FVector2D& Encoded)
	{
		FVector Direction(
			Encoded.X,
			Encoded.Y,
			1.0 - FMath::Abs(Encoded.X) - FMath::Abs(Encoded.Y));
		if (Direction.Z < 0.0)
		{
			const double OldX = Direction.X;
			Direction.X = (1.0 - FMath::Abs(Direction.Y)) * (OldX >= 0.0 ? 1.0 : -1.0);
			Direction.Y = (1.0 - FMath::Abs(OldX)) * (Direction.Y >= 0.0 ? 1.0 : -1.0);
		}
		return Direction.GetSafeNormal();
	}

	double ComputeSharedCaptureHalfExtent(
		const TArray<FVector>& SourceVertices,
		const FBoxSphereBounds& SourceBounds,
		const TArray<FImpostorCaptureView>& Views,
		const int32 TileResolution)
	{
		double UnpaddedHalfExtent = 0.0;
		for (const FVector& Vertex : SourceVertices)
		{
			const FVector LocalPosition = Vertex - SourceBounds.Origin;
			for (const FImpostorCaptureView& View : Views)
			{
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.AxisU)));
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.AxisV)));
				UnpaddedHalfExtent = FMath::Max(
					UnpaddedHalfExtent,
					FMath::Abs(FVector::DotProduct(LocalPosition, View.CaptureRayDirection)));
			}
		}

		const int32 UsableTileResolution = FMath::Max(
			TileResolution - ImpostorProjectionGuardPixels * 2,
			1);
		const double GuardScale = static_cast<double>(TileResolution)
			/ static_cast<double>(UsableTileResolution);
		const double SourceSphereRadius = FMath::Max(
			static_cast<double>(SourceBounds.SphereRadius),
			UE_DOUBLE_SMALL_NUMBER);
		return FMath::Min(
			SourceSphereRadius,
			FMath::Max(UnpaddedHalfExtent * GuardScale, UE_DOUBLE_SMALL_NUMBER));
	}

	void BuildCaptureViews(
		const UFoliageBakerImpostorSettings& Settings,
		const FBoxSphereBounds& SourceBounds,
		const TArray<FVector>& SourceVertices,
		TArray<FImpostorCaptureView>& OutViews,
		TArray<UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo>& OutTileInfos,
		FImpostorBakeStats& OutStats,
		double& OutSharedCaptureHalfExtent)
	{
		const int32 GridSize = FMath::Clamp(Settings.FrameGridSize, 3, 8);
		const int32 MaxAtlasResolution = FMath::Clamp(Settings.TextureResolution, 256, 4096);
		const int32 TileResolution = FMath::Max(4, (MaxAtlasResolution / GridSize) & ~3);

		OutStats.TileResolution = TileResolution;
		OutStats.AtlasWidth = TileResolution * GridSize;
		OutStats.AtlasHeight = TileResolution * GridSize;
		OutStats.ViewCount = GridSize * GridSize;
		OutViews.Reset(OutStats.ViewCount);
		OutTileInfos.Reset(OutStats.ViewCount);

		for (int32 GridY = 0; GridY < GridSize; ++GridY)
		{
			for (int32 GridX = 0; GridX < GridSize; ++GridX)
			{
				const FVector2D Encoded(
					-1.0 + 2.0 * static_cast<double>(GridX) / static_cast<double>(GridSize - 1),
					-1.0 + 2.0 * static_cast<double>(GridY) / static_cast<double>(GridSize - 1));
				FImpostorCaptureView& View = OutViews.AddDefaulted_GetRef();
				View.GridIndex = FIntPoint(GridX, GridY);
				View.ViewDirection = Settings.Coverage == EFoliageBakerImpostorCoverage::FullSphere
					? DecodeFullOctahedralDirection(Encoded)
					: DecodeHemiOctahedralDirection(Encoded);
				View.CaptureRayDirection = -View.ViewDirection;
				const FRotationMatrix CaptureRotation(View.CaptureRayDirection.Rotation());
				View.AxisU = CaptureRotation.GetScaledAxis(EAxis::Y);
				View.AxisV = CaptureRotation.GetScaledAxis(EAxis::Z);
				View.TilePixelMin = FIntPoint(GridX * TileResolution, GridY * TileResolution);
				View.TileSize = FIntPoint(TileResolution, TileResolution);

				UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& TileInfo = OutTileInfos.AddDefaulted_GetRef();
				TileInfo.AtlasPixelMin = View.TilePixelMin;
				TileInfo.AtlasTileSize = View.TileSize;
				TileInfo.AtlasTileResolution = TileResolution;
			}
		}

		OutSharedCaptureHalfExtent = ComputeSharedCaptureHalfExtent(
			SourceVertices,
			SourceBounds,
			OutViews,
			TileResolution);
		for (int32 ViewIndex = 0; ViewIndex < OutViews.Num(); ++ViewIndex)
		{
			FImpostorCaptureView& View = OutViews[ViewIndex];
			View.ProjectionHalfExtentU = OutSharedCaptureHalfExtent;
			View.ProjectionHalfExtentV = OutSharedCaptureHalfExtent;
			UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& TileInfo = OutTileInfos[ViewIndex];
			TileInfo.SourcePlaneIndex = ViewIndex;
			TileInfo.Normal = View.ViewDirection;
			TileInfo.Rho = FVector::DotProduct(View.ViewDirection, SourceBounds.Origin);
			TileInfo.AxisU = View.AxisU;
			TileInfo.AxisV = View.AxisV;
			const double CenterU = FVector::DotProduct(SourceBounds.Origin, View.AxisU);
			const double CenterV = FVector::DotProduct(SourceBounds.Origin, View.AxisV);
			TileInfo.MinU = CenterU - OutSharedCaptureHalfExtent;
			TileInfo.MaxU = CenterU + OutSharedCaptureHalfExtent;
			TileInfo.MinV = CenterV - OutSharedCaptureHalfExtent;
			TileInfo.MaxV = CenterV + OutSharedCaptureHalfExtent;
		}
	}

	bool BakeViewAtlas(
		const UStaticMesh& SourceStaticMesh,
		const UFoliageBakerImpostorSettings& Settings,
		FImpostorBakeData& InOutData,
		FString& OutError)
	{
		const int32 PixelCount = InOutData.Stats.AtlasWidth * InOutData.Stats.AtlasHeight;
		if (PixelCount <= 0 || InOutData.Views.IsEmpty())
		{
			OutError = TEXT("The Impostor capture grid is empty.");
			return false;
		}

		InOutData.BaseColorPixels.Init(FColor(0, 0, 0, 0), PixelCount);
		InOutData.NormalDepthPixels.Init(
			EncodeOctahedralObjectSpaceNormal(
				FVector::UpVector,
				0,
				UnitFloatToByte(0.5f)),
			PixelCount);
		if (Settings.bBakeMix)
		{
			InOutData.MixPixels.Init(FColor(255, 128, 0, 0), PixelCount);
		}
		else
		{
			InOutData.MixPixels.Reset();
		}

		TBitArray<> CoverageMask;
		CoverageMask.Init(false, PixelCount);
		InOutData.CoverageValues.Init(0.0f, PixelCount);
		TBitArray<> NormalCoverage;
		NormalCoverage.Init(false, PixelCount);

		TArray<int32> SourceTriangleIndices;
		SourceTriangleIndices.Reserve(InOutData.Triangles.Num());
		TSet<int32> ReferencedMaterialSet;
		for (int32 TriangleIndex = 0; TriangleIndex < InOutData.Triangles.Num(); ++TriangleIndex)
		{
			const FSourceTriangle& Triangle = InOutData.Triangles[TriangleIndex];
			if (Triangle.Area <= 0.0)
			{
				continue;
			}
			SourceTriangleIndices.Add(TriangleIndex);
			ReferencedMaterialSet.Add(FMath::Max(0, Triangle.MaterialIndex));
		}
		TArray<int32> ReferencedMaterialIndices = ReferencedMaterialSet.Array();
		ReferencedMaterialIndices.Sort();
		if (SourceTriangleIndices.IsEmpty() || ReferencedMaterialIndices.IsEmpty())
		{
			OutError = TEXT("The selected LOD contains no rasterizable Impostor triangles.");
			return false;
		}

		const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
		const double SharedCaptureHalfExtent = FMath::Max(InOutData.SharedCaptureHalfExtent, UE_DOUBLE_SMALL_NUMBER);
		const FVector SharedCenter = InOutData.SourceBounds.Origin;
		const TArray<UE::FoliageBaker::PlaneCover::FCrackReductionProjection> NoCrackReductionProjections;
		struct FDepthCorrectMaterialStorage
		{
			UMaterialInterface* MaterialInterface = nullptr;
			FMeshDescription MeshDescription;
			TArray<FVector2D> CustomTileUVs;
			TArray<int32> RasterSourceTriangleIndices;
			FMeshData MeshSettings;
		};
		for (int32 ViewIndex = 0; ViewIndex < InOutData.Views.Num(); ++ViewIndex)
		{
			const FImpostorCaptureView& View = InOutData.Views[ViewIndex];
			if (!InOutData.TileInfos.IsValidIndex(ViewIndex))
			{
				OutError = FString::Printf(TEXT("Impostor frame %d has no matching tile geometry."), ViewIndex);
				return false;
			}
			const UE::FoliageBaker::PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo =
				InOutData.TileInfos[ViewIndex];
			const int32 TilePixelCount = View.TileSize.X * View.TileSize.Y;

			TArray<TUniquePtr<FDepthCorrectMaterialStorage>> MaterialStorage;
			MaterialStorage.Reserve(ReferencedMaterialIndices.Num());
			FFoliageBakerDepthCorrectTileRequest DepthCorrectRequest;
			DepthCorrectRequest.TextureSize = View.TileSize;
			DepthCorrectRequest.CaptureRayDirection = View.CaptureRayDirection;
			DepthCorrectRequest.SourceBounds = InOutData.SourceBounds;
			DepthCorrectRequest.bBakeBaseColor = Settings.bBakeBaseColorSdf;
			DepthCorrectRequest.bBakeObjectSpaceNormal = Settings.bBakeNormalDepth;
			DepthCorrectRequest.bBakePackedMix = Settings.bBakeMix;
			DepthCorrectRequest.bBakeRoughnessSpecular = !Settings.bBakeMix;
			DepthCorrectRequest.Materials.Reserve(ReferencedMaterialIndices.Num());

			for (const int32 MaterialIndex : ReferencedMaterialIndices)
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

				UE::FoliageBaker::ProjectedMaterialBake::FPlaneSideBakeParams ProjectedBakeParams;
				ProjectedBakeParams.CaptureRayDirection = View.CaptureRayDirection;
				ProjectedBakeParams.AtlasVConvention =
					UE::FoliageBaker::PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV;
				ProjectedBakeParams.MaterialIndexFilter = MaterialIndex;
				ProjectedBakeParams.bBackSide = false;

				int32 MatchingTriangleCount = 0;
				FString ProjectedInputError;
				const bool bBuiltPlaneSideBakeInputs =
					UE::FoliageBaker::ProjectedMaterialBake::BuildPlaneSideBakeInputs(
						InOutData.Triangles,
						SourceTriangleIndices,
						NoCrackReductionProjections,
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
						TEXT("Impostor depth-correct material input failed for frame %d, material %d: %s"),
						ViewIndex,
						MaterialIndex,
						*ProjectedInputError);
					return false;
				}

				InOutData.Stats.RasterizedTriangleReferences += MatchingTriangleCount;
				if (Storage->MaterialInterface->GetBlendMode() == BLEND_Masked)
				{
					InOutData.Stats.MaskedTriangleReferences += MatchingTriangleCount;
				}

				Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
				Storage->MeshSettings.Mesh = &SourceStaticMesh;
				Storage->MeshSettings.MaterialIndices.Add(0);
				Storage->MeshSettings.TextureCoordinateBox =
					FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				Storage->MeshSettings.TextureCoordinateIndex = 0;
				Storage->MeshSettings.LightMapIndex = 0;
				Storage->MeshSettings.PrimitiveData = FPrimitiveData(InOutData.SourceBounds);
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
				continue;
			}

			FFoliageBakerDepthCorrectTileResult DepthCorrectResult;
			FString DepthCorrectError;
			if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(
					DepthCorrectRequest,
					DepthCorrectResult,
					&DepthCorrectError))
			{
				OutError = FString::Printf(
					TEXT("Impostor depth-correct tile bake failed for frame %d: %s"),
					ViewIndex,
					*DepthCorrectError);
				return false;
			}
			if (DepthCorrectResult.SourceTriangleIdAndDepth.Num() != TilePixelCount
				|| (Settings.bBakeBaseColorSdf && DepthCorrectResult.BaseColor.Num() != TilePixelCount)
				|| (Settings.bBakeNormalDepth && DepthCorrectResult.ObjectSpaceNormal.Num() != TilePixelCount)
				|| (Settings.bBakeMix && DepthCorrectResult.PackedMix.Num() != TilePixelCount)
				|| (!Settings.bBakeMix
					&& (DepthCorrectResult.Roughness.Num() != TilePixelCount
						|| DepthCorrectResult.Specular.Num() != TilePixelCount)))
			{
				OutError = FString::Printf(
					TEXT("Impostor depth-correct tile returned invalid sizes for frame %d: base=%d, id=%d, normal=%d, mix=%d, roughness=%d, specular=%d, expected=%d."),
					ViewIndex,
					DepthCorrectResult.BaseColor.Num(),
					DepthCorrectResult.SourceTriangleIdAndDepth.Num(),
					DepthCorrectResult.ObjectSpaceNormal.Num(),
					DepthCorrectResult.PackedMix.Num(),
					DepthCorrectResult.Roughness.Num(),
					DepthCorrectResult.Specular.Num(),
					TilePixelCount);
				return false;
			}
			++InOutData.Stats.DepthCorrectTileCount;

			for (int32 LocalY = 0; LocalY < View.TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = View.TilePixelMin.Y + LocalY;
				for (int32 LocalX = 0; LocalX < View.TileSize.X; ++LocalX)
				{
					const int32 TileIndex = LocalY * View.TileSize.X + LocalX;
					const int32 TriangleIndex =
						FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(
							DepthCorrectResult.SourceTriangleIdAndDepth[TileIndex]);
					if (TriangleIndex == INDEX_NONE)
					{
						continue;
					}
					if (!InOutData.Triangles.IsValidIndex(TriangleIndex))
					{
						OutError = FString::Printf(
							TEXT("Impostor depth-correct tile decoded invalid triangle %d at pixel (%d,%d) for frame %d."),
							TriangleIndex,
							LocalX,
							LocalY,
							ViewIndex);
						return false;
					}

					const FSourceTriangle& Triangle = InOutData.Triangles[TriangleIndex];
					if (!Settings.bBakeMix)
					{
						InOutData.Stats.MaterialAverages.AddSample(
							Triangle.bIsTrunk,
							DepthCorrectResult.Roughness[TileIndex].R,
							DepthCorrectResult.Specular[TileIndex].R);
					}
					FVector2D ProjectedPoints[3];
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						const FVector LocalPosition = Triangle.Vertices[VertexIndex] - SharedCenter;
						const double U = 0.5
							+ FVector::DotProduct(LocalPosition, View.AxisU)
								/ (2.0 * View.ProjectionHalfExtentU);
						const double V = 0.5
							- FVector::DotProduct(LocalPosition, View.AxisV)
								/ (2.0 * View.ProjectionHalfExtentV);
						ProjectedPoints[VertexIndex] = FVector2D(
							U * View.TileSize.X,
							V * View.TileSize.Y);
					}

					double W0 = 0.0;
					double W1 = 0.0;
					double W2 = 0.0;
					if (!UE::FoliageBaker::ProjectedMaterialBake::ComputeGpuWinnerBarycentric2D(
							FVector2D(LocalX + 0.5, LocalY + 0.5),
							ProjectedPoints[0],
							ProjectedPoints[1],
							ProjectedPoints[2],
							W0,
							W1,
							W2))
					{
						OutError = FString::Printf(
							TEXT("Impostor depth-correct tile encountered a degenerate triangle %d at pixel (%d,%d) for frame %d."),
							TriangleIndex,
							LocalX,
							LocalY,
							ViewIndex);
						return false;
					}

					const FVector SourcePoint = Triangle.Vertices[0] * W0
						+ Triangle.Vertices[1] * W1
						+ Triangle.Vertices[2] * W2;
					const float CaptureDepth = static_cast<float>(FVector::DotProduct(
						SourcePoint - SharedCenter,
						View.CaptureRayDirection));
					const int32 AtlasX = View.TilePixelMin.X + LocalX;
					const int32 AtlasIndex = AtlasY * InOutData.Stats.AtlasWidth + AtlasX;

					if (Settings.bBakeBaseColorSdf)
					{
						FColor Color = DepthCorrectResult.BaseColor[TileIndex];
						Color.A = 255;
						InOutData.BaseColorPixels[AtlasIndex] = Color;
					}
					CoverageMask[AtlasIndex] = true;
					InOutData.CoverageValues[AtlasIndex] = 1.0f;

					if (Settings.bBakeNormalDepth)
					{
						const float LinearDepth = FMath::Clamp(
							static_cast<float>((SharedCaptureHalfExtent - CaptureDepth)
								/ (2.0 * SharedCaptureHalfExtent)),
							0.0f,
							1.0f);
						InOutData.NormalDepthPixels[AtlasIndex] =
							EncodeOctahedralObjectSpaceNormal(
								DecodeObjectSpaceNormal(
									DepthCorrectResult.ObjectSpaceNormal[TileIndex]),
								UE::FoliageBaker::Atlas::EncodeTrunkLeafAlpha(
									Triangle.bIsTrunk),
								UnitFloatToByte(LinearDepth));
						NormalCoverage[AtlasIndex] = true;
					}

					if (Settings.bBakeMix)
					{
						InOutData.MixPixels[AtlasIndex] = DepthCorrectResult.PackedMix[TileIndex];
					}
					++InOutData.Stats.PaintedPixels;
				}
			}
		}

		if (Settings.bBakeBaseColorSdf)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.BaseColorPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos);
			UE::FoliageBaker::Atlas::WriteUnionSdfToAlpha(
				InOutData.BaseColorPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				CoverageMask,
				FMath::Clamp(Settings.OpacitySdfRangePixels, 1, 64));
		}
		else
		{
			InOutData.BaseColorPixels.Reset();
		}

		if (Settings.bBakeNormalDepth)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.NormalDepthPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				&NormalCoverage,
				false);
			for (int32 PixelIndex = 0; PixelIndex < InOutData.NormalDepthPixels.Num(); ++PixelIndex)
			{
				if (!NormalCoverage[PixelIndex])
				{
					InOutData.NormalDepthPixels[PixelIndex].A = UnitFloatToByte(0.5f);
				}
			}
		}
		else
		{
			InOutData.NormalDepthPixels.Reset();
		}

		if (Settings.bBakeMix)
		{
			UE::FoliageBaker::Atlas::FillTransparentRGBInsideTiles(
				InOutData.MixPixels,
				InOutData.Stats.AtlasWidth,
				InOutData.Stats.AtlasHeight,
				InOutData.TileInfos,
				&CoverageMask,
				true);
		}
		if (InOutData.Stats.PaintedPixels <= 0)
		{
			OutError = TEXT("The Impostor RDG capture produced no visible pixels.");
			return false;
		}
		return true;
	}

	constexpr int32 ImpostorCutoutOutlineVertexCount = 8;
	constexpr int32 ImpostorCutoutOrientationSampleCount = 32;
	constexpr double ImpostorCutoutGuardPixels = 2.0;

	double ComputeCutoutOutlineArea(const TArray<FVector2D>& Outline)
	{
		double TwiceArea = 0.0;
		for (int32 Index = 0; Index < Outline.Num(); ++Index)
		{
			const FVector2D& A = Outline[Index];
			const FVector2D& B = Outline[(Index + 1) % Outline.Num()];
			TwiceArea += A.X * B.Y - A.Y * B.X;
		}
		return FMath::Abs(TwiceArea) * 0.5;
	}

	bool IntersectCutoutSupportLines(
		const FVector2D& NormalA,
		const double SupportA,
		const FVector2D& NormalB,
		const double SupportB,
		FVector2D& OutPoint)
	{
		const double Determinant = NormalA.X * NormalB.Y - NormalA.Y * NormalB.X;
		if (FMath::IsNearlyZero(Determinant, UE_DOUBLE_SMALL_NUMBER))
		{
			return false;
		}
		OutPoint = FVector2D(
			(SupportA * NormalB.Y - NormalA.Y * SupportB) / Determinant,
			(NormalA.X * SupportB - SupportA * NormalB.X) / Determinant);
		return FMath::IsFinite(OutPoint.X) && FMath::IsFinite(OutPoint.Y);
	}

	bool BuildCutoutCoverageSupportPoints(
		const TArray<FImpostorCaptureView>& Views,
		const TArray<float>& CoverageValues,
		const int32 AtlasWidth,
		TArray<FVector2D>& OutSupportPoints,
		int32& OutTileResolution)
	{
		if (Views.IsEmpty() || AtlasWidth <= 0)
		{
			return false;
		}

		const FIntPoint TileSize = Views[0].TileSize;
		if (TileSize.X <= 0 || TileSize.Y <= 0)
		{
			return false;
		}
		for (const FImpostorCaptureView& View : Views)
		{
			if (View.TileSize != TileSize)
			{
				return false;
			}
		}

		TArray<int32> MinCoveredX;
		TArray<int32> MaxCoveredX;
		MinCoveredX.Init(TileSize.X, TileSize.Y);
		MaxCoveredX.Init(INDEX_NONE, TileSize.Y);
		for (const FImpostorCaptureView& View : Views)
		{
			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = View.TilePixelMin.Y + LocalY;
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 AtlasX = View.TilePixelMin.X + LocalX;
					const int32 AtlasIndex = AtlasY * AtlasWidth + AtlasX;
					if (CoverageValues.IsValidIndex(AtlasIndex)
						&& CoverageValues[AtlasIndex] > 0.0f)
					{
						MinCoveredX[LocalY] = FMath::Min(MinCoveredX[LocalY], LocalX);
						MaxCoveredX[LocalY] = FMath::Max(MaxCoveredX[LocalY], LocalX);
					}
				}
			}
		}

		OutSupportPoints.Reset(TileSize.Y * 8 + 1);
		OutSupportPoints.Add(FVector2D(0.5, 0.5));
		const double InverseWidth = 1.0 / static_cast<double>(TileSize.X);
		const double InverseHeight = 1.0 / static_cast<double>(TileSize.Y);
		for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
		{
			if (MaxCoveredX[LocalY] == INDEX_NONE)
			{
				continue;
			}

			const double MinX = static_cast<double>(MinCoveredX[LocalY]) * InverseWidth;
			const double MaxX = static_cast<double>(MaxCoveredX[LocalY] + 1) * InverseWidth;
			const double MinY = static_cast<double>(LocalY) * InverseHeight;
			const double MaxY = static_cast<double>(LocalY + 1) * InverseHeight;
			OutSupportPoints.Add(FVector2D(MinX, MinY));
			OutSupportPoints.Add(FVector2D(MinX, MaxY));
			OutSupportPoints.Add(FVector2D(MaxX, MinY));
			OutSupportPoints.Add(FVector2D(MaxX, MaxY));
		}

		OutTileResolution = FMath::Min(TileSize.X, TileSize.Y);
		return OutSupportPoints.Num() > 1;
	}

	bool FindMinimumAreaCutoutOctagon(
		const TArray<FVector2D>& SupportPoints,
		const double GuardUV,
		TArray<FVector2D>& OutOutline)
	{
		constexpr double UnitSquareTolerance = 1.0e-6;
		constexpr double HalfPlaneTolerance = 1.0e-8;
		double BestArea = TNumericLimits<double>::Max();
		TArray<FVector2D> BestOutline;

		for (int32 OrientationIndex = 0;
			OrientationIndex < ImpostorCutoutOrientationSampleCount;
			++OrientationIndex)
		{
			const double BaseAngle = (PI / 4.0)
				* static_cast<double>(OrientationIndex)
				/ static_cast<double>(ImpostorCutoutOrientationSampleCount);
			FVector2D Normals[ImpostorCutoutOutlineVertexCount];
			double Supports[ImpostorCutoutOutlineVertexCount];
			for (int32 SideIndex = 0; SideIndex < ImpostorCutoutOutlineVertexCount; ++SideIndex)
			{
				const double Angle = BaseAngle + static_cast<double>(SideIndex) * PI / 4.0;
				Normals[SideIndex] = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle));
				Supports[SideIndex] = -TNumericLimits<double>::Max();
				for (const FVector2D& Point : SupportPoints)
				{
					Supports[SideIndex] = FMath::Max(
						Supports[SideIndex],
						FVector2D::DotProduct(Normals[SideIndex], Point));
				}
				Supports[SideIndex] += GuardUV;
			}

			TArray<FVector2D> CandidateOutline;
			CandidateOutline.Reserve(ImpostorCutoutOutlineVertexCount);
			bool bValidCandidate = true;
			for (int32 SideIndex = 0; SideIndex < ImpostorCutoutOutlineVertexCount; ++SideIndex)
			{
				const int32 NextSideIndex = (SideIndex + 1) % ImpostorCutoutOutlineVertexCount;
				FVector2D Point;
				if (!IntersectCutoutSupportLines(
						Normals[SideIndex],
						Supports[SideIndex],
						Normals[NextSideIndex],
						Supports[NextSideIndex],
						Point)
					|| Point.X < -UnitSquareTolerance
					|| Point.X > 1.0 + UnitSquareTolerance
					|| Point.Y < -UnitSquareTolerance
					|| Point.Y > 1.0 + UnitSquareTolerance)
				{
					bValidCandidate = false;
					break;
				}
				Point.X = FMath::Clamp(Point.X, 0.0, 1.0);
				Point.Y = FMath::Clamp(Point.Y, 0.0, 1.0);
				CandidateOutline.Add(Point);
			}
			if (!bValidCandidate)
			{
				continue;
			}

			for (const FVector2D& Point : CandidateOutline)
			{
				for (int32 SideIndex = 0; SideIndex < ImpostorCutoutOutlineVertexCount; ++SideIndex)
				{
					if (FVector2D::DotProduct(Normals[SideIndex], Point)
						> Supports[SideIndex] + HalfPlaneTolerance)
					{
						bValidCandidate = false;
						break;
					}
				}
				if (!bValidCandidate)
				{
					break;
				}
			}
			if (!bValidCandidate)
			{
				continue;
			}

			const double Area = ComputeCutoutOutlineArea(CandidateOutline);
			if (Area > UE_DOUBLE_SMALL_NUMBER && Area < BestArea)
			{
				BestArea = Area;
				BestOutline = MoveTemp(CandidateOutline);
			}
		}

		if (BestOutline.Num() != ImpostorCutoutOutlineVertexCount)
		{
			return false;
		}
		BestOutline.Sort([](const FVector2D& A, const FVector2D& B)
		{
			return FMath::Atan2(A.Y - 0.5, A.X - 0.5)
				> FMath::Atan2(B.Y - 0.5, B.X - 0.5);
		});
		OutOutline = MoveTemp(BestOutline);
		return true;
	}

	bool BuildConservativeCutoutOctagon(
		const TArray<FImpostorCaptureView>& Views,
		const TArray<float>& CoverageValues,
		const int32 AtlasWidth,
		TArray<FVector2D>& OutOutline)
	{
		TArray<FVector2D> SupportPoints;
		int32 TileResolution = 0;
		if (!BuildCutoutCoverageSupportPoints(
				Views,
				CoverageValues,
				AtlasWidth,
				SupportPoints,
				TileResolution))
		{
			return false;
		}

		const double RequestedGuardUV = ImpostorCutoutGuardPixels
			/ static_cast<double>(FMath::Max(TileResolution, 1));
		if (FindMinimumAreaCutoutOctagon(SupportPoints, RequestedGuardUV, OutOutline))
		{
			return true;
		}

		double ValidGuardUV = 0.0;
		double InvalidGuardUV = RequestedGuardUV;
		if (!FindMinimumAreaCutoutOctagon(SupportPoints, ValidGuardUV, OutOutline))
		{
			return false;
		}
		for (int32 Iteration = 0; Iteration < 12; ++Iteration)
		{
			const double CandidateGuardUV = (ValidGuardUV + InvalidGuardUV) * 0.5;
			TArray<FVector2D> CandidateOutline;
			if (FindMinimumAreaCutoutOctagon(
					SupportPoints,
					CandidateGuardUV,
					CandidateOutline))
			{
				ValidGuardUV = CandidateGuardUV;
				OutOutline = MoveTemp(CandidateOutline);
			}
			else
			{
				InvalidGuardUV = CandidateGuardUV;
			}
		}
		return OutOutline.Num() == ImpostorCutoutOutlineVertexCount;
	}

	bool BuildCutoutMeshDescription(
		const FBoxSphereBounds& SourceBounds,
		const double SharedCaptureHalfExtent,
		const TArray<FImpostorCaptureView>& Views,
		const TArray<float>& CoverageValues,
		const int32 AtlasWidth,
		const int32 FrameGridSize,
		FMeshDescription& OutMeshDescription,
		FString& OutError)
	{
		OutMeshDescription.Empty();
		if (Views.IsEmpty() || CoverageValues.IsEmpty() || AtlasWidth <= 0)
		{
			OutError = TEXT("The captured coverage is empty and cannot produce an Impostor cutout.");
			return false;
		}
		TArray<FVector2D> Outline;
		if (!BuildConservativeCutoutOctagon(Views, CoverageValues, AtlasWidth, Outline))
		{
			OutError = TEXT("The combined Impostor coverage cannot produce a conservative eight-vertex cutout.");
			return false;
		}

		FStaticMeshAttributes Attributes(OutMeshDescription);
		Attributes.Register();
		Attributes.RegisterTriangleNormalAndTangentAttributes();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TTriangleAttributesRef<FVector3f> TriangleNormals = Attributes.GetTriangleNormals();
		TTriangleAttributesRef<FVector3f> TriangleTangents = Attributes.GetTriangleTangents();
		TTriangleAttributesRef<FVector3f> TriangleBinormals = Attributes.GetTriangleBinormals();
		TEdgeAttributesRef<bool> EdgeHardnesses = Attributes.GetEdgeHardnesses();
		TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		VertexInstanceUVs.SetNumChannels(1);
		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		MaterialSlotNames[PolygonGroupID] = TEXT("ImpostorProxy");
		const FVector Center = SourceBounds.Origin;
		const double HalfExtent = FMath::Max(SharedCaptureHalfExtent, UE_DOUBLE_SMALL_NUMBER);
		const double ClampedFrameGridSize = FMath::Clamp(FrameGridSize, 3, 8);
		const auto EncodeStoredUV = [ClampedFrameGridSize](const FVector2D& CutoutUV)
		{
			const double U = (FMath::Clamp(CutoutUV.X, 0.0, 1.0) + 0.001) * 0.995;
			const double V = (FMath::Clamp(CutoutUV.Y, 0.0, 1.0) + 0.001) * 0.995;
			return FVector2f(
				static_cast<float>((U / ClampedFrameGridSize) / 10.0),
				static_cast<float>((V / ClampedFrameGridSize) / 10.0));
		};
		const FVector FaceNormal = FVector::UpVector;
		const FVector FaceTangent = FVector::ForwardVector;
		const FVector FaceBinormal = FVector::RightVector;
		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Outline.Num() + 1);
		const FVertexID CenterVertexID = OutMeshDescription.CreateVertex();
		VertexPositions[CenterVertexID] = FVector3f(Center);
		VertexIDs.Add(CenterVertexID);
		for (const FVector2D& UV : Outline)
		{
			const FVertexID VertexID = OutMeshDescription.CreateVertex();
			const double LocalU = ((UV.X - 0.5) * 2.0 * HalfExtent) / 10.0;
			const double LocalV = ((UV.Y - 0.5) * 2.0 * HalfExtent) / 10.0;
			VertexPositions[VertexID] = FVector3f(
				Center + FaceTangent * LocalU + FaceBinormal * LocalV);
			VertexIDs.Add(VertexID);
		}
		for (int32 OutlineIndex = 0; OutlineIndex < Outline.Num(); ++OutlineIndex)
		{
			const int32 NextOutlineIndex = (OutlineIndex + 1) % Outline.Num();
			const int32 CornerIndices[3] = { OutlineIndex + 1, NextOutlineIndex + 1, 0 };
			TArray<FVertexInstanceID> VertexInstanceIDs;
			VertexInstanceIDs.Reserve(3);
			for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex)
			{
				const int32 VertexIndex = CornerIndices[CornerIndex];
				const FVertexID VertexID = VertexIDs[VertexIndex];
				const FVertexInstanceID VertexInstanceID = OutMeshDescription.CreateVertexInstance(VertexID);
				VertexInstanceIDs.Add(VertexInstanceID);
				const FVector2D CutoutUV = VertexIndex == 0
					? FVector2D(0.5, 0.5)
					: Outline[VertexIndex - 1];
				VertexInstanceUVs.Set(VertexInstanceID, 0, EncodeStoredUV(CutoutUV));
				VertexInstanceNormals[VertexInstanceID] = FVector3f(FaceNormal);
				VertexInstanceTangents[VertexInstanceID] = FVector3f(FaceTangent);
				VertexInstanceBinormalSigns[VertexInstanceID] = 1.0f;
			}

			TArray<FEdgeID> NewEdges;
			const FPolygonID PolygonID = OutMeshDescription.CreatePolygon(PolygonGroupID, VertexInstanceIDs, &NewEdges);
			for (const FEdgeID EdgeID : NewEdges)
			{
				EdgeHardnesses[EdgeID] = true;
			}
			for (const FTriangleID TriangleID : OutMeshDescription.GetPolygonTriangles(PolygonID))
			{
				TriangleNormals[TriangleID] = FVector3f(FaceNormal);
				TriangleTangents[TriangleID] = FVector3f(FaceTangent);
				TriangleBinormals[TriangleID] = FVector3f(FaceBinormal);
			}
		}

		if (OutMeshDescription.Vertices().Num() != Outline.Num() + 1
			|| OutMeshDescription.Triangles().Num() != Outline.Num())
		{
			OutError = TEXT("The Impostor cutout mesh did not produce the expected triangles.");
			return false;
		}
		return true;
	}

	FFoliageBakerSourceLODAssetParams BuildSourceLODAssetParams(
		const UFoliageBakerImpostorSettings& Settings,
		const FFoliageBakerMeshOutputSelection& MeshOutputSelection)
	{
		FFoliageBakerSourceLODAssetParams Params;
		Params.OutputMode = MeshOutputSelection.OutputMode;
		Params.RequestedReplaceLODIndex = MeshOutputSelection.ReplaceLODIndex;
		Params.RequestedInsertAfterLODIndex = MeshOutputSelection.InsertAfterLODIndex;
		Params.SourceLODIndex = Settings.SourceLODIndex;
		Params.DesiredUVChannelCount = 1;
		Params.MaterialSlotName = TEXT("ImpostorProxy");
		Params.bRecomputeNormals = true;
		Params.bRecomputeTangents = true;
		Params.BaseLODModel = 0;
		Params.RebuildLODMetadataKey = TEXT("FoliageBaker.ImpostorLOD");
		return Params;
	}

	UTexture2D* CreateTexture(
		const UStaticMesh& SourceStaticMesh,
		FFoliageBakerAssetTransaction& Transaction,
		const UFoliageBakerImpostorSettings& Settings,
		const FString& OutputPackagePathOverride,
		const FString& Suffix,
		const TArray<FColor>& Pixels,
		const FImpostorBakeStats& Stats,
		const TextureCompressionSettings Compression,
		const TextureGroup LODGroup,
		const bool bSRGB,
		const FColor MipBackgroundColor,
		FString& OutError)
	{
		FFoliageBakerTextureAssetParams Params;
		Params.OutputFolderName = Settings.TextureOutputFolderName;
		Params.OutputPackagePathOverride = OutputPackagePathOverride;
		Params.AssetNamePrefix = Settings.TextureNamePrefix;
		Params.AssetNameSuffix = Suffix;
		Params.Width = Stats.AtlasWidth;
		Params.Height = Stats.AtlasHeight;
		Params.CompressionSettings = Compression;
		Params.LODGroup = LODGroup;
		Params.bSRGB = bSRGB;
		Params.MipBackgroundColor = MipBackgroundColor;
		Params.bNormalizeMipNormals = false;
		const int32 TileResolution = FMath::Max(1, Stats.TileResolution);
		for (int32 TileY = 0; TileY < Stats.AtlasHeight; TileY += TileResolution)
		{
			for (int32 TileX = 0; TileX < Stats.AtlasWidth; TileX += TileResolution)
			{
				Params.MipTileRects.Add(FIntRect(
					FIntPoint(TileX, TileY),
					FIntPoint(
						FMath::Min(TileX + TileResolution, Stats.AtlasWidth),
						FMath::Min(TileY + TileResolution, Stats.AtlasHeight))));
			}
		}
		Params.EmptyPixelsError = TEXT("No Impostor atlas pixels were generated.");
		return FFoliageBakerAssetBuilder::CreateTextureAsset(SourceStaticMesh, Transaction, Params, Pixels, OutError);
	}

	bool ValidateParameterNames(const UFoliageBakerImpostorSettings& Settings, FString& OutError)
	{
		TSet<FName> TextureParameterNames;
		auto ValidateTextureName = [&](const bool bEnabled, const FName Name, const TCHAR* Label)
		{
			if (!bEnabled)
			{
				return true;
			}
			if (Name.IsNone())
			{
				OutError = FString::Printf(TEXT("%s texture parameter name is None."), Label);
				return false;
			}
			if (TextureParameterNames.Contains(Name))
			{
				OutError = FString::Printf(TEXT("Texture parameter '%s' is assigned to more than one enabled output."), *Name.ToString());
				return false;
			}
			TextureParameterNames.Add(Name);
			return true;
		};
		return ValidateTextureName(Settings.bBakeBaseColorSdf, Settings.BaseColorSdfTextureParameterName, TEXT("BaseColor/SDF"))
			&& ValidateTextureName(Settings.bBakeNormalDepth, Settings.NormalDepthTextureParameterName, TEXT("Normal/Depth"))
			&& ValidateTextureName(Settings.bBakeMix, Settings.MixTextureParameterName, TEXT("Mix"))
			&& !Settings.FramesParameterName.IsNone()
			&& !Settings.DefaultMeshSizeParameterName.IsNone()
			&& !Settings.PivotOffsetParameterName.IsNone()
			&& !Settings.UpperHemisphereStaticSwitchParameterName.IsNone();
	}

	void AppendCreatedAsset(UObject* Asset, TArray<UObject*>& OutAssets)
	{
		if (Asset)
		{
			OutAssets.AddUnique(Asset);
		}
	}
}

FFoliageBakerImpostorBakeResult FFoliageBakerImpostorBaker::Bake(
	UStaticMesh& SourceStaticMesh,
	UMaterialInstanceConstant& MaterialTemplate,
	const UFoliageBakerImpostorSettings& Settings)
{
	FFoliageBakerImpostorBakeResult Result;
	FString Error;
	if (Settings.SourceLODIndex < 0 || Settings.SourceLODIndex >= MAX_STATIC_MESH_LODS)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: Source LOD Index is outside the supported range."), *SourceStaticMesh.GetName());
		return Result;
	}
	if (!Settings.bBakeBaseColorSdf && !Settings.bBakeNormalDepth && !Settings.bBakeMix)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: no Impostor texture output is enabled."), *SourceStaticMesh.GetName());
		return Result;
	}
	if (!ValidateParameterNames(Settings, Error))
	{
		if (Error.IsEmpty())
		{
			Error = TEXT("One or more runtime material parameter names are None.");
		}
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	FImpostorBakeData BakeData;
	if (!UE::FoliageBaker::PlaneCover::ExtractTrianglesFromStaticMesh(
			&SourceStaticMesh,
			Settings.SourceLODIndex,
			BakeData.Triangles,
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}
	const UE::FoliageBaker::MaterialResolver::FMaterialKeywordMatchResult
		TrunkMaterialMatches =
			UE::FoliageBaker::MaterialResolver::ResolveMaterialKeywordMatches(
				SourceStaticMesh,
				Settings.TrunkMaterialKeywords);
	for (FSourceTriangle& Triangle : BakeData.Triangles)
	{
		Triangle.bIsTrunk = TrunkMaterialMatches.IsMatch(Triangle.MaterialIndex);
	}
	if (!ComputeSourceBounds(BakeData.Triangles, BakeData.SourceVertices, BakeData.SourceBounds))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: selected LOD bounds could not be computed."), *SourceStaticMesh.GetName());
		return Result;
	}

	BuildCaptureViews(
		Settings,
		BakeData.SourceBounds,
		BakeData.SourceVertices,
		BakeData.Views,
		BakeData.TileInfos,
		BakeData.Stats,
		BakeData.SharedCaptureHalfExtent);
	if (!BakeViewAtlas(SourceStaticMesh, Settings, BakeData, Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), Error.IsEmpty() ? TEXT("no visible Impostor pixels were captured.") : *Error);
		return Result;
	}

	FMeshDescription MeshDescription;
	if (!BuildCutoutMeshDescription(
			BakeData.SourceBounds,
			BakeData.SharedCaptureHalfExtent,
			BakeData.Views,
			BakeData.CoverageValues,
			BakeData.Stats.AtlasWidth,
			Settings.FrameGridSize,
			MeshDescription,
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	const TOptional<FFoliageBakerMeshOutputSelection> MeshOutputSelection =
		FFoliageBakerMeshOutputDialog::OpenAfterBake(SourceStaticMesh, Settings.SourceLODIndex);
	if (!MeshOutputSelection.IsSet())
	{
		Result.bCancelled = true;
		Result.Report = FString::Printf(
			TEXT("%s\n  cancelled after bake: no mesh output was selected and no generated assets were committed."),
			*SourceStaticMesh.GetName());
		return Result;
	}
	if (MeshOutputSelection->OutputMode != EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset
		&& !FFoliageBakerAssetBuilder::ValidateSourceMeshOutputTarget(
			SourceStaticMesh,
			BuildSourceLODAssetParams(Settings, MeshOutputSelection.GetValue()),
			Error))
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	FFoliageBakerGeneratedAssetOutputFolders OutputFolders;
	if (Settings.bPlaceGeneratedAssetsNearReplacedLODAssets
		&& MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::ReplaceSourceMeshLOD)
	{
		OutputFolders = FFoliageBakerAssetBuilder::ResolveSourceLODAssetOutputFolders(
			SourceStaticMesh,
			MeshOutputSelection->ReplaceLODIndex);
	}

	FFoliageBakerAssetTransaction Transaction;
	if (Settings.bBakeBaseColorSdf)
	{
		Result.BaseColorSdfTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			OutputFolders.TexturePackagePath,
			Settings.BaseColorSdfTextureSuffix,
			BakeData.BaseColorPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_World,
			true,
			FColor(0, 0, 0, 0),
			Error);
		if (!Result.BaseColorSdfTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	if (Settings.bBakeNormalDepth)
	{
		Result.NormalDepthTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			OutputFolders.TexturePackagePath,
			Settings.NormalDepthTextureSuffix,
			BakeData.NormalDepthPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_WorldNormalMap,
			false,
			EncodeOctahedralObjectSpaceNormal(
				FVector::UpVector,
				0,
				UnitFloatToByte(0.5f)),
			Error);
		if (!Result.NormalDepthTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	if (Settings.bBakeMix)
	{
		Result.MixTexture = CreateTexture(
			SourceStaticMesh,
			Transaction,
			Settings,
			OutputFolders.TexturePackagePath,
			Settings.MixTextureSuffix,
			BakeData.MixPixels,
			BakeData.Stats,
			TC_BC7,
			TEXTUREGROUP_WorldSpecular,
			false,
			FColor(255, 128, 0, 0),
			Error);
		if (!Result.MixTexture)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
	}

	FFoliageBakerMaterialInstanceAssetParams MaterialParams;
	MaterialParams.OutputFolderName = Settings.MaterialOutputFolderName;
	MaterialParams.OutputPackagePathOverride = OutputFolders.MaterialPackagePath;
	MaterialParams.AssetNamePrefix = Settings.MaterialInstanceNamePrefix;
	MaterialParams.AssetNameSuffix = Settings.MaterialInstanceNameSuffix;
	MaterialParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
	MaterialParams.BaseColorOpacityTextureParameterName = Settings.BaseColorSdfTextureParameterName;
	MaterialParams.NormalDepthTextureParameterName = Settings.NormalDepthTextureParameterName;
	MaterialParams.MixTextureParameterName = Settings.MixTextureParameterName;
	if (!Settings.bBakeMix)
	{
		const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
			ParameterNames = {
				Settings.LeafRoughnessParameterName,
				Settings.LeafSpecularParameterName,
				Settings.TrunkRoughnessParameterName,
				Settings.TrunkSpecularParameterName,
			};
		if (!UE::FoliageBaker::MaterialResolver::ResolveTrunkLeafMaterialScalarParameters(
				BakeData.Stats.MaterialAverages,
				ParameterNames,
				MaterialParams.ScalarParameterValues,
				Error))
		{
			Result.Report = FString::Printf(
				TEXT("%s\n  failed: %s"),
				*SourceStaticMesh.GetName(),
				*Error);
			return Result;
		}
	}
	Result.MaterialInstance = FFoliageBakerAssetBuilder::CreateMaterialInstanceAsset(
		SourceStaticMesh,
		Transaction,
		MaterialParams,
		&MaterialTemplate,
		Result.BaseColorSdfTexture,
		Result.NormalDepthTexture,
		Result.MixTexture,
		Error);
	if (!Result.MaterialInstance)
	{
		Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
		return Result;
	}

	Result.MaterialInstance->PreEditChange(nullptr);
	const float FrameGridSize = static_cast<float>(FMath::Clamp(Settings.FrameGridSize, 3, 8));
	Result.MaterialInstance->SetScalarParameterValueEditorOnly(
		Settings.FramesParameterName,
		FrameGridSize);
	Result.MaterialInstance->SetScalarParameterValueEditorOnly(
		Settings.DefaultMeshSizeParameterName,
		static_cast<float>(BakeData.SharedCaptureHalfExtent * 2.0));
	Result.MaterialInstance->SetVectorParameterValueEditorOnly(
		Settings.PivotOffsetParameterName,
		FLinearColor(
			static_cast<float>(BakeData.SourceBounds.Origin.X),
			static_cast<float>(BakeData.SourceBounds.Origin.Y),
			static_cast<float>(BakeData.SourceBounds.Origin.Z),
			1.0f));
	const auto SetStaticSwitch = [&](const FName ParameterName, const bool bValue)
	{
		Result.MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
			FMaterialParameterInfo(ParameterName),
			bValue);
	};
	SetStaticSwitch(
		Settings.UpperHemisphereStaticSwitchParameterName,
		Settings.Coverage == EFoliageBakerImpostorCoverage::UpperHemisphere);
	Result.MaterialInstance->PostEditChange();
	Result.MaterialInstance->UpdateStaticPermutation();
	Result.MaterialInstance->MarkPackageDirty();

	if (MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
	{
		FFoliageBakerStaticMeshAssetParams MeshParams;
		MeshParams.AssetNameSuffix = TEXT("_ImpostorProxy");
		MeshParams.ExistingAssetPolicy = EFoliageBakerExistingAssetPolicy::ReuseOrCreate;
		MeshParams.DesiredUVChannelCount = 1;
		MeshParams.MaterialSlotName = TEXT("ImpostorProxy");
		MeshParams.bRecomputeNormals = true;
		MeshParams.bRecomputeTangents = true;
		MeshParams.BaseLODModel = 0;
		Result.ProxyMesh = FFoliageBakerAssetBuilder::CreateStaticMeshAsset(
			SourceStaticMesh,
			Transaction,
			MeshParams,
			MeshDescription,
			Result.MaterialInstance,
			Error);
		if (!Result.ProxyMesh)
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
		Result.ProxyMesh->SetNegativeBoundsExtension(FVector::ZeroVector);
		Result.ProxyMesh->SetPositiveBoundsExtension(FVector::ZeroVector);
		Result.ProxyMesh->CalculateExtendedBounds();
		const FBox CurrentBounds = Result.ProxyMesh->GetBounds().GetBox();
		const FVector DesiredMin = BakeData.SourceBounds.Origin - BakeData.SourceBounds.BoxExtent;
		const FVector DesiredMax = BakeData.SourceBounds.Origin + BakeData.SourceBounds.BoxExtent;
		const FVector NegativeExtension(
			FMath::Max(CurrentBounds.Min.X - DesiredMin.X, 0.0),
			FMath::Max(CurrentBounds.Min.Y - DesiredMin.Y, 0.0),
			FMath::Max(CurrentBounds.Min.Z - DesiredMin.Z, 0.0));
		const FVector PositiveExtension(
			FMath::Max(DesiredMax.X - CurrentBounds.Max.X, 0.0),
			FMath::Max(DesiredMax.Y - CurrentBounds.Max.Y, 0.0),
			FMath::Max(DesiredMax.Z - CurrentBounds.Max.Z, 0.0));
		Result.ProxyMesh->SetNegativeBoundsExtension(NegativeExtension);
		Result.ProxyMesh->SetPositiveBoundsExtension(PositiveExtension);
		Result.ProxyMesh->CalculateExtendedBounds();
		Result.ProxyMesh->PostEditChange();
		Result.ProxyMesh->SetExtendedBounds(BakeData.SourceBounds);
		Result.ProxyMesh->MarkPackageDirty();
	}
	else
	{
		int32 InstalledLODIndex = INDEX_NONE;
		if (!FFoliageBakerAssetBuilder::InstallMeshDescriptionAsSourceMeshLOD(
				SourceStaticMesh,
				Transaction,
				BuildSourceLODAssetParams(Settings, MeshOutputSelection.GetValue()),
				MeshDescription,
				Result.MaterialInstance,
				InstalledLODIndex,
				Error))
		{
			Result.Report = FString::Printf(TEXT("%s\n  failed: %s"), *SourceStaticMesh.GetName(), *Error);
			return Result;
		}
		Result.ProxyMesh = &SourceStaticMesh;
		Result.SourceMeshLODIndex = InstalledLODIndex;
	}

	Transaction.Commit();
	Result.bSucceeded = true;
	if (MeshOutputSelection->OutputMode == EFoliageBakerMeshAssetOutputMode::SeparateMeshAsset)
	{
		AppendCreatedAsset(Result.ProxyMesh, Result.CreatedAssets);
	}
	AppendCreatedAsset(Result.BaseColorSdfTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.NormalDepthTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.MixTexture, Result.CreatedAssets);
	AppendCreatedAsset(Result.MaterialInstance, Result.CreatedAssets);
	const int32 AtlasPixelCount = BakeData.Stats.AtlasWidth * BakeData.Stats.AtlasHeight;
	const double PaintedPixelPercent = AtlasPixelCount > 0
		? static_cast<double>(BakeData.Stats.PaintedPixels) * 100.0 / static_cast<double>(AtlasPixelCount)
		: 0.0;
	const double TexelAreaDensityGain = FMath::Square(
		static_cast<double>(BakeData.SourceBounds.SphereRadius) / BakeData.SharedCaptureHalfExtent);
	const UE::FoliageBaker::MaterialResolver::FTrunkLeafMaterialParameterNames
		MaterialScalarParameterNames = {
			Settings.LeafRoughnessParameterName,
			Settings.LeafSpecularParameterName,
			Settings.TrunkRoughnessParameterName,
			Settings.TrunkSpecularParameterName,
		};
	const FString MaterialScalarDetails =
		UE::FoliageBaker::MaterialResolver::BuildTrunkLeafMaterialAveragesReport(
			!Settings.bBakeMix,
			BakeData.Stats.MaterialAverages,
			MaterialScalarParameterNames);
	Result.Report = FString::Printf(
		TEXT("%s\n  Impostor bake succeeded\n  source LOD: %d\n  coverage: %s\n  sampling grid: %dx%d octahedral directions (%d views)\n  atlas: %dx%d, tile=%d\n  projection: shared tight square with up to %d px guard\n  channels: ColorOpacity RGB + SDF A, NormalMask octahedral object/local Normal RG + trunk 0.5/leaf 1 Mask B + Depth A (near 1, far 0, empty 0.5)%s\n  material scalar averages: %s\n  resolve: shared masked RDG depth per frame; BaseColor, Normal, material properties and Source Triangle ID come from the same winning fragment\n  bounds center: (%.3f, %.3f, %.3f), source sphere radius: %.3f cm, shared capture half extent: %.3f cm\n  projected texel area-density gain versus SphereRadius: %.3fx\n  proxy: XY cutout with center + 8 full-resolution conservative support vertices, up to %.0f px cutout guard, +Z facing, source asset Pivot preserved\n  painted pixels: %d/%d (%.2f%%), rasterized triangle references: %d, masked triangle references: %d, depth-correct tiles: %d\n  WPO/displacement: disabled by the Core masked material proxy\n  collision: off, lightmap UV: off, distance fields: on\n  material instance: %s"),
		*SourceStaticMesh.GetName(),
		Settings.SourceLODIndex,
		Settings.Coverage == EFoliageBakerImpostorCoverage::FullSphere ? TEXT("full sphere") : TEXT("upper hemisphere"),
		FMath::Clamp(Settings.FrameGridSize, 3, 8),
		FMath::Clamp(Settings.FrameGridSize, 3, 8),
		BakeData.Stats.ViewCount,
		BakeData.Stats.AtlasWidth,
		BakeData.Stats.AtlasHeight,
		BakeData.Stats.TileResolution,
		ImpostorProjectionGuardPixels,
		Settings.bBakeMix ? TEXT(", Mix RGBA enabled") : TEXT(""),
		*MaterialScalarDetails,
		BakeData.SourceBounds.Origin.X,
		BakeData.SourceBounds.Origin.Y,
		BakeData.SourceBounds.Origin.Z,
		BakeData.SourceBounds.SphereRadius,
		BakeData.SharedCaptureHalfExtent,
		TexelAreaDensityGain,
		ImpostorCutoutGuardPixels,
		BakeData.Stats.PaintedPixels,
		AtlasPixelCount,
		PaintedPixelPercent,
		BakeData.Stats.RasterizedTriangleReferences,
		BakeData.Stats.MaskedTriangleReferences,
		BakeData.Stats.DepthCorrectTileCount,
		*Result.MaterialInstance->GetPathName());
	UE_LOG(LogFoliageBakerImpostor, Display, TEXT("\n%s"), *Result.Report);
	return Result;
}
