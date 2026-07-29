#include "FoliageBakerL1Visibility.h"

#include "FoliageBakerAtlasTools.h"
#include "FoliageBakerMaskedMaterialBaker.h"
#include "FoliageBakerProjectedMaterialBake.h"
#include "Engine/StaticMesh.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialBakingStructures.h"
#include "MeshDescription.h"

namespace UE::FoliageBaker::L1Visibility
{
	namespace
	{
		constexpr int32 L1CoefficientCount = 4;
		constexpr int32 AugmentedMatrixColumnCount = L1CoefficientCount * 2;
		constexpr int32 PcfKernelWidth = 5;
		constexpr int32 PcfKernelRadius = PcfKernelWidth / 2;
		constexpr double ShadowDepthQuantizationBias = 2.0 / 255.0;
		static_assert(PcfKernelWidth % 2 == 1);

		struct FUpperHemisphereShadowProjection
		{
			PlaneCover::FPlaneProxyPlaneInfo PlaneInfo;
			FIntPoint TextureSize = FIntPoint::ZeroValue;
		};

		class FL1VisibilityFitter
		{
		public:
			bool Initialize(
				int32 RequestedSampleCount,
				int32 PixelCount,
				FString& OutError);

			const TArray<FVector>& GetDirections() const
			{
				return Directions;
			}

			void AddVisibility(
				int32 DirectionIndex,
				int32 PixelIndex,
				float Visibility);

			FColor EncodePixelInCaptureFrame(
				int32 PixelIndex,
				const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo) const;

		private:
			TArray<FVector> Directions;
			TArray<FVector4f> SampleBases;
			TArray<FVector4f> NormalEquationRightHandSides;
			double InverseNormalMatrix[L1CoefficientCount][L1CoefficientCount] = {};
		};

		bool BuildInverseNormalMatrix(
			const TArray<FVector>& Directions,
			double OutInverse[L1CoefficientCount][L1CoefficientCount])
		{
			double Augmented[L1CoefficientCount][AugmentedMatrixColumnCount] = {};
			for (const FVector& Direction : Directions)
			{
				const double Basis[L1CoefficientCount] = {1.0, Direction.X, Direction.Y, Direction.Z};
				for (int32 Row = 0; Row < L1CoefficientCount; ++Row)
				{
					for (int32 Column = 0; Column < L1CoefficientCount; ++Column)
					{
						Augmented[Row][Column] += Basis[Row] * Basis[Column];
					}
				}
			}
			for (int32 Index = 0; Index < L1CoefficientCount; ++Index)
			{
				Augmented[Index][Index + L1CoefficientCount] = 1.0;
			}

			for (int32 PivotColumn = 0; PivotColumn < L1CoefficientCount; ++PivotColumn)
			{
				int32 PivotRow = PivotColumn;
				for (int32 CandidateRow = PivotColumn + 1; CandidateRow < L1CoefficientCount; ++CandidateRow)
				{
					if (FMath::Abs(Augmented[CandidateRow][PivotColumn]) > FMath::Abs(Augmented[PivotRow][PivotColumn]))
					{
						PivotRow = CandidateRow;
					}
				}
				if (FMath::Abs(Augmented[PivotRow][PivotColumn]) <= 1.0e-10)
				{
					return false;
				}
				if (PivotRow != PivotColumn)
				{
					for (int32 Column = 0; Column < AugmentedMatrixColumnCount; ++Column)
					{
						Swap(Augmented[PivotRow][Column], Augmented[PivotColumn][Column]);
					}
				}

				const double Pivot = Augmented[PivotColumn][PivotColumn];
				for (int32 Column = 0; Column < AugmentedMatrixColumnCount; ++Column)
				{
					Augmented[PivotColumn][Column] /= Pivot;
				}
				for (int32 Row = 0; Row < L1CoefficientCount; ++Row)
				{
					if (Row == PivotColumn)
					{
						continue;
					}
					const double Scale = Augmented[Row][PivotColumn];
					for (int32 Column = 0; Column < AugmentedMatrixColumnCount; ++Column)
					{
						Augmented[Row][Column] -= Scale * Augmented[PivotColumn][Column];
					}
				}
			}

			for (int32 Row = 0; Row < L1CoefficientCount; ++Row)
			{
				for (int32 Column = 0; Column < L1CoefficientCount; ++Column)
				{
					OutInverse[Row][Column] = Augmented[Row][Column + L1CoefficientCount];
				}
			}
			return true;
		}

		uint8 EncodeUnsigned(const double Value)
		{
			return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Value, 0.0, 1.0) * 255.0), 0, 255));
		}

		uint8 EncodeSigned(const double Value)
		{
			return static_cast<uint8>(
				FMath::Clamp(FMath::RoundToInt((FMath::Clamp(Value, -1.0, 1.0) * 0.5 + 0.5) * 255.0), 0, 255));
		}

		bool BuildShadowProjection(const TArray<PlaneCover::FSourceTriangle>& Triangles,
								   const FBoxSphereBounds& SourceBounds, const FVector& InLightDirection,
								   const int32 MaximumResolution, FUpperHemisphereShadowProjection& OutProjection,
								   FString& OutError)
		{
			OutProjection = FUpperHemisphereShadowProjection();
			const FVector LightDirection = InLightDirection.GetSafeNormal();
			if (LightDirection.IsNearlyZero())
			{
				OutError = TEXT("Upper-hemisphere L1 visibility received a zero light direction.");
				return false;
			}

			FVector AxisU = FMath::Abs(LightDirection.Z) < 0.999
								? FVector::CrossProduct(FVector::UpVector, LightDirection).GetSafeNormal()
								: FVector::ForwardVector;
			if (AxisU.IsNearlyZero())
			{
				AxisU = FVector::ForwardVector;
			}
			const FVector AxisV = FVector::CrossProduct(LightDirection, AxisU).GetSafeNormal();
			if (AxisV.IsNearlyZero())
			{
				OutError = TEXT("Upper-hemisphere L1 visibility could not construct a "
								"shadow projection frame.");
				return false;
			}

			double MinU = TNumericLimits<double>::Max();
			double MaxU = TNumericLimits<double>::Lowest();
			double MinV = TNumericLimits<double>::Max();
			double MaxV = TNumericLimits<double>::Lowest();
			for (const PlaneCover::FSourceTriangle& Triangle : Triangles)
			{
				for (const FVector& Vertex : Triangle.Vertices)
				{
					const double U = FVector::DotProduct(Vertex, AxisU);
					const double V = FVector::DotProduct(Vertex, AxisV);
					MinU = FMath::Min(MinU, U);
					MaxU = FMath::Max(MaxU, U);
					MinV = FMath::Min(MinV, V);
					MaxV = FMath::Max(MaxV, V);
				}
			}

			const double UExtent = MaxU - MinU;
			const double VExtent = MaxV - MinV;
			if (!FMath::IsFinite(UExtent) || !FMath::IsFinite(VExtent) || UExtent <= UE_DOUBLE_SMALL_NUMBER ||
				VExtent <= UE_DOUBLE_SMALL_NUMBER)
			{
				OutError = TEXT("Upper-hemisphere L1 visibility source projection is degenerate.");
				return false;
			}

			const int32 SafeMaximumResolution = FMath::Clamp(MaximumResolution, 64, 1024);
			int32 Width = SafeMaximumResolution;
			int32 Height = SafeMaximumResolution;
			if (UExtent > VExtent)
			{
				Height =
					FMath::Max(16, FMath::RoundToInt(static_cast<double>(SafeMaximumResolution) * VExtent / UExtent));
			}
			else
			{
				Width =
					FMath::Max(16, FMath::RoundToInt(static_cast<double>(SafeMaximumResolution) * UExtent / VExtent));
			}
			Width = FMath::Clamp(Align(Width, 4), 16, SafeMaximumResolution);
			Height = FMath::Clamp(Align(Height, 4), 16, SafeMaximumResolution);

			const int32 InteriorWidth = FMath::Max(1, Width - PcfKernelRadius * 2);
			const int32 InteriorHeight = FMath::Max(1, Height - PcfKernelRadius * 2);
			const double MarginU =
				UExtent * static_cast<double>(PcfKernelRadius) / static_cast<double>(InteriorWidth);
			const double MarginV =
				VExtent * static_cast<double>(PcfKernelRadius) / static_cast<double>(InteriorHeight);
			MinU -= MarginU;
			MaxU += MarginU;
			MinV -= MarginV;
			MaxV += MarginV;

			OutProjection.TextureSize = FIntPoint(Width, Height);
			OutProjection.PlaneInfo.Normal = LightDirection;
			OutProjection.PlaneInfo.Rho = FVector::DotProduct(SourceBounds.Origin, LightDirection);
			OutProjection.PlaneInfo.AxisU = AxisU;
			OutProjection.PlaneInfo.AxisV = AxisV;
			OutProjection.PlaneInfo.MinU = MinU;
			OutProjection.PlaneInfo.MaxU = MaxU;
			OutProjection.PlaneInfo.MinV = MinV;
			OutProjection.PlaneInfo.MaxV = MaxV;
			return true;
		}

		bool BakeShadowDepth(const UStaticMesh& SourceStaticMesh,
							 const FBoxSphereBounds& PrimitiveBounds,
							 const FBoxSphereBounds& SourceBounds,
							 const TArray<PlaneCover::FSourceTriangle>& Triangles,
							 const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
							 const PlaneCover::EAtlasVConvention AtlasVConvention,
							 const FUpperHemisphereShadowProjection& Projection,
							 FFoliageBakerDepthCorrectTileResult& OutResult, FString& OutError)
		{
			TArray<int32> AllTriangleIndices;
			AllTriangleIndices.Reserve(Triangles.Num());
			TSet<int32> ReferencedMaterialSet;
			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				AllTriangleIndices.Add(TriangleIndex);
				ReferencedMaterialSet.Add(Triangles[TriangleIndex].MaterialIndex);
			}
			TArray<int32> ReferencedMaterialIndices = ReferencedMaterialSet.Array();
			ReferencedMaterialIndices.Sort();

			struct FShadowMaterialStorage
			{
				TStrongObjectPtr<UMaterialInterface> MaterialInterface;
				FMeshDescription MeshDescription;
				TArray<FVector2D> CustomTileUVs;
				TArray<int32> RasterSourceTriangleIndices;
				FMeshData MeshSettings;
			};

			const TArray<FStaticMaterial>& SourceMaterials = SourceStaticMesh.GetStaticMaterials();
			TArray<TUniquePtr<FShadowMaterialStorage>> MaterialStorage;
			MaterialStorage.Reserve(ReferencedMaterialIndices.Num());
			FFoliageBakerDepthCorrectTileRequest Request;
			Request.TextureSize = Projection.TextureSize;
			Request.CaptureRayDirection = -Projection.PlaneInfo.Normal;
			Request.ProjectionAxisU = Projection.PlaneInfo.AxisU;
			Request.ProjectionAxisV = Projection.PlaneInfo.AxisV;
			Request.ProjectionMinU = Projection.PlaneInfo.MinU;
			Request.ProjectionMaxU = Projection.PlaneInfo.MaxU;
			Request.ProjectionMinV = Projection.PlaneInfo.MinV;
			Request.ProjectionMaxV = Projection.PlaneInfo.MaxV;
			Request.SourceBounds = SourceBounds;
			Request.bFlipProjectionV =
				AtlasVConvention
				== PlaneCover::EAtlasVConvention::
					GeometryMinVToTextureMaxV;
			Request.bBakeBaseColor = false;
			Request.bBakeObjectSpaceNormal = false;
			Request.bBakePackedMix = false;
			Request.bBakeRoughnessSpecular = false;

			const TArray<PlaneCover::FCrackReductionProjection> NoCrackReduction;
			for (const int32 MaterialIndex : ReferencedMaterialIndices)
			{
				if (!SourceMaterials.IsValidIndex(MaterialIndex))
				{
					OutError = FString::Printf(TEXT("Upper-hemisphere L1 visibility "
													"references invalid material index %d."),
											   MaterialIndex);
					return false;
				}

				TUniquePtr<FShadowMaterialStorage> Storage = MakeUnique<FShadowMaterialStorage>();
				Storage->MaterialInterface =
					BakeMaterialOverrides.ResolveMaterial(MaterialIndex);
				if (!Storage->MaterialInterface)
				{
					Storage->MaterialInterface.Reset(
						SourceMaterials[MaterialIndex]
							.MaterialInterface.Get());
				}
				if (!Storage->MaterialInterface)
				{
					Storage->MaterialInterface.Reset(
						UMaterial::GetDefaultMaterial(MD_Surface));
				}

				ProjectedMaterialBake::FPlaneSideBakeParams BakeParams;
				BakeParams.CaptureRayDirection = Request.CaptureRayDirection;
				BakeParams.AtlasVConvention = AtlasVConvention;
				BakeParams.MaterialIndexFilter = MaterialIndex;
				BakeParams.bBackSide = false;
				int32 MatchingTriangleCount = 0;
				FString InputError;
				if (!ProjectedMaterialBake::BuildPlaneSideBakeInputs(
						Triangles, AllTriangleIndices, NoCrackReduction, Projection.PlaneInfo, BakeParams,
						Storage->MeshDescription, Storage->CustomTileUVs, MatchingTriangleCount, &InputError,
						&Storage->RasterSourceTriangleIndices))
				{
					OutError = FString::Printf(TEXT("Upper-hemisphere L1 visibility could "
													"not build material %d shadow input: %s"),
											   MaterialIndex, *InputError);
					return false;
				}

				Storage->MeshSettings.MeshDescription = &Storage->MeshDescription;
				Storage->MeshSettings.Mesh = &SourceStaticMesh;
				Storage->MeshSettings.MaterialIndices.Add(0);
				Storage->MeshSettings.TextureCoordinateBox = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
				Storage->MeshSettings.TextureCoordinateIndex = 0;
				Storage->MeshSettings.LightMapIndex = 0;
				Storage->MeshSettings.PrimitiveData =
					FPrimitiveData(PrimitiveBounds);
				Storage->MeshSettings.CustomTextureCoordinates = MoveTemp(Storage->CustomTileUVs);
				FShadowMaterialStorage* StoragePtr = Storage.Get();
				MaterialStorage.Add(MoveTemp(Storage));

				FFoliageBakerDepthCorrectTileMaterialInput& MaterialInput = Request.Materials.AddDefaulted_GetRef();
				MaterialInput.MaterialInterface = StoragePtr->MaterialInterface;
				MaterialInput.MeshSettings = &StoragePtr->MeshSettings;
				MaterialInput.RasterSourceTriangleIndices = &StoragePtr->RasterSourceTriangleIndices;
			}

			FString BakeError;
			if (!FFoliageBakerMaskedMaterialBaker::BakeDepthCorrectTile(Request, OutResult, &BakeError))
			{
				OutError = FString::Printf(TEXT("Upper-hemisphere L1 visibility shadow bake failed: %s"), *BakeError);
				return false;
			}
			return true;
		}

		FVector ReconstructSourcePosition(const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo, const FIntPoint& TileSize,
										  const int32 LocalX, const int32 LocalY, const uint8 EncodedLinearDepth,
										  const FVector& CaptureRayDirection, const FBoxSphereBounds& SourceBounds,
										  const PlaneCover::EAtlasVConvention AtlasVConvention)
		{
			const double UFraction = (static_cast<double>(LocalX) + 0.5) / static_cast<double>(TileSize.X);
			const double TextureVFraction = (static_cast<double>(LocalY) + 0.5) / static_cast<double>(TileSize.Y);
			const double PlaneVFraction = AtlasVConvention == PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV
											  ? 1.0 - TextureVFraction
											  : TextureVFraction;
			const double U = FMath::Lerp(PlaneInfo.MinU, PlaneInfo.MaxU, UFraction);
			const double V = FMath::Lerp(PlaneInfo.MinV, PlaneInfo.MaxV, PlaneVFraction);
			const FVector ProjectedPoint = PlaneInfo.Normal * PlaneInfo.Rho + PlaneInfo.AxisU * U + PlaneInfo.AxisV * V;
			const double LinearDepth =
				static_cast<double>(EncodedLinearDepth) / 255.0;
			const double SignedDepth =
				(LinearDepth * 2.0 - 1.0) *
				FMath::Max(static_cast<double>(SourceBounds.SphereRadius), UE_DOUBLE_SMALL_NUMBER);
			const FVector SafeCaptureDirection = CaptureRayDirection.GetSafeNormal();
			const double AlongRay =
				SignedDepth - FVector::DotProduct(ProjectedPoint - SourceBounds.Origin, SafeCaptureDirection);
			return ProjectedPoint + SafeCaptureDirection * AlongRay;
		}

		bool FL1VisibilityFitter::Initialize(
			const int32 RequestedSampleCount,
			const int32 PixelCount,
			FString& OutError)
		{
			Directions.Reset();
			SampleBases.Reset();
			NormalEquationRightHandSides.Reset();
			FMemory::Memzero(InverseNormalMatrix, sizeof(InverseNormalMatrix));

			const int32 SampleCount = FMath::Clamp(RequestedSampleCount, 4, 32);
			Directions.Reserve(SampleCount);
			SampleBases.Reserve(SampleCount);
			constexpr double GoldenAngle = 2.39996322972865332;
			for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
			{
				const double Z = (static_cast<double>(SampleIndex) + 0.5) / static_cast<double>(SampleCount);
				const double Radius = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
				const double Azimuth = GoldenAngle * static_cast<double>(SampleIndex);
				const FVector Direction(Radius * FMath::Cos(Azimuth), Radius * FMath::Sin(Azimuth), Z);
				Directions.Add(Direction);
				SampleBases.Add(FVector4f(
					1.0f,
					static_cast<float>(Direction.X),
					static_cast<float>(Direction.Y),
					static_cast<float>(Direction.Z)));
			}

			if (!BuildInverseNormalMatrix(Directions, InverseNormalMatrix))
			{
				OutError = TEXT("Upper-hemisphere L1 visibility sample basis is singular.");
				return false;
			}

			NormalEquationRightHandSides.SetNumZeroed(FMath::Max(0, PixelCount));
			return true;
		}

		void FL1VisibilityFitter::AddVisibility(
			const int32 DirectionIndex,
			const int32 PixelIndex,
			const float Visibility)
		{
			if (!SampleBases.IsValidIndex(DirectionIndex)
				|| !NormalEquationRightHandSides.IsValidIndex(PixelIndex))
			{
				return;
			}
			NormalEquationRightHandSides[PixelIndex] +=
				SampleBases[DirectionIndex] * FMath::Clamp(Visibility, 0.0f, 1.0f);
		}

		FColor FL1VisibilityFitter::EncodePixelInCaptureFrame(
			const int32 PixelIndex,
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo) const
		{
			if (!NormalEquationRightHandSides.IsValidIndex(PixelIndex))
			{
				return FColor(128, 128, 128, 255);
			}

			const FVector4f& RightHandSide = NormalEquationRightHandSides[PixelIndex];
			const double RightHandSideValues[L1CoefficientCount] = {
				RightHandSide.X,
				RightHandSide.Y,
				RightHandSide.Z,
				RightHandSide.W};
			double Coefficients[L1CoefficientCount] = {};
			for (int32 Row = 0; Row < L1CoefficientCount; ++Row)
			{
				for (int32 Column = 0; Column < L1CoefficientCount; ++Column)
				{
					Coefficients[Row] +=
						InverseNormalMatrix[Row][Column] * RightHandSideValues[Column];
				}
			}

			const FVector DirectionalCoefficients(
				Coefficients[1],
				Coefficients[2],
				Coefficients[3]);
			const FVector CaptureNormal = PlaneInfo.Normal.GetSafeNormal();
			const FVector CaptureAxisU = PlaneInfo.AxisU.GetSafeNormal();
			const FVector CaptureAxisV = PlaneInfo.AxisV.GetSafeNormal();
			// The runtime evaluates the texture against BillboardSunDirLocal, whose axes
			// correspond to the capture plane's Normal, AxisU, and AxisV respectively.
			return FColor(
				EncodeSigned(FVector::DotProduct(DirectionalCoefficients, CaptureNormal)),
				EncodeSigned(FVector::DotProduct(DirectionalCoefficients, CaptureAxisU)),
				EncodeSigned(FVector::DotProduct(DirectionalCoefficients, CaptureAxisV)),
				EncodeUnsigned(Coefficients[0]));
		}

		float SampleShadowVisibilityPcf5x5(
			const FVector& SourcePosition,
			const FUpperHemisphereShadowProjection& Projection,
			const FBoxSphereBounds& SourceBounds,
			const PlaneCover::EAtlasVConvention AtlasVConvention,
			const TArray<FColor>& SourceTriangleIdAndDepth)
		{
			if (Projection.TextureSize.X <= 0 || Projection.TextureSize.Y <= 0)
			{
				return 1.0f;
			}

			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo = Projection.PlaneInfo;
			const double UExtent = PlaneInfo.MaxU - PlaneInfo.MinU;
			const double VExtent = PlaneInfo.MaxV - PlaneInfo.MinV;
			if (UExtent <= UE_DOUBLE_SMALL_NUMBER || VExtent <= UE_DOUBLE_SMALL_NUMBER)
			{
				return 1.0f;
			}

			const double UFraction =
				(FVector::DotProduct(SourcePosition, PlaneInfo.AxisU) - PlaneInfo.MinU) / UExtent;
			const double PlaneVFraction =
				(FVector::DotProduct(SourcePosition, PlaneInfo.AxisV) - PlaneInfo.MinV) / VExtent;
			const double TextureVFraction =
				AtlasVConvention == PlaneCover::EAtlasVConvention::GeometryMinVToTextureMaxV
					? 1.0 - PlaneVFraction
					: PlaneVFraction;
			if (UFraction < 0.0 || UFraction > 1.0 || TextureVFraction < 0.0 || TextureVFraction > 1.0)
			{
				return 1.0f;
			}

			const double PixelX = UFraction * static_cast<double>(Projection.TextureSize.X) - 0.5;
			const double PixelY = TextureVFraction * static_cast<double>(Projection.TextureSize.Y) - 0.5;
			const int32 CenterX = FMath::RoundToInt(PixelX);
			const int32 CenterY = FMath::RoundToInt(PixelY);

			const double DepthRadius =
				FMath::Max(static_cast<double>(SourceBounds.SphereRadius), UE_DOUBLE_SMALL_NUMBER);
			const double ReceiverSignedDepth =
				FVector::DotProduct(SourcePosition - SourceBounds.Origin, -PlaneInfo.Normal);
			const double ReceiverLinearDepth =
				FMath::Clamp((ReceiverSignedDepth + DepthRadius) / (2.0 * DepthRadius), 0.0, 1.0);

			double VisibilitySum = 0.0;
			int32 ValidTapCount = 0;
			for (int32 OffsetY = -PcfKernelRadius; OffsetY <= PcfKernelRadius; ++OffsetY)
			{
				const int32 Y = CenterY + OffsetY;
				if (Y < 0 || Y >= Projection.TextureSize.Y)
				{
					continue;
				}
				for (int32 OffsetX = -PcfKernelRadius; OffsetX <= PcfKernelRadius; ++OffsetX)
				{
					const int32 X = CenterX + OffsetX;
					if (X < 0 || X >= Projection.TextureSize.X)
					{
						continue;
					}

					double TapVisibility = 1.0;
					const int32 PixelIndex = Y * Projection.TextureSize.X + X;
					if (SourceTriangleIdAndDepth.IsValidIndex(PixelIndex))
					{
						const FColor& Encoded = SourceTriangleIdAndDepth[PixelIndex];
						if (FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(Encoded) != INDEX_NONE)
						{
							const double FrontmostLinearDepth =
								static_cast<double>(Encoded.A) / 255.0;
							TapVisibility =
								ReceiverLinearDepth <= FrontmostLinearDepth + ShadowDepthQuantizationBias
									? 1.0
									: 0.0;
						}
					}
					VisibilitySum += TapVisibility;
					++ValidTapCount;
				}
			}

			return ValidTapCount > 0
				? static_cast<float>(VisibilitySum / static_cast<double>(ValidTapCount))
				: 1.0f;
		}

	} // namespace

	bool BakeUpperHemisphere(const UStaticMesh& SourceStaticMesh,
							 const FBoxSphereBounds& PrimitiveBounds,
							 const FBoxSphereBounds& SourceBounds,
							 const TArray<PlaneCover::FSourceTriangle>& Triangles,
							 const TArray<PlaneCover::FSourceTriangle>& BakeTriangles,
							 const FFoliageBakerBakeMaterialOverrideSet& BakeMaterialOverrides,
							 const TArray<PlaneCover::FPlaneProxyPlaneInfo>& PlaneInfos,
							 const PlaneCover::FPlaneProxySettings& Settings,
							 const TArray<FColor>& SourceTriangleIdAndDepthPixels, const int32 AtlasWidth,
							 const int32 AtlasHeight, const int32 RequestedSampleCount, const int32 ShadowMapResolution,
							 TArray<FColor>& OutPixels, FString& OutError)
	{
		const int32 AtlasPixelCount = AtlasWidth * AtlasHeight;
		if (AtlasWidth <= 0 || AtlasHeight <= 0 || SourceTriangleIdAndDepthPixels.Num() != AtlasPixelCount)
		{
			OutError = TEXT("Upper-hemisphere L1 visibility receiver data does not "
							"match the atlas dimensions.");
			return false;
		}

		TArray<FVector> ReceiverPositions;
		ReceiverPositions.SetNumZeroed(AtlasPixelCount);
		TArray<int32> ReceiverPlaneIndices;
		ReceiverPlaneIndices.Init(INDEX_NONE, AtlasPixelCount);
		TBitArray<> ReceiverCoverage;
		ReceiverCoverage.Init(false, AtlasPixelCount);
		int32 ReceiverCount = 0;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			const PlaneCover::FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			const FIntPoint TileSize = PlaneInfo.AtlasTileSize;
			const FVector CaptureRayDirection = -PlaneInfo.Normal;
			for (int32 LocalY = 0; LocalY < TileSize.Y; ++LocalY)
			{
				const int32 AtlasY = PlaneInfo.AtlasPixelMin.Y + LocalY;
				if (AtlasY < 0 || AtlasY >= AtlasHeight)
				{
					continue;
				}
				for (int32 LocalX = 0; LocalX < TileSize.X; ++LocalX)
				{
					const int32 AtlasX = PlaneInfo.AtlasPixelMin.X + LocalX;
					if (AtlasX < 0 || AtlasX >= AtlasWidth)
					{
						continue;
					}
					const int32 AtlasPixelIndex = AtlasY * AtlasWidth + AtlasX;
					const FColor& Encoded = SourceTriangleIdAndDepthPixels[AtlasPixelIndex];
					if (FFoliageBakerMaskedMaterialBaker::DecodeSourceTriangleId(Encoded) == INDEX_NONE)
					{
						continue;
					}
					ReceiverPositions[AtlasPixelIndex] =
						ReconstructSourcePosition(
							PlaneInfo,
							TileSize,
							LocalX,
							LocalY,
							Encoded.A,
							CaptureRayDirection,
							SourceBounds,
							Settings.AtlasVConvention);
					ReceiverPlaneIndices[AtlasPixelIndex] = PlaneIndex;
					ReceiverCoverage[AtlasPixelIndex] = true;
					++ReceiverCount;
				}
			}
		}
		if (ReceiverCount == 0)
		{
			OutError = TEXT("Upper-hemisphere L1 visibility found no visible receiver pixels.");
			return false;
		}

		FL1VisibilityFitter Fitter;
		if (!Fitter.Initialize(RequestedSampleCount, AtlasPixelCount, OutError))
		{
			return false;
		}

		const TArray<FVector>& Directions = Fitter.GetDirections();
		for (int32 DirectionIndex = 0; DirectionIndex < Directions.Num(); ++DirectionIndex)
		{
			FUpperHemisphereShadowProjection Projection;
			if (!BuildShadowProjection(Triangles, SourceBounds, Directions[DirectionIndex], ShadowMapResolution,
									   Projection, OutError))
			{
				return false;
			}

			FFoliageBakerDepthCorrectTileResult ShadowResult;
			if (!BakeShadowDepth(SourceStaticMesh, PrimitiveBounds, SourceBounds,
								 BakeTriangles, BakeMaterialOverrides,
								 Settings.AtlasVConvention, Projection,
								 ShadowResult, OutError))
			{
				return false;
			}
			const int32 ExpectedShadowPixels = Projection.TextureSize.X * Projection.TextureSize.Y;
			if (ShadowResult.SourceTriangleIdAndDepth.Num() != ExpectedShadowPixels)
			{
				OutError = TEXT("Upper-hemisphere L1 visibility shadow readback returned "
								"an invalid size.");
				return false;
			}

			for (TConstSetBitIterator<> ReceiverIt(ReceiverCoverage); ReceiverIt; ++ReceiverIt)
			{
				const int32 PixelIndex = ReceiverIt.GetIndex();
				const float Visibility = SampleShadowVisibilityPcf5x5(
					ReceiverPositions[PixelIndex],
					Projection,
					SourceBounds,
					Settings.AtlasVConvention,
					ShadowResult.SourceTriangleIdAndDepth);
				Fitter.AddVisibility(DirectionIndex, PixelIndex, Visibility);
			}
		}

		OutPixels.Init(FColor(128, 128, 128, 255), AtlasPixelCount);
		for (TConstSetBitIterator<> ReceiverIt(ReceiverCoverage); ReceiverIt; ++ReceiverIt)
		{
			const int32 PixelIndex = ReceiverIt.GetIndex();
			const int32 PlaneIndex = ReceiverPlaneIndices[PixelIndex];
			if (PlaneInfos.IsValidIndex(PlaneIndex))
			{
				OutPixels[PixelIndex] =
					Fitter.EncodePixelInCaptureFrame(PixelIndex, PlaneInfos[PlaneIndex]);
			}
		}
		Atlas::FillTransparentRGBInsideTiles(OutPixels, AtlasWidth, AtlasHeight, PlaneInfos, &ReceiverCoverage, true);
		return true;
	}
} // namespace UE::FoliageBaker::L1Visibility
