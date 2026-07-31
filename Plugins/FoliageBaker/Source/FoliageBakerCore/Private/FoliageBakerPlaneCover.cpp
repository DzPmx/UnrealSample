#include "FoliageBakerPlaneCover.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "RawIndexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace UE::FoliageBaker::PlaneCover
{
	namespace
	{
		constexpr double DegenerateTriangleTolerance = 1.0e-8;

		struct FPreparedProxyPlane
		{
			int32 SourcePlaneIndex = INDEX_NONE;
			bool bIsTrunkCard = false;
			FVector OrientedNormal = FVector::UpVector;
			double OrientedRho = 0.0;
			FVector AxisU = FVector::RightVector;
			FVector AxisV = FVector::UpVector;
			FVector ShadingNormal = FVector::UpVector;
			double MinU = 0.0;
			double MaxU = 0.0;
			double MinV = 0.0;
			double MaxV = 0.0;
			double MinSignedDistance = 0.0;
			double MaxSignedDistance = 0.0;
			double EnvelopeMinU = 0.0;
			double EnvelopeMaxU = 0.0;
			double EnvelopeMinV = 0.0;
			double EnvelopeMaxV = 0.0;
			double EnvelopeMinSignedDistance = 0.0;
			double EnvelopeMaxSignedDistance = 0.0;
			FVector Corners[4] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
			FVector2f AtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
			FVector2f BackAtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
			FIntPoint AtlasPixelMin = FIntPoint::ZeroValue;
			FIntPoint AtlasTileSize = FIntPoint::ZeroValue;
			FIntPoint BackAtlasPixelMin = FIntPoint::ZeroValue;
			FIntPoint BackAtlasTileSize = FIntPoint::ZeroValue;
			int32 AtlasTilePaddingPixels = 0;
			bool bHasBackFaceAtlas = false;
			double PlaneToShadingNormalDot = 1.0;
			TArray<int32> TriangleIndices;
			TArray<FCrackReductionProjection> CrackReductionProjections;
		};

		void BuildPlaneFrame(const FVector& Normal, FVector& OutAxisU, FVector& OutAxisV)
		{
			const FVector ReferenceAxis = FMath::Abs(Normal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
			OutAxisU = FVector::CrossProduct(ReferenceAxis, Normal).GetSafeNormal();
			if (OutAxisU.IsNearlyZero())
			{
				OutAxisU = FVector::RightVector;
			}
			OutAxisV = FVector::CrossProduct(Normal, OutAxisU).GetSafeNormal();
		}

		double Cross2D(const FVector2D& Origin, const FVector2D& A, const FVector2D& B)
		{
			return (A.X - Origin.X) * (B.Y - Origin.Y) - (A.Y - Origin.Y) * (B.X - Origin.X);
		}

		double Dot2D(const FVector2D& A, const FVector2D& B)
		{
			return A.X * B.X + A.Y * B.Y;
		}

		bool BuildConvexHull2D(TArray<FVector2D> Points, TArray<FVector2D>& OutHull)
		{
			OutHull.Reset();
			if (Points.IsEmpty())
			{
				return false;
			}

			Points.Sort([](const FVector2D& A, const FVector2D& B)
			{
				if (!FMath::IsNearlyEqual(A.X, B.X, 1.0e-6))
				{
					return A.X < B.X;
				}
				return A.Y < B.Y;
			});

			TArray<FVector2D> UniquePoints;
			UniquePoints.Reserve(Points.Num());
			for (const FVector2D& Point : Points)
			{
				if (UniquePoints.IsEmpty()
					|| !FMath::IsNearlyEqual(Point.X, UniquePoints.Last().X, 1.0e-6)
					|| !FMath::IsNearlyEqual(Point.Y, UniquePoints.Last().Y, 1.0e-6))
				{
					UniquePoints.Add(Point);
				}
			}

			if (UniquePoints.Num() <= 2)
			{
				OutHull = MoveTemp(UniquePoints);
				return true;
			}

			TArray<FVector2D> Hull;
			Hull.Reserve(UniquePoints.Num() * 2);
			for (const FVector2D& Point : UniquePoints)
			{
				while (Hull.Num() >= 2 && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 1.0e-6)
				{
					Hull.Pop(EAllowShrinking::No);
				}
				Hull.Add(Point);
			}

			const int32 LowerHullCount = Hull.Num();
			for (int32 PointIndex = UniquePoints.Num() - 2; PointIndex >= 0; --PointIndex)
			{
				const FVector2D& Point = UniquePoints[PointIndex];
				while (Hull.Num() > LowerHullCount && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 1.0e-6)
				{
					Hull.Pop(EAllowShrinking::No);
				}
				Hull.Add(Point);
			}

			if (Hull.Num() > 1)
			{
				Hull.Pop(EAllowShrinking::No);
			}

			OutHull = MoveTemp(Hull);
			return !OutHull.IsEmpty();
		}

		bool ComputeMinimumAreaRectangleAxes2D(const TArray<FVector2D>& Points, FVector2D& OutAxisU, FVector2D& OutAxisV)
		{
			TArray<FVector2D> Hull;
			if (!BuildConvexHull2D(Points, Hull))
			{
				return false;
			}

			OutAxisU = FVector2D(1.0, 0.0);
			OutAxisV = FVector2D(0.0, 1.0);
			if (Hull.Num() == 1)
			{
				return true;
			}

			double BestArea = TNumericLimits<double>::Max();
			for (int32 EdgeIndex = 0; EdgeIndex < Hull.Num(); ++EdgeIndex)
			{
				const FVector2D& A = Hull[EdgeIndex];
				const FVector2D& B = Hull[(EdgeIndex + 1) % Hull.Num()];
				const FVector2D Edge(B.X - A.X, B.Y - A.Y);
				const double EdgeLength = FMath::Sqrt(Edge.X * Edge.X + Edge.Y * Edge.Y);
				if (EdgeLength <= 1.0e-6)
				{
					continue;
				}

				const FVector2D AxisU(Edge.X / EdgeLength, Edge.Y / EdgeLength);
				const FVector2D AxisV(-AxisU.Y, AxisU.X);
				double MinU = TNumericLimits<double>::Max();
				double MaxU = -TNumericLimits<double>::Max();
				double MinV = TNumericLimits<double>::Max();
				double MaxV = -TNumericLimits<double>::Max();
				for (const FVector2D& Point : Hull)
				{
					const double U = Dot2D(Point, AxisU);
					const double V = Dot2D(Point, AxisV);
					MinU = FMath::Min(MinU, U);
					MaxU = FMath::Max(MaxU, U);
					MinV = FMath::Min(MinV, V);
					MaxV = FMath::Max(MaxV, V);
				}

				const double Area = FMath::Max(0.0, MaxU - MinU) * FMath::Max(0.0, MaxV - MinV);
				if (Area < BestArea)
				{
					BestArea = Area;
					OutAxisU = AxisU;
					OutAxisV = AxisV;
				}
			}

			return FMath::IsFinite(BestArea);
		}

		bool ComputeMinimumAreaPlaneRectangle(
			const TArray<FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const FVector& PlaneNormal,
			const double PlaneRho,
			FVector& InOutAxisU,
			FVector& InOutAxisV,
			double& OutMinU,
			double& OutMaxU,
			double& OutMinV,
			double& OutMaxV)
		{
			TArray<FVector2D> ProjectedPoints;
			ProjectedPoints.Reserve(TriangleIndices.Num() * 3);
			for (const int32 TriangleIndex : TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				for (const FVector& Vertex : Triangle.Vertices)
				{
					const FVector ProjectedVertex = ProjectPointToPlane(Vertex, PlaneNormal, PlaneRho);
					ProjectedPoints.Emplace(
						FVector::DotProduct(ProjectedVertex, InOutAxisU),
						FVector::DotProduct(ProjectedVertex, InOutAxisV));
				}
			}

			if (ProjectedPoints.IsEmpty())
			{
				return false;
			}

			FVector2D AxisU2D;
			FVector2D AxisV2D;
			if (!ComputeMinimumAreaRectangleAxes2D(ProjectedPoints, AxisU2D, AxisV2D))
			{
				return false;
			}

			const FVector OriginalAxisU = InOutAxisU;
			const FVector OriginalAxisV = InOutAxisV;
			const FVector NewAxisU = (OriginalAxisU * AxisU2D.X + OriginalAxisV * AxisU2D.Y).GetSafeNormal();
			const FVector NewAxisV = FVector::CrossProduct(PlaneNormal, NewAxisU).GetSafeNormal();
			if (NewAxisU.IsNearlyZero() || NewAxisV.IsNearlyZero())
			{
				return false;
			}

			InOutAxisU = NewAxisU;
			InOutAxisV = NewAxisV;
			OutMinU = TNumericLimits<double>::Max();
			OutMaxU = -TNumericLimits<double>::Max();
			OutMinV = TNumericLimits<double>::Max();
			OutMaxV = -TNumericLimits<double>::Max();

			for (const FVector2D& Point : ProjectedPoints)
			{
				const double U = Dot2D(Point, AxisU2D);
				const double V = Dot2D(Point, AxisV2D);
				OutMinU = FMath::Min(OutMinU, U);
				OutMaxU = FMath::Max(OutMaxU, U);
				OutMinV = FMath::Min(OutMinV, V);
				OutMaxV = FMath::Max(OutMaxV, V);
			}

			return FMath::IsFinite(OutMinU)
				&& FMath::IsFinite(OutMaxU)
				&& FMath::IsFinite(OutMinV)
				&& FMath::IsFinite(OutMaxV);
		}

		bool ComputeFixedFramePlaneRectangle(
			const TArray<FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const FVector& PlaneNormal,
			const double PlaneRho,
			const FVector& AxisU,
			const FVector& AxisV,
			double& OutMinU,
			double& OutMaxU,
			double& OutMinV,
			double& OutMaxV)
		{
			OutMinU = TNumericLimits<double>::Max();
			OutMaxU = -TNumericLimits<double>::Max();
			OutMinV = TNumericLimits<double>::Max();
			OutMaxV = -TNumericLimits<double>::Max();

			for (const int32 TriangleIndex : TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				for (const FVector& Vertex : Triangle.Vertices)
				{
					const FVector ProjectedVertex = ProjectPointToPlane(Vertex, PlaneNormal, PlaneRho);
					const double U = FVector::DotProduct(ProjectedVertex, AxisU);
					const double V = FVector::DotProduct(ProjectedVertex, AxisV);
					OutMinU = FMath::Min(OutMinU, U);
					OutMaxU = FMath::Max(OutMaxU, U);
					OutMinV = FMath::Min(OutMinV, V);
					OutMaxV = FMath::Max(OutMaxV, V);
				}
			}

			return FMath::IsFinite(OutMinU)
				&& FMath::IsFinite(OutMaxU)
				&& FMath::IsFinite(OutMinV)
				&& FMath::IsFinite(OutMaxV);
		}

		double ComputeSourceBoundsMaxDimension(const TArray<FSourceTriangle>& Triangles)
		{
			FBox Bounds(ForceInit);
			for (const FSourceTriangle& Triangle : Triangles)
			{
				for (const FVector& Vertex : Triangle.Vertices)
				{
					Bounds += Vertex;
				}
			}

			if (!Bounds.IsValid)
			{
				return 1.0;
			}

			const FVector Extent = Bounds.GetSize();
			return FMath::Max3(Extent.X, Extent.Y, Extent.Z);
		}

		void SetAtlasUVsFromTile(
			const FIntPoint& AtlasPixelMin,
			const FIntPoint& AtlasTileSize,
			const int32 AtlasWidth,
			const int32 AtlasHeight,
			const EAtlasVConvention AtlasVConvention,
			FVector2f OutAtlasUVs[4])
		{
			const double SafeAtlasWidth = static_cast<double>(FMath::Max(1, AtlasWidth));
			const double SafeAtlasHeight = static_cast<double>(FMath::Max(1, AtlasHeight));


			const double MinU = (static_cast<double>(AtlasPixelMin.X) + 0.5) / SafeAtlasWidth;
			const double MinV = (static_cast<double>(AtlasPixelMin.Y) + 0.5) / SafeAtlasHeight;
			const double MaxU = (static_cast<double>(AtlasPixelMin.X + FMath::Max(1, AtlasTileSize.X)) - 0.5) / SafeAtlasWidth;
			const double MaxV = (static_cast<double>(AtlasPixelMin.Y + FMath::Max(1, AtlasTileSize.Y)) - 0.5) / SafeAtlasHeight;

			if (AtlasVConvention == EAtlasVConvention::GeometryMinVToTextureMaxV)
			{


				OutAtlasUVs[0] = FVector2f(static_cast<float>(MinU), static_cast<float>(MaxV));
				OutAtlasUVs[1] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MaxV));
				OutAtlasUVs[2] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MinV));
				OutAtlasUVs[3] = FVector2f(static_cast<float>(MinU), static_cast<float>(MinV));
			}
			else
			{
				OutAtlasUVs[0] = FVector2f(static_cast<float>(MinU), static_cast<float>(MinV));
				OutAtlasUVs[1] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MinV));
				OutAtlasUVs[2] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MaxV));
				OutAtlasUVs[3] = FVector2f(static_cast<float>(MinU), static_cast<float>(MaxV));
			}
		}

		bool ShouldBakeBackFaceAtlas(const FPreparedProxyPlane& Plane, const FPlaneProxySettings& Settings)
		{
			switch (Settings.DoubleSidedBakeMode)
			{
			case EDoubleSidedBakeMode::TrunkCardsOnly:
				return Plane.bIsTrunkCard;
			case EDoubleSidedBakeMode::BillboardPlanesOnly:
				return !Plane.bIsTrunkCard;
			case EDoubleSidedBakeMode::AllPlanes:
				return true;
			case EDoubleSidedBakeMode::Off:
			default:
				return false;
			}
		}

		struct FAtlasPackRect
		{
			int32 PlaneIndex = INDEX_NONE;
			bool bBackFace = false;
			FIntPoint PitchSize = FIntPoint::ZeroValue;
			FIntPoint InteriorSize = FIntPoint::ZeroValue;
		};

		struct FAtlasFreeRect
		{
			int32 X = 0;
			int32 Y = 0;
			int32 W = 0;
			int32 H = 0;
		};

		bool DoRectsIntersect(const FAtlasFreeRect& A, const FAtlasFreeRect& B)
		{
			return A.X < B.X + B.W
				&& A.X + A.W > B.X
				&& A.Y < B.Y + B.H
				&& A.Y + A.H > B.Y;
		}

		bool DoesRectContainRect(const FAtlasFreeRect& Outer, const FAtlasFreeRect& Inner)
		{
			return Inner.X >= Outer.X
				&& Inner.Y >= Outer.Y
				&& Inner.X + Inner.W <= Outer.X + Outer.W
				&& Inner.Y + Inner.H <= Outer.Y + Outer.H;
		}

		void PruneContainedFreeRects(TArray<FAtlasFreeRect>& FreeRects)
		{
			for (int32 RectIndex = 0; RectIndex < FreeRects.Num(); ++RectIndex)
			{
				for (int32 OtherIndex = RectIndex + 1; OtherIndex < FreeRects.Num();)
				{
					if (DoesRectContainRect(FreeRects[RectIndex], FreeRects[OtherIndex]))
					{
						FreeRects.RemoveAtSwap(OtherIndex, 1, EAllowShrinking::No);
					}
					else if (DoesRectContainRect(FreeRects[OtherIndex], FreeRects[RectIndex]))
					{
						FreeRects.RemoveAtSwap(RectIndex, 1, EAllowShrinking::No);
						--RectIndex;
						break;
					}
					else
					{
						++OtherIndex;
					}
				}
			}
		}

		void SplitFreeRects(TArray<FAtlasFreeRect>& FreeRects, const FAtlasFreeRect& UsedRect)
		{
			TArray<FAtlasFreeRect> NewFreeRects;
			for (const FAtlasFreeRect& FreeRect : FreeRects)
			{
				if (!DoRectsIntersect(FreeRect, UsedRect))
				{
					NewFreeRects.Add(FreeRect);
					continue;
				}

				if (UsedRect.X > FreeRect.X)
				{
					NewFreeRects.Add({ FreeRect.X, FreeRect.Y, UsedRect.X - FreeRect.X, FreeRect.H });
				}
				if (UsedRect.X + UsedRect.W < FreeRect.X + FreeRect.W)
				{
					const int32 NewX = UsedRect.X + UsedRect.W;
					NewFreeRects.Add({ NewX, FreeRect.Y, FreeRect.X + FreeRect.W - NewX, FreeRect.H });
				}
				if (UsedRect.Y > FreeRect.Y)
				{
					NewFreeRects.Add({ FreeRect.X, FreeRect.Y, FreeRect.W, UsedRect.Y - FreeRect.Y });
				}
				if (UsedRect.Y + UsedRect.H < FreeRect.Y + FreeRect.H)
				{
					const int32 NewY = UsedRect.Y + UsedRect.H;
					NewFreeRects.Add({ FreeRect.X, NewY, FreeRect.W, FreeRect.Y + FreeRect.H - NewY });
				}
			}

			FreeRects = MoveTemp(NewFreeRects);
			PruneContainedFreeRects(FreeRects);
		}

		bool TryPackAtlasRects(
			TArray<FAtlasPackRect>& PackRects,
			const int32 AtlasResolution,
			TArray<FIntPoint>& OutInteriorMins)
		{
			OutInteriorMins.SetNum(PackRects.Num());
			TArray<int32> SortedRectIndices;
			SortedRectIndices.Reserve(PackRects.Num());
			for (int32 RectIndex = 0; RectIndex < PackRects.Num(); ++RectIndex)
			{
				const FAtlasPackRect& Rect = PackRects[RectIndex];
				if (Rect.PitchSize.X <= 0 || Rect.PitchSize.Y <= 0 || Rect.PitchSize.X > AtlasResolution || Rect.PitchSize.Y > AtlasResolution)
				{
					return false;
				}
				SortedRectIndices.Add(RectIndex);
			}

			SortedRectIndices.Sort([&PackRects](const int32 A, const int32 B)
			{
				const int64 AreaA = static_cast<int64>(PackRects[A].PitchSize.X) * static_cast<int64>(PackRects[A].PitchSize.Y);
				const int64 AreaB = static_cast<int64>(PackRects[B].PitchSize.X) * static_cast<int64>(PackRects[B].PitchSize.Y);
				if (AreaA != AreaB)
				{
					return AreaA > AreaB;
				}
				if (PackRects[A].PitchSize.Y != PackRects[B].PitchSize.Y)
				{
					return PackRects[A].PitchSize.Y > PackRects[B].PitchSize.Y;
				}
				return PackRects[A].PitchSize.X > PackRects[B].PitchSize.X;
			});

			TArray<FAtlasFreeRect> FreeRects;
			FreeRects.Add({ 0, 0, AtlasResolution, AtlasResolution });

			for (const int32 RectIndex : SortedRectIndices)
			{
				const FAtlasPackRect& Rect = PackRects[RectIndex];
				int32 BestFreeRectIndex = INDEX_NONE;
				int64 BestAreaWaste = TNumericLimits<int64>::Max();
				int32 BestShortSideWaste = TNumericLimits<int32>::Max();
				for (int32 FreeRectIndex = 0; FreeRectIndex < FreeRects.Num(); ++FreeRectIndex)
				{
					const FAtlasFreeRect& FreeRect = FreeRects[FreeRectIndex];
					if (Rect.PitchSize.X > FreeRect.W || Rect.PitchSize.Y > FreeRect.H)
					{
						continue;
					}

					const int64 AreaWaste = static_cast<int64>(FreeRect.W) * static_cast<int64>(FreeRect.H)
						- static_cast<int64>(Rect.PitchSize.X) * static_cast<int64>(Rect.PitchSize.Y);
					const int32 ShortSideWaste = FMath::Min(FreeRect.W - Rect.PitchSize.X, FreeRect.H - Rect.PitchSize.Y);
					if (AreaWaste < BestAreaWaste || (AreaWaste == BestAreaWaste && ShortSideWaste < BestShortSideWaste))
					{
						BestAreaWaste = AreaWaste;
						BestShortSideWaste = ShortSideWaste;
						BestFreeRectIndex = FreeRectIndex;
					}
				}

				if (BestFreeRectIndex == INDEX_NONE)
				{
					return false;
				}

				const FAtlasFreeRect UsedRect =
				{
					FreeRects[BestFreeRectIndex].X,
					FreeRects[BestFreeRectIndex].Y,
					Rect.PitchSize.X,
					Rect.PitchSize.Y
				};
				OutInteriorMins[RectIndex] = FIntPoint(UsedRect.X, UsedRect.Y);
				SplitFreeRects(FreeRects, UsedRect);
			}

			return true;
		}

		template <typename BuildPackRectsType>
		bool ResolveAtlasPacking(
			const FPlaneProxySettings& Settings,
			const double MaxPlaneDimension,
			BuildPackRectsType&& BuildPackRects,
			int32& OutAtlasResolution,
			TArray<FAtlasPackRect>& OutPackRects,
			TArray<FIntPoint>& OutInteriorMins)
		{
			const bool bUseWorldTexelSize =
				Settings.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize;
			const int32 MaximumAtlasResolution = bUseWorldTexelSize
				? TextureResolution::FloorToSupportedPowerOfTwo(
					Settings.TextureAtlasResolution)
				: FMath::Clamp(
					Settings.TextureAtlasResolution,
					TextureResolution::MinimumSupportedAtlasResolution,
					TextureResolution::MaximumSupportedAtlasResolution);

			TArray<FAtlasPackRect> CandidatePackRects;
			TArray<FIntPoint> CandidateInteriorMins;
			if (bUseWorldTexelSize)
			{
				const int32 MinimumAtlasResolution =
					TextureResolution::ResolveMinimumAtlasResolution(
						Settings.MinimumTextureAtlasResolution,
						MaximumAtlasResolution);
				const double TargetPixelsPerCentimeter =
					FMath::Max(Settings.TargetTexelsPerMeter, 0.01)
					/ TextureResolution::CentimetersPerMeter;
				// Keep target-sized tiles unchanged and leave unused power-of-two
				// atlas space empty so different assets retain the same density.
				if (MaxPlaneDimension * TargetPixelsPerCentimeter
					<= static_cast<double>(MaximumAtlasResolution))
				{
					BuildPackRects(TargetPixelsPerCentimeter, CandidatePackRects);
					for (int32 AtlasResolution = MinimumAtlasResolution;
						AtlasResolution <= MaximumAtlasResolution;
						AtlasResolution *= 2)
					{
						if (TryPackAtlasRects(
							CandidatePackRects,
							AtlasResolution,
							CandidateInteriorMins))
						{
							OutAtlasResolution = AtlasResolution;
							OutPackRects = MoveTemp(CandidatePackRects);
							OutInteriorMins = MoveTemp(CandidateInteriorMins);
							return true;
						}
					}
				}
			}

			double LowScale = 0.0;
			double HighScale =
				static_cast<double>(MaximumAtlasResolution) / MaxPlaneDimension;
			bool bFoundPack = false;
			for (int32 Iteration = 0; Iteration < 28; ++Iteration)
			{
				const double MidScale = 0.5 * (LowScale + HighScale);
				BuildPackRects(MidScale, CandidatePackRects);
				if (TryPackAtlasRects(
					CandidatePackRects,
					MaximumAtlasResolution,
					CandidateInteriorMins))
				{
					LowScale = MidScale;
					OutPackRects = CandidatePackRects;
					OutInteriorMins = CandidateInteriorMins;
					bFoundPack = true;
				}
				else
				{
					HighScale = MidScale;
				}
			}

			if (!bFoundPack)
			{
				BuildPackRects(0.0, CandidatePackRects);
				if (!TryPackAtlasRects(
					CandidatePackRects,
					MaximumAtlasResolution,
					CandidateInteriorMins))
				{
					return false;
				}
				OutPackRects = MoveTemp(CandidatePackRects);
				OutInteriorMins = MoveTemp(CandidateInteriorMins);
			}

			OutAtlasResolution = MaximumAtlasResolution;
			return true;
		}

		template <typename PlaneType>
		void UpdateWorldTexelSizeStats(
			const TArray<PlaneType>& Planes,
			FPlaneProxyMeshStats& InOutStats)
		{
			double MinimumTexelSize = TNumericLimits<double>::Max();
			double MaximumTexelSize = 0.0;
			for (const PlaneType& Plane : Planes)
			{
				if (Plane.AtlasTileSize.X <= 0 || Plane.AtlasTileSize.Y <= 0)
				{
					continue;
				}

				const double TexelSizeU =
					FMath::Max(Plane.MaxU - Plane.MinU, UE_DOUBLE_SMALL_NUMBER)
					/ static_cast<double>(Plane.AtlasTileSize.X);
				const double TexelSizeV =
					FMath::Max(Plane.MaxV - Plane.MinV, UE_DOUBLE_SMALL_NUMBER)
					/ static_cast<double>(Plane.AtlasTileSize.Y);
				MinimumTexelSize = FMath::Min(
					MinimumTexelSize,
					FMath::Min(TexelSizeU, TexelSizeV));
				MaximumTexelSize = FMath::Max(
					MaximumTexelSize,
					FMath::Max(TexelSizeU, TexelSizeV));
			}

			InOutStats.MinimumWorldTexelSizeCm =
				MaximumTexelSize > 0.0 ? MinimumTexelSize : 0.0;
			InOutStats.MaximumWorldTexelSizeCm = MaximumTexelSize;
		}

		bool PackPreparedProxyPlanesIntoAtlas(
			TArray<FPreparedProxyPlane>& PreparedPlanes,
			const FPlaneProxySettings& Settings,
			const double SourceMaxDimension,
			int32& OutAtlasWidth,
			int32& OutAtlasHeight,
			int32& OutLargestInteriorDimension,
			int32& OutLargestPadding)
		{
			if (PreparedPlanes.IsEmpty())
			{
				return false;
			}

			(void)SourceMaxDimension;

			double MaxPlaneDimension = 1.0;
			for (const FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
			{
				const double AtlasResolutionScale = PreparedPlane.bIsTrunkCard
					? FMath::Clamp(Settings.TrunkCardAtlasScale, 0.5, 2.0)
					: 1.0;
				MaxPlaneDimension = FMath::Max(
					MaxPlaneDimension,
					FMath::Max(PreparedPlane.MaxU - PreparedPlane.MinU, PreparedPlane.MaxV - PreparedPlane.MinV) * AtlasResolutionScale);
			}

			auto BuildPackRects = [
				&PreparedPlanes,
				&Settings](const double PixelsPerUnit, TArray<FAtlasPackRect>& OutPackRects)
			{
				OutPackRects.Reset();
				for (int32 PlaneIndex = 0; PlaneIndex < PreparedPlanes.Num(); ++PlaneIndex)
				{
					FPreparedProxyPlane& PreparedPlane = PreparedPlanes[PlaneIndex];
					const double AtlasResolutionScale = PreparedPlane.bIsTrunkCard
						? FMath::Clamp(Settings.TrunkCardAtlasScale, 0.5, 2.0)
						: 1.0;
					const double PlaneWidth = FMath::Max(PreparedPlane.MaxU - PreparedPlane.MinU, 1.0) * AtlasResolutionScale;
					const double PlaneHeight = FMath::Max(PreparedPlane.MaxV - PreparedPlane.MinV, 1.0) * AtlasResolutionScale;
					const int32 InteriorWidth = FMath::Max(1, FMath::CeilToInt(PlaneWidth * PixelsPerUnit));
					const int32 InteriorHeight = FMath::Max(1, FMath::CeilToInt(PlaneHeight * PixelsPerUnit));
					FAtlasPackRect FrontRect;
					FrontRect.PlaneIndex = PlaneIndex;
					FrontRect.bBackFace = false;
					FrontRect.InteriorSize = FIntPoint(InteriorWidth, InteriorHeight);
					FrontRect.PitchSize = FrontRect.InteriorSize;
					OutPackRects.Add(FrontRect);

					if (ShouldBakeBackFaceAtlas(PreparedPlane, Settings))
					{
						FAtlasPackRect BackRect = FrontRect;
						BackRect.bBackFace = true;
						OutPackRects.Add(BackRect);
					}
				}
			};

			TArray<FAtlasPackRect> BestPackRects;
			TArray<FIntPoint> BestInteriorMins;
			int32 AtlasResolution = 0;
			if (!ResolveAtlasPacking(
				Settings,
				MaxPlaneDimension,
				BuildPackRects,
				AtlasResolution,
				BestPackRects,
				BestInteriorMins))
			{
				return false;
			}

			OutAtlasWidth = AtlasResolution;
			OutAtlasHeight = AtlasResolution;
			OutLargestInteriorDimension = 0;
			OutLargestPadding = 0;

			for (FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
			{
				PreparedPlane.AtlasPixelMin = FIntPoint::ZeroValue;
				PreparedPlane.BackAtlasPixelMin = FIntPoint::ZeroValue;
				PreparedPlane.AtlasTileSize = FIntPoint::ZeroValue;
				PreparedPlane.BackAtlasTileSize = FIntPoint::ZeroValue;
				PreparedPlane.AtlasTilePaddingPixels = 0;
				PreparedPlane.bHasBackFaceAtlas = false;
			}

			for (int32 RectIndex = 0; RectIndex < BestPackRects.Num(); ++RectIndex)
			{
				const FAtlasPackRect& PackRect = BestPackRects[RectIndex];
				if (!PreparedPlanes.IsValidIndex(PackRect.PlaneIndex))
				{
					continue;
				}

				FPreparedProxyPlane& PreparedPlane = PreparedPlanes[PackRect.PlaneIndex];
				if (PackRect.bBackFace)
				{
					PreparedPlane.bHasBackFaceAtlas = true;
					PreparedPlane.BackAtlasPixelMin = BestInteriorMins[RectIndex];
					PreparedPlane.BackAtlasTileSize = PackRect.InteriorSize;
				}
				else
				{
					PreparedPlane.AtlasPixelMin = BestInteriorMins[RectIndex];
					PreparedPlane.AtlasTileSize = PackRect.InteriorSize;
				}
			}

			for (FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
			{
				SetAtlasUVsFromTile(PreparedPlane.AtlasPixelMin, PreparedPlane.AtlasTileSize, OutAtlasWidth, OutAtlasHeight, Settings.AtlasVConvention, PreparedPlane.AtlasUVs);
				if (PreparedPlane.bHasBackFaceAtlas)
				{
					SetAtlasUVsFromTile(PreparedPlane.BackAtlasPixelMin, PreparedPlane.BackAtlasTileSize, OutAtlasWidth, OutAtlasHeight, Settings.AtlasVConvention, PreparedPlane.BackAtlasUVs);
				}
				else
				{
					for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
					{
						PreparedPlane.BackAtlasUVs[CornerIndex] = PreparedPlane.AtlasUVs[CornerIndex];
					}
				}

				OutLargestInteriorDimension = FMath::Max(OutLargestInteriorDimension, FMath::Max(PreparedPlane.AtlasTileSize.X, PreparedPlane.AtlasTileSize.Y));
				OutLargestPadding = FMath::Max(OutLargestPadding, PreparedPlane.AtlasTilePaddingPixels);
			}

			return true;
		}

		void UpdatePlaneInfoCornersFromBounds(FPlaneProxyPlaneInfo& PlaneInfo)
		{
			const FVector PlaneOrigin = PlaneInfo.Normal * PlaneInfo.Rho;
			PlaneInfo.Corners[0] = PlaneOrigin + PlaneInfo.AxisU * PlaneInfo.MinU + PlaneInfo.AxisV * PlaneInfo.MinV;
			PlaneInfo.Corners[1] = PlaneOrigin + PlaneInfo.AxisU * PlaneInfo.MaxU + PlaneInfo.AxisV * PlaneInfo.MinV;
			PlaneInfo.Corners[2] = PlaneOrigin + PlaneInfo.AxisU * PlaneInfo.MaxU + PlaneInfo.AxisV * PlaneInfo.MaxV;
			PlaneInfo.Corners[3] = PlaneOrigin + PlaneInfo.AxisU * PlaneInfo.MinU + PlaneInfo.AxisV * PlaneInfo.MaxV;
		}

		bool PackPlaneInfosIntoAtlas(
			TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
			const FPlaneProxySettings& Settings,
			int32& OutAtlasWidth,
			int32& OutAtlasHeight,
			int32& OutLargestInteriorDimension,
			int32& OutLargestPadding)
		{
			if (PlaneInfos.IsEmpty())
			{
				return false;
			}

			double MaxPlaneDimension = 1.0;
			for (const FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
			{
				const double AtlasResolutionScale = PlaneInfo.bIsTrunkCard
					? FMath::Clamp(Settings.TrunkCardAtlasScale, 0.5, 2.0)
					: 1.0;
				MaxPlaneDimension = FMath::Max(
					MaxPlaneDimension,
					FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, PlaneInfo.MaxV - PlaneInfo.MinV) * AtlasResolutionScale);
			}

			auto BuildPackRects = [
				&PlaneInfos,
				&Settings](const double PixelsPerUnit, TArray<FAtlasPackRect>& OutPackRects)
			{
				OutPackRects.Reset();
				for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
				{
					FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
					const double AtlasResolutionScale = PlaneInfo.bIsTrunkCard
						? FMath::Clamp(Settings.TrunkCardAtlasScale, 0.5, 2.0)
						: 1.0;
					const double PlaneWidth = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, 1.0) * AtlasResolutionScale;
					const double PlaneHeight = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, 1.0) * AtlasResolutionScale;
					const int32 InteriorWidth = FMath::Max(1, FMath::CeilToInt(PlaneWidth * PixelsPerUnit));
					const int32 InteriorHeight = FMath::Max(1, FMath::CeilToInt(PlaneHeight * PixelsPerUnit));
					FAtlasPackRect FrontRect;
					FrontRect.PlaneIndex = PlaneIndex;
					FrontRect.bBackFace = false;
					FrontRect.InteriorSize = FIntPoint(InteriorWidth, InteriorHeight);
					FrontRect.PitchSize = FrontRect.InteriorSize;
					OutPackRects.Add(FrontRect);

					if (PlaneInfo.bHasBackFaceAtlas)
					{
						FAtlasPackRect BackRect = FrontRect;
						BackRect.bBackFace = true;
						OutPackRects.Add(BackRect);
					}
				}
			};

			TArray<FAtlasPackRect> BestPackRects;
			TArray<FIntPoint> BestInteriorMins;
			int32 AtlasResolution = 0;
			if (!ResolveAtlasPacking(
				Settings,
				MaxPlaneDimension,
				BuildPackRects,
				AtlasResolution,
				BestPackRects,
				BestInteriorMins))
			{
				return false;
			}

			OutAtlasWidth = AtlasResolution;
			OutAtlasHeight = AtlasResolution;
			OutLargestInteriorDimension = 0;
			OutLargestPadding = 0;

			for (FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
			{
				PlaneInfo.AtlasPixelMin = FIntPoint::ZeroValue;
				PlaneInfo.BackAtlasPixelMin = FIntPoint::ZeroValue;
				PlaneInfo.AtlasTileSize = FIntPoint::ZeroValue;
				PlaneInfo.BackAtlasTileSize = FIntPoint::ZeroValue;
				PlaneInfo.AtlasTileResolution = 0;
				PlaneInfo.AtlasTilePaddingPixels = 0;
			}

			for (int32 RectIndex = 0; RectIndex < BestPackRects.Num(); ++RectIndex)
			{
				const FAtlasPackRect& PackRect = BestPackRects[RectIndex];
				if (!PlaneInfos.IsValidIndex(PackRect.PlaneIndex))
				{
					continue;
				}

				FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PackRect.PlaneIndex];
				if (PackRect.bBackFace)
				{
					PlaneInfo.BackAtlasPixelMin = BestInteriorMins[RectIndex];
					PlaneInfo.BackAtlasTileSize = PackRect.InteriorSize;
				}
				else
				{
					PlaneInfo.AtlasPixelMin = BestInteriorMins[RectIndex];
					PlaneInfo.AtlasTileSize = PackRect.InteriorSize;
				}
			}

			for (FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
			{
				SetAtlasUVsFromTile(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, OutAtlasWidth, OutAtlasHeight, Settings.AtlasVConvention, PlaneInfo.AtlasUVs);
				if (PlaneInfo.bHasBackFaceAtlas)
				{
					SetAtlasUVsFromTile(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, OutAtlasWidth, OutAtlasHeight, Settings.AtlasVConvention, PlaneInfo.BackAtlasUVs);
				}
				else
				{
					for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
					{
						PlaneInfo.BackAtlasUVs[CornerIndex] = PlaneInfo.AtlasUVs[CornerIndex];
					}
				}

				PlaneInfo.AtlasTileResolution = FMath::Min(PlaneInfo.AtlasTileSize.X, PlaneInfo.AtlasTileSize.Y);
				OutLargestInteriorDimension = FMath::Max(OutLargestInteriorDimension, FMath::Max(PlaneInfo.AtlasTileSize.X, PlaneInfo.AtlasTileSize.Y));
				OutLargestPadding = FMath::Max(OutLargestPadding, PlaneInfo.AtlasTilePaddingPixels);
			}

			return true;
		}

		bool PackSharedTwoPlaneInfosIntoAtlas(
			TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
			const FPlaneProxySettings& Settings,
			int32& OutAtlasWidth,
			int32& OutAtlasHeight,
			int32& OutLargestInteriorDimension,
			int32& OutLargestPadding)
		{
			if (PlaneInfos.Num() != 2
				|| PlaneInfos[0].bHasBackFaceAtlas
				|| PlaneInfos[1].bHasBackFaceAtlas)
			{
				return false;
			}

			const double PlaneWidth = FMath::Max(PlaneInfos[0].MaxU - PlaneInfos[0].MinU, 1.0);
			const double PlaneHeight = FMath::Max(PlaneInfos[0].MaxV - PlaneInfos[0].MinV, 1.0);
			const bool bHorizontalLayout = PlaneHeight > PlaneWidth;
			const bool bUseWorldTexelSize =
				Settings.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize;
			const int32 MaximumAtlasResolution = bUseWorldTexelSize
				? TextureResolution::FloorToSupportedPowerOfTwo(
					Settings.TextureAtlasResolution)
				: FMath::Clamp(
					Settings.TextureAtlasResolution,
					TextureResolution::MinimumSupportedAtlasResolution,
					TextureResolution::MaximumSupportedAtlasResolution);

			int32 AtlasResolution = MaximumAtlasResolution;
			int32 TileWidth = 0;
			int32 TileHeight = 0;
			if (bUseWorldTexelSize)
			{
				const double TargetPixelsPerCentimeter =
					FMath::Max(Settings.TargetTexelsPerMeter, 0.01)
					/ TextureResolution::CentimetersPerMeter;
				const double TargetTileWidth =
					PlaneWidth * TargetPixelsPerCentimeter;
				const double TargetTileHeight =
					PlaneHeight * TargetPixelsPerCentimeter;
				const bool bTargetFits = bHorizontalLayout
					? (TargetTileWidth <= MaximumAtlasResolution / 2.0
						&& TargetTileHeight <= MaximumAtlasResolution)
					: (TargetTileWidth <= MaximumAtlasResolution
						&& TargetTileHeight <= MaximumAtlasResolution / 2.0);
				if (bTargetFits)
				{
					TileWidth =
						FMath::Max(1, FMath::CeilToInt(TargetTileWidth));
					TileHeight =
						FMath::Max(1, FMath::CeilToInt(TargetTileHeight));
					const int32 RequiredAtlasResolution = bHorizontalLayout
						? FMath::Max(TileWidth * 2, TileHeight)
						: FMath::Max(TileWidth, TileHeight * 2);
					AtlasResolution = FMath::Max(
						TextureResolution::ResolveMinimumAtlasResolution(
							Settings.MinimumTextureAtlasResolution,
							MaximumAtlasResolution),
						TextureResolution::CeilToSupportedPowerOfTwo(
							RequiredAtlasResolution));
				}
			}

			if (TileWidth == 0 || TileHeight == 0)
			{
				const double PixelsPerUnit = bHorizontalLayout
					? FMath::Min(
						static_cast<double>(AtlasResolution) / (2.0 * PlaneWidth),
						static_cast<double>(AtlasResolution) / PlaneHeight)
					: FMath::Min(
						static_cast<double>(AtlasResolution) / PlaneWidth,
						static_cast<double>(AtlasResolution) / (2.0 * PlaneHeight));
				TileWidth = FMath::Clamp(
					FMath::FloorToInt(PlaneWidth * PixelsPerUnit),
					1,
					bHorizontalLayout ? AtlasResolution / 2 : AtlasResolution);
				TileHeight = FMath::Clamp(
					FMath::FloorToInt(PlaneHeight * PixelsPerUnit),
					1,
					bHorizontalLayout ? AtlasResolution : AtlasResolution / 2);
			}

			OutAtlasWidth = AtlasResolution;
			OutAtlasHeight = AtlasResolution;
			OutLargestInteriorDimension = FMath::Max(TileWidth, TileHeight);
			OutLargestPadding = 0;
			for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
			{
				FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
				PlaneInfo.AtlasPixelMin = PlaneIndex == 0
					? FIntPoint::ZeroValue
					: bHorizontalLayout
						? FIntPoint(TileWidth, 0)
						: FIntPoint(0, TileHeight);
				PlaneInfo.AtlasTileSize = FIntPoint(TileWidth, TileHeight);
				PlaneInfo.BackAtlasPixelMin = FIntPoint::ZeroValue;
				PlaneInfo.BackAtlasTileSize = FIntPoint::ZeroValue;
				PlaneInfo.AtlasTileResolution = FMath::Min(TileWidth, TileHeight);
				PlaneInfo.AtlasTilePaddingPixels = 0;
				SetAtlasUVsFromTile(
					PlaneInfo.AtlasPixelMin,
					PlaneInfo.AtlasTileSize,
					OutAtlasWidth,
					OutAtlasHeight,
					Settings.AtlasVConvention,
					PlaneInfo.AtlasUVs);
				for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
				{
					PlaneInfo.BackAtlasUVs[CornerIndex] = PlaneInfo.AtlasUVs[CornerIndex];
				}
			}

			return true;
		}

		void ComputeSignedDistanceRangeForTriangles(
			const TArray<FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const FVector& PlaneNormal,
			const double PlaneRho,
			double& OutMinSignedDistance,
			double& OutMaxSignedDistance)
		{
			OutMinSignedDistance = TNumericLimits<double>::Max();
			OutMaxSignedDistance = -TNumericLimits<double>::Max();

			for (const int32 TriangleIndex : TriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				for (const FVector& Vertex : Triangle.Vertices)
				{
					const double SignedDistance = FVector::DotProduct(PlaneNormal, Vertex) - PlaneRho;
					OutMinSignedDistance = FMath::Min(OutMinSignedDistance, SignedDistance);
					OutMaxSignedDistance = FMath::Max(OutMaxSignedDistance, SignedDistance);
				}
			}

			if (!FMath::IsFinite(OutMinSignedDistance) || !FMath::IsFinite(OutMaxSignedDistance))
			{
				OutMinSignedDistance = 0.0;
				OutMaxSignedDistance = 0.0;
			}
		}

		double GetPreparedPlaneEnvelopeCoordinate(const FPreparedProxyPlane& Plane, const FVector& Point, const int32 AxisIndex)
		{
			if (AxisIndex == 0)
			{
				return FVector::DotProduct(Point, Plane.AxisU);
			}
			if (AxisIndex == 1)
			{
				return FVector::DotProduct(Point, Plane.AxisV);
			}
			return FVector::DotProduct(Point, Plane.OrientedNormal) - Plane.OrientedRho;
		}

		void ClipPolygonAgainstPreparedPlaneEnvelopeBound(
			TArray<FVector>& Polygon,
			const FPreparedProxyPlane& Plane,
			const int32 AxisIndex,
			const double Bound,
			const bool bKeepGreater,
			const double Tolerance)
		{
			if (Polygon.IsEmpty())
			{
				return;
			}

			TArray<FVector> ClippedPolygon;
			ClippedPolygon.Reserve(Polygon.Num() + 1);

			FVector PreviousPoint = Polygon.Last();
			double PreviousValue = GetPreparedPlaneEnvelopeCoordinate(Plane, PreviousPoint, AxisIndex);
			bool bPreviousInside = bKeepGreater
				? PreviousValue >= Bound - Tolerance
				: PreviousValue <= Bound + Tolerance;

			for (const FVector& CurrentPoint : Polygon)
			{
				const double CurrentValue = GetPreparedPlaneEnvelopeCoordinate(Plane, CurrentPoint, AxisIndex);
				const bool bCurrentInside = bKeepGreater
					? CurrentValue >= Bound - Tolerance
					: CurrentValue <= Bound + Tolerance;

				if (bPreviousInside != bCurrentInside)
				{
					const double Denominator = CurrentValue - PreviousValue;
					if (FMath::Abs(Denominator) > UE_SMALL_NUMBER)
					{
						const double T = FMath::Clamp((Bound - PreviousValue) / Denominator, 0.0, 1.0);
						ClippedPolygon.Add(PreviousPoint + (CurrentPoint - PreviousPoint) * T);
					}
				}

				if (bCurrentInside)
				{
					ClippedPolygon.Add(CurrentPoint);
				}

				PreviousPoint = CurrentPoint;
				PreviousValue = CurrentValue;
				bPreviousInside = bCurrentInside;
			}

			Polygon = MoveTemp(ClippedPolygon);
		}

		double ComputePolygonArea3D(const TArray<FVector>& Polygon)
		{
			if (Polygon.Num() < 3)
			{
				return 0.0;
			}

			const FVector& Origin = Polygon[0];
			double Area = 0.0;
			for (int32 PointIndex = 1; PointIndex + 1 < Polygon.Num(); ++PointIndex)
			{
				Area += 0.5 * FVector::CrossProduct(Polygon[PointIndex] - Origin, Polygon[PointIndex + 1] - Origin).Size();
			}
			return Area;
		}

		void GetScaledPreparedPlaneEnvelopeBounds(
			const FPreparedProxyPlane& Plane,
			const double EnvelopeScale,
			double& OutMinU,
			double& OutMaxU,
			double& OutMinV,
			double& OutMaxV,
			double& OutMinSignedDistance,
			double& OutMaxSignedDistance)
		{
			const double Scale = FMath::Clamp(EnvelopeScale, 0.0, 1.0);
			auto ScaleRange = [Scale](const double MinValue, const double MaxValue, double& OutMinValue, double& OutMaxValue)
			{
				const double Center = (MinValue + MaxValue) * 0.5;
				const double HalfExtent = FMath::Max(0.0, (MaxValue - MinValue) * 0.5 * Scale);
				OutMinValue = Center - HalfExtent;
				OutMaxValue = Center + HalfExtent;
			};

			ScaleRange(Plane.EnvelopeMinU, Plane.EnvelopeMaxU, OutMinU, OutMaxU);
			ScaleRange(Plane.EnvelopeMinV, Plane.EnvelopeMaxV, OutMinV, OutMaxV);
			ScaleRange(Plane.EnvelopeMinSignedDistance, Plane.EnvelopeMaxSignedDistance, OutMinSignedDistance, OutMaxSignedDistance);
		}

		struct FClippedCrackReductionTriangle
		{
			int32 TriangleIndex = INDEX_NONE;
			TArray<FVector> ClippedPolygon;
		};

		bool ClipTriangleToPreparedPlaneEnvelope(
			const TArray<FSourceTriangle>& Triangles,
			const int32 TriangleIndex,
			const FPreparedProxyPlane& Plane,
			const double Tolerance,
			const double EnvelopeScale,
			TArray<FVector>& OutClippedPolygon)
		{
			OutClippedPolygon.Reset();
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				return false;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			OutClippedPolygon.Reserve(8);
			OutClippedPolygon.Add(Triangle.Vertices[0]);
			OutClippedPolygon.Add(Triangle.Vertices[1]);
			OutClippedPolygon.Add(Triangle.Vertices[2]);

			double MinU = 0.0;
			double MaxU = 0.0;
			double MinV = 0.0;
			double MaxV = 0.0;
			double MinSignedDistance = 0.0;
			double MaxSignedDistance = 0.0;
			GetScaledPreparedPlaneEnvelopeBounds(Plane, EnvelopeScale, MinU, MaxU, MinV, MaxV, MinSignedDistance, MaxSignedDistance);

			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 0, MinU, true, Tolerance);
			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 0, MaxU, false, Tolerance);
			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 1, MinV, true, Tolerance);
			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 1, MaxV, false, Tolerance);
			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 2, MinSignedDistance, true, Tolerance);
			ClipPolygonAgainstPreparedPlaneEnvelopeBound(OutClippedPolygon, Plane, 2, MaxSignedDistance, false, Tolerance);

			if (ComputePolygonArea3D(OutClippedPolygon) <= UE_SMALL_NUMBER)
			{
				OutClippedPolygon.Reset();
				return false;
			}

			return true;
		}

		void CollectTrianglesIntersectingPreparedPlaneEnvelope(
			const TArray<FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const FPreparedProxyPlane& Plane,
			const double Tolerance,
			const double EnvelopeScale,
			TArray<FClippedCrackReductionTriangle>& OutTriangles)
		{
			OutTriangles.Reset();
			for (const int32 TriangleIndex : TriangleIndices)
			{
				FClippedCrackReductionTriangle Candidate;
				if (ClipTriangleToPreparedPlaneEnvelope(Triangles, TriangleIndex, Plane, Tolerance, EnvelopeScale, Candidate.ClippedPolygon))
				{
					Candidate.TriangleIndex = TriangleIndex;
					OutTriangles.Add(MoveTemp(Candidate));
				}
			}
		}

		uint64 MakeCrackReductionProjectionKey(const int32 SourcePlaneInfoIndex, const int32 TriangleIndex)
		{
			return (static_cast<uint64>(static_cast<uint32>(SourcePlaneInfoIndex)) << 32)
				| static_cast<uint64>(static_cast<uint32>(TriangleIndex));
		}

		void AddCrackReductionProjection(
			TSet<uint64>& ProjectionKeySet,
			TArray<FCrackReductionProjection>& CrackProjections,
			const TArray<int32>& PrimaryTriangleIndices,
			const int32 SourcePlaneInfoIndex,
			const int32 TriangleIndex,
			const TArray<FVector>& ClippedPolygon)
		{
			if (PrimaryTriangleIndices.Contains(TriangleIndex))
			{
				return;
			}
			if (ClippedPolygon.Num() < 3)
			{
				return;
			}

			const uint64 ProjectionKey = MakeCrackReductionProjectionKey(SourcePlaneInfoIndex, TriangleIndex);
			if (ProjectionKeySet.Contains(ProjectionKey))
			{
				return;
			}

			ProjectionKeySet.Add(ProjectionKey);
			FCrackReductionProjection& Projection = CrackProjections.AddDefaulted_GetRef();
			Projection.TriangleIndex = TriangleIndex;
			Projection.SourcePlaneInfoIndex = SourcePlaneInfoIndex;
			Projection.ClippedPolygon = ClippedPolygon;
		}

		void ApplyPlaneProxyEnvelopeCrackReduction(
			const TArray<FSourceTriangle>& Triangles,
			const FPlaneProxySettings& Settings,
			TArray<FPreparedProxyPlane>& PreparedPlanes)
		{
			if (Settings.CrackReductionMode == EPlaneProxyCrackReductionMode::Off
				|| PreparedPlanes.Num() <= 1)
			{
				return;
			}

			const double ProjectionScale = FMath::Clamp(Settings.CrackReductionProjectionScale, 0.0, 1.0);
			if (ProjectionScale <= UE_DOUBLE_SMALL_NUMBER)
			{
				return;
			}
			constexpr double Tolerance = 1.0e-4;
			TArray<TSet<uint64>> CrackProjectionKeySets;
			CrackProjectionKeySets.SetNum(PreparedPlanes.Num());
			TArray<TArray<FCrackReductionProjection>> CrackProjectionLists;
			CrackProjectionLists.SetNum(PreparedPlanes.Num());

			TArray<FClippedCrackReductionTriangle> TrianglesFromAIntersectingB;
			TArray<FClippedCrackReductionTriangle> TrianglesFromBIntersectingA;
			for (int32 PlaneAIndex = 0; PlaneAIndex < PreparedPlanes.Num(); ++PlaneAIndex)
			{
				for (int32 PlaneBIndex = PlaneAIndex + 1; PlaneBIndex < PreparedPlanes.Num(); ++PlaneBIndex)
				{
					const FPreparedProxyPlane& PlaneA = PreparedPlanes[PlaneAIndex];
					const FPreparedProxyPlane& PlaneB = PreparedPlanes[PlaneBIndex];
					if (PlaneA.bIsTrunkCard || PlaneB.bIsTrunkCard)
					{
						continue;
					}

					CollectTrianglesIntersectingPreparedPlaneEnvelope(Triangles, PlaneA.TriangleIndices, PlaneB, Tolerance, ProjectionScale, TrianglesFromAIntersectingB);
					CollectTrianglesIntersectingPreparedPlaneEnvelope(Triangles, PlaneB.TriangleIndices, PlaneA, Tolerance, ProjectionScale, TrianglesFromBIntersectingA);
					if (TrianglesFromAIntersectingB.IsEmpty() || TrianglesFromBIntersectingA.IsEmpty())
					{
						continue;
					}

					for (const FClippedCrackReductionTriangle& Triangle : TrianglesFromAIntersectingB)
					{
						AddCrackReductionProjection(CrackProjectionKeySets[PlaneBIndex], CrackProjectionLists[PlaneBIndex], PlaneB.TriangleIndices, PlaneAIndex, Triangle.TriangleIndex, Triangle.ClippedPolygon);
					}
					for (const FClippedCrackReductionTriangle& Triangle : TrianglesFromBIntersectingA)
					{
						AddCrackReductionProjection(CrackProjectionKeySets[PlaneAIndex], CrackProjectionLists[PlaneAIndex], PlaneA.TriangleIndices, PlaneBIndex, Triangle.TriangleIndex, Triangle.ClippedPolygon);
					}
				}
			}

			for (int32 PlaneIndex = 0; PlaneIndex < PreparedPlanes.Num(); ++PlaneIndex)
			{
				PreparedPlanes[PlaneIndex].CrackReductionProjections = MoveTemp(CrackProjectionLists[PlaneIndex]);
				PreparedPlanes[PlaneIndex].CrackReductionProjections.Sort([](const FCrackReductionProjection& A, const FCrackReductionProjection& B)
				{
					if (A.SourcePlaneInfoIndex != B.SourcePlaneInfoIndex)
					{
						return A.SourcePlaneInfoIndex < B.SourcePlaneInfoIndex;
					}
					return A.TriangleIndex < B.TriangleIndex;
				});
			}
		}

		bool AddQuadPolygon(
			FMeshDescription& MeshDescription,
			const FPolygonGroupID PolygonGroupID,
			TVertexAttributesRef<FVector3f>& VertexPositions,
			TVertexInstanceAttributesRef<FVector2f>& VertexInstanceUVs,
			TVertexInstanceAttributesRef<FVector3f>& VertexInstanceNormals,
			TVertexInstanceAttributesRef<FVector3f>& VertexInstanceTangents,
			TVertexInstanceAttributesRef<float>& VertexInstanceBinormalSigns,
			TTriangleAttributesRef<FVector3f>& TriangleNormals,
			TTriangleAttributesRef<FVector3f>& TriangleTangents,
			TTriangleAttributesRef<FVector3f>& TriangleBinormals,
			TEdgeAttributesRef<bool>& EdgeHardnesses,
			const FVector (&Corners)[4],
			const FVector2f (&CornerUVs)[4],
			const FVector2f (&BackCornerUVs)[4],
			const FVector2f MaskUV,
			const FVector& InShadingNormal,
			const bool bReverseFacing)
		{
			static constexpr int32 UnrealFrontFaceOrder[4] = { 0, 3, 2, 1 };
			static constexpr int32 UnrealBackFaceOrder[4] = { 0, 1, 2, 3 };
			const int32 (&VertexOrder)[4] =
				bReverseFacing ? UnrealBackFaceOrder : UnrealFrontFaceOrder;

			const FVector TangentU = Corners[1] - Corners[0];
			const FVector TangentV = Corners[3] - Corners[0];
			const FVector GeometryTangent = TangentU.GetSafeNormal();
			const FVector BaseFaceNormal = FVector::CrossProduct(TangentU, Corners[2] - Corners[0]).GetSafeNormal();
			const FVector IntendedFaceNormal = bReverseFacing ? -BaseFaceNormal : BaseFaceNormal;
			const FVector DesiredBinormal = (bReverseFacing ? -TangentV : TangentV).GetSafeNormal();
			if (GeometryTangent.IsNearlyZero() || IntendedFaceNormal.IsNearlyZero() || DesiredBinormal.IsNearlyZero())
			{
				return false;
			}

			FVector ShadingNormal = InShadingNormal.GetSafeNormal();
			if (ShadingNormal.IsNearlyZero())
			{
				ShadingNormal = IntendedFaceNormal;
			}
			if (FVector::DotProduct(ShadingNormal, IntendedFaceNormal) < 0.0)
			{
				ShadingNormal *= -1.0;
			}

			FVector Tangent = GeometryTangent - ShadingNormal * FVector::DotProduct(GeometryTangent, ShadingNormal);
			Tangent.Normalize();
			if (Tangent.IsNearlyZero())
			{
				FVector UnusedAxisV = FVector::UpVector;
				BuildPlaneFrame(ShadingNormal, Tangent, UnusedAxisV);
			}

			const FVector DerivedBinormal = FVector::CrossProduct(ShadingNormal, Tangent).GetSafeNormal();
			const float BinormalSign = FVector::DotProduct(DerivedBinormal, DesiredBinormal) >= 0.0 ? 1.0f : -1.0f;
			const FVector SignedBinormal = DerivedBinormal * BinormalSign;

			TArray<FVertexInstanceID> VertexInstanceIDs;
			VertexInstanceIDs.SetNum(4);
			for (int32 VertexOrderIndex = 0; VertexOrderIndex < 4; ++VertexOrderIndex)
			{
				const int32 CornerIndex = VertexOrder[VertexOrderIndex];
				const FVertexID VertexID = MeshDescription.CreateVertex();
				VertexPositions[VertexID] = FVector3f(Corners[CornerIndex]);

				const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
				VertexInstanceIDs[VertexOrderIndex] = VertexInstanceID;
				VertexInstanceUVs.Set(VertexInstanceID, 0, CornerUVs[CornerIndex]);
				VertexInstanceUVs.Set(VertexInstanceID, 1, BackCornerUVs[CornerIndex]);
				VertexInstanceUVs.Set(VertexInstanceID, 2, MaskUV);
				VertexInstanceNormals[VertexInstanceID] = FVector3f(ShadingNormal);
				VertexInstanceTangents[VertexInstanceID] = FVector3f(Tangent);
				VertexInstanceBinormalSigns[VertexInstanceID] = BinormalSign;
			}

			TArray<FEdgeID> NewEdgeIDs;
			const FPolygonID PolygonID = MeshDescription.CreatePolygon(PolygonGroupID, VertexInstanceIDs, &NewEdgeIDs);
			for (const FEdgeID EdgeID : NewEdgeIDs)
			{
				EdgeHardnesses[EdgeID] = true;
			}

			for (const FTriangleID TriangleID : MeshDescription.GetPolygonTriangles(PolygonID))
			{
				TriangleNormals[TriangleID] = FVector3f(IntendedFaceNormal);
				TriangleTangents[TriangleID] = FVector3f(Tangent);
				TriangleBinormals[TriangleID] = FVector3f(SignedBinormal);
			}

			return true;
		}

		bool ExtractTrianglesFromRenderData(
			const UStaticMesh& StaticMesh,
			const int32 LODIndex,
			TArray<FSourceTriangle>& OutTriangles)
		{
			OutTriangles.Reset();
			if (!StaticMesh.GetRenderData()
				|| !StaticMesh.GetRenderData()->LODResources.IsValidIndex(LODIndex))
			{
				return false;
			}

			const FStaticMeshLODResources& LODResources =
				StaticMesh.GetRenderData()->LODResources[LODIndex];
			const FPositionVertexBuffer& PositionBuffer = LODResources.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& StaticMeshVertexBuffer = LODResources.VertexBuffers.StaticMeshVertexBuffer;
			const FColorVertexBuffer& ColorVertexBuffer = LODResources.VertexBuffers.ColorVertexBuffer;
			const FIndexArrayView Indices = LODResources.IndexBuffer.GetArrayView();
			if (PositionBuffer.GetNumVertices() == 0 || StaticMeshVertexBuffer.GetNumVertices() == 0 || Indices.Num() < 3)
			{
				return false;
			}

			const int32 SourceUVChannelCount = StaticMeshVertexBuffer.GetNumTexCoords();
			const bool bHasUVs = SourceUVChannelCount > 0;
			const bool bHasVertexColors = ColorVertexBuffer.GetNumVertices() == PositionBuffer.GetNumVertices();
			const int32 StoredUVChannelCount = bHasUVs
				? FMath::Min(SourceUVChannelCount, UE::FoliageBaker::PlaneCover::MaxSourceMeshUVChannels)
				: 0;
			OutTriangles.Reserve(Indices.Num() / 3);

			for (const FStaticMeshSection& Section : LODResources.Sections)
			{
				const int32 FirstIndex = static_cast<int32>(Section.FirstIndex);
				const int32 LastIndexExclusive = FirstIndex + static_cast<int32>(Section.NumTriangles) * 3;
				if (FirstIndex < 0 || LastIndexExclusive > Indices.Num())
				{
					continue;
				}

				for (int32 IndexOffset = FirstIndex; IndexOffset < LastIndexExclusive; IndexOffset += 3)
				{
					const uint32 VertexIndices[3] =
					{
						Indices[IndexOffset + 0],
						Indices[IndexOffset + 1],
						Indices[IndexOffset + 2]
					};

					if (VertexIndices[0] >= PositionBuffer.GetNumVertices()
						|| VertexIndices[1] >= PositionBuffer.GetNumVertices()
						|| VertexIndices[2] >= PositionBuffer.GetNumVertices()
						|| VertexIndices[0] >= StaticMeshVertexBuffer.GetNumVertices()
						|| VertexIndices[1] >= StaticMeshVertexBuffer.GetNumVertices()
						|| VertexIndices[2] >= StaticMeshVertexBuffer.GetNumVertices())
					{
						continue;
					}

					FSourceTriangle Triangle;
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						const uint32 RenderVertexIndex = VertexIndices[VertexIndex];
						Triangle.Vertices[VertexIndex] = FVector(PositionBuffer.VertexPosition(RenderVertexIndex));
						if (bHasUVs)
						{
							for (int32 UVChannel = 0; UVChannel < StoredUVChannelCount; ++UVChannel)
							{
								Triangle.UVChannels[UVChannel][VertexIndex] = StaticMeshVertexBuffer.GetVertexUV(RenderVertexIndex, UVChannel);
							}
							Triangle.UVs[VertexIndex] = Triangle.UVChannels[0][VertexIndex];
						}

						const FVector4f RenderTangentZ4 = StaticMeshVertexBuffer.VertexTangentZ(RenderVertexIndex);
						FVector SourceNormal(RenderTangentZ4.X, RenderTangentZ4.Y, RenderTangentZ4.Z);
						SourceNormal = SourceNormal.GetSafeNormal();
						if (SourceNormal.IsNearlyZero())
						{
							SourceNormal = FVector::UpVector;
						}

						Triangle.VertexNormals[VertexIndex] = SourceNormal;

						const FVector4f RenderTangentX4 = StaticMeshVertexBuffer.VertexTangentX(RenderVertexIndex);
						FVector SourceTangent(RenderTangentX4.X, RenderTangentX4.Y, RenderTangentX4.Z);
						SourceTangent = SourceTangent.GetSafeNormal();
						if (SourceTangent.IsNearlyZero())
						{

							const FVector ReferenceAxis = FMath::Abs(SourceNormal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
							SourceTangent = FVector::CrossProduct(ReferenceAxis, SourceNormal).GetSafeNormal();
							if (SourceTangent.IsNearlyZero())
							{
								SourceTangent = FVector::ForwardVector;
							}
						}
						Triangle.VertexTangents[VertexIndex] = SourceTangent;

						if (bHasVertexColors)
						{
							const FLinearColor SourceColor = ColorVertexBuffer.VertexColor(RenderVertexIndex).ReinterpretAsLinear();
							Triangle.VertexColors[VertexIndex] = FVector4f(SourceColor.R, SourceColor.G, SourceColor.B, SourceColor.A);
						}


						const FVector4f RenderTangentY4 = StaticMeshVertexBuffer.VertexTangentY(RenderVertexIndex);
						const FVector RenderTangentY(RenderTangentY4.X, RenderTangentY4.Y, RenderTangentY4.Z);
						const FVector DerivedBinormal = FVector::CrossProduct(SourceNormal, SourceTangent);
						Triangle.BinormalSigns[VertexIndex] = FVector::DotProduct(DerivedBinormal, RenderTangentY) >= 0.0 ? 1.0f : -1.0f;
					}

					const FVector Edge01 = Triangle.Vertices[1] - Triangle.Vertices[0];
					const FVector Edge02 = Triangle.Vertices[2] - Triangle.Vertices[0];
					const FVector AreaVector = FVector::CrossProduct(Edge01, Edge02);
					const double DoubleArea = AreaVector.Size();
					if (DoubleArea <= DegenerateTriangleTolerance)
					{
						continue;
					}

					Triangle.Normal = AreaVector / DoubleArea;
					Triangle.ShadingNormal = (Triangle.VertexNormals[0] + Triangle.VertexNormals[1] + Triangle.VertexNormals[2]).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Triangle.Normal);
					Triangle.Area = 0.5 * DoubleArea;
					Triangle.MaterialIndex = Section.MaterialIndex;
					Triangle.NumUVChannels = StoredUVChannelCount;
					Triangle.bHasUVs = bHasUVs;
					Triangle.bHasSourceShadingNormal = true;
					Triangle.bHasTangents = true;
					Triangle.bHasVertexColors = bHasVertexColors;
					OutTriangles.Add(Triangle);
				}
			}

			return !OutTriangles.IsEmpty();
		}
	}

	bool ExtractTrianglesFromStaticMesh(
		const UStaticMesh& StaticMesh,
		int32 LODIndex,
		TArray<FSourceTriangle>& OutTriangles,
		FString& OutError)
	{
		OutTriangles.Reset();
		OutError.Reset();

		if (LODIndex < 0)
		{
			OutError = FString::Printf(TEXT("Source LOD index %d is invalid."), LODIndex);
			return false;
		}

		const FStaticMeshRenderData* RenderData = StaticMesh.GetRenderData();
		const int32 RenderLODCount = RenderData ? RenderData->LODResources.Num() : 0;
		const bool bHasRenderLOD = RenderData && RenderData->LODResources.IsValidIndex(LODIndex);
		if (bHasRenderLOD && ExtractTrianglesFromRenderData(StaticMesh, LODIndex, OutTriangles))
		{
			return true;
		}

		if (!StaticMesh.IsMeshDescriptionValid(LODIndex))
		{
			OutError = FString::Printf(
				TEXT("Static Mesh '%s' does not contain usable source LOD %d (render LOD count: %d, source model count: %d)."),
				*StaticMesh.GetName(),
				LODIndex,
				RenderLODCount,
				StaticMesh.GetNumSourceModels());
			return false;
		}

		const FMeshDescription& MeshDescription =
			*StaticMesh.GetMeshDescription(LODIndex);
		const TVertexAttributesConstRef<FVector3f> VertexPositions =
			MeshDescription.GetVertexPositions();
		const FStaticMeshConstAttributes MeshAttributes(MeshDescription);
		const bool bHasVertexInstanceNormals =
			MeshDescription.VertexInstanceAttributes().HasAttribute(
				MeshAttribute::VertexInstance::Normal);
		const bool bHasVertexInstanceTangents =
			MeshDescription.VertexInstanceAttributes().HasAttribute(
				MeshAttribute::VertexInstance::Tangent);
		const bool bHasVertexInstanceBinormalSigns =
			MeshDescription.VertexInstanceAttributes().HasAttribute(
				MeshAttribute::VertexInstance::BinormalSign);
		const bool bHasVertexInstanceUVs =
			MeshDescription.VertexInstanceAttributes().HasAttribute(
				MeshAttribute::VertexInstance::TextureCoordinate);
		const bool bHasVertexInstanceColors =
			MeshDescription.VertexInstanceAttributes().HasAttribute(
				MeshAttribute::VertexInstance::Color);
		const bool bHasPolygonGroupMaterialSlots =
			MeshDescription.PolygonGroupAttributes().HasAttribute(
				MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceNormals;
		if (bHasVertexInstanceNormals)
		{
			VertexInstanceNormals = MeshAttributes.GetVertexInstanceNormals();
		}
		TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceTangents;
		if (bHasVertexInstanceTangents)
		{
			VertexInstanceTangents = MeshAttributes.GetVertexInstanceTangents();
		}
		TVertexInstanceAttributesConstRef<float> VertexInstanceBinormalSigns;
		if (bHasVertexInstanceBinormalSigns)
		{
			VertexInstanceBinormalSigns = MeshAttributes.GetVertexInstanceBinormalSigns();
		}
		TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs;
		if (bHasVertexInstanceUVs)
		{
			VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();
		}
		TVertexInstanceAttributesConstRef<FVector4f> VertexInstanceColors;
		if (bHasVertexInstanceColors)
		{
			VertexInstanceColors = MeshAttributes.GetVertexInstanceColors();
		}
		TPolygonGroupAttributesConstRef<FName> PolygonGroupMaterialSlotNames;
		if (bHasPolygonGroupMaterialSlots)
		{
			PolygonGroupMaterialSlotNames = MeshAttributes.GetPolygonGroupMaterialSlotNames();
		}
		OutTriangles.Reserve(MeshDescription.Triangles().Num());

		for (const FTriangleID TriangleID :
			MeshDescription.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> TriangleVertexIDs =
				MeshDescription.GetTriangleVertices(TriangleID);
			if (TriangleVertexIDs.Num() != 3)
			{
				continue;
			}

			FSourceTriangle Triangle;
			Triangle.Vertices[0] = FVector(VertexPositions[TriangleVertexIDs[0]]);
			Triangle.Vertices[1] = FVector(VertexPositions[TriangleVertexIDs[1]]);
			Triangle.Vertices[2] = FVector(VertexPositions[TriangleVertexIDs[2]]);

			const FVector Edge01 = Triangle.Vertices[1] - Triangle.Vertices[0];
			const FVector Edge02 = Triangle.Vertices[2] - Triangle.Vertices[0];
			const FVector AreaVector = FVector::CrossProduct(Edge01, Edge02);
			const double DoubleArea = AreaVector.Size();
			if (DoubleArea <= DegenerateTriangleTolerance)
			{
				continue;
			}

			Triangle.Normal = AreaVector / DoubleArea;
			Triangle.ShadingNormal = Triangle.Normal;
			for (FVector& VertexNormal : Triangle.VertexNormals)
			{
				VertexNormal = Triangle.Normal;
			}
			if (bHasVertexInstanceNormals)
			{
				const TArrayView<const FVertexInstanceID>
					TriangleVertexInstanceIDs =
						MeshDescription.GetTriangleVertexInstances(
							TriangleID);
				FVector AveragedNormal = FVector::ZeroVector;
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						FVector SourceNormal = FVector(VertexInstanceNormals[TriangleVertexInstanceIDs[VertexIndex]]).GetSafeNormal();
						if (SourceNormal.IsNearlyZero())
						{
							SourceNormal = Triangle.Normal;
						}
						Triangle.VertexNormals[VertexIndex] = SourceNormal;
						AveragedNormal += SourceNormal;
					}

					if (!AveragedNormal.IsNearlyZero())
					{
						Triangle.ShadingNormal = AveragedNormal.GetSafeNormal();
						Triangle.bHasSourceShadingNormal = true;
					}
				}
			}
			if (bHasVertexInstanceTangents)
			{
				const TArrayView<const FVertexInstanceID>
					TriangleVertexInstanceIDs =
						MeshDescription.GetTriangleVertexInstances(
							TriangleID);
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					bool bAnyTangentValid = false;
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						FVector SourceTangent = FVector(VertexInstanceTangents[TriangleVertexInstanceIDs[VertexIndex]]).GetSafeNormal();
						if (SourceTangent.IsNearlyZero())
						{
							const FVector& VertexNormal = Triangle.VertexNormals[VertexIndex];
							const FVector ReferenceAxis = FMath::Abs(VertexNormal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
							SourceTangent = FVector::CrossProduct(ReferenceAxis, VertexNormal).GetSafeNormal();
							if (SourceTangent.IsNearlyZero())
							{
								SourceTangent = FVector::ForwardVector;
							}
						}
						else
						{
							bAnyTangentValid = true;
						}
						Triangle.VertexTangents[VertexIndex] = SourceTangent;
						Triangle.BinormalSigns[VertexIndex] = bHasVertexInstanceBinormalSigns
							? (VertexInstanceBinormalSigns[TriangleVertexInstanceIDs[VertexIndex]] >= 0.0f ? 1.0f : -1.0f)
							: 1.0f;
					}
					Triangle.bHasTangents = bAnyTangentValid;
				}
			}
			if (bHasVertexInstanceUVs && VertexInstanceUVs.GetNumChannels() > 0)
			{
				const TArrayView<const FVertexInstanceID>
					TriangleVertexInstanceIDs =
						MeshDescription.GetTriangleVertexInstances(
							TriangleID);
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					const int32 StoredUVChannelCount = FMath::Min(VertexInstanceUVs.GetNumChannels(), UE::FoliageBaker::PlaneCover::MaxSourceMeshUVChannels);
					for (int32 UVChannel = 0; UVChannel < StoredUVChannelCount; ++UVChannel)
					{
						for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
						{
							Triangle.UVChannels[UVChannel][VertexIndex] = VertexInstanceUVs.Get(TriangleVertexInstanceIDs[VertexIndex], UVChannel);
						}
					}
					Triangle.UVs[0] = Triangle.UVChannels[0][0];
					Triangle.UVs[1] = Triangle.UVChannels[0][1];
					Triangle.UVs[2] = Triangle.UVChannels[0][2];
					Triangle.NumUVChannels = StoredUVChannelCount;
					Triangle.bHasUVs = true;
				}
			}
			if (bHasVertexInstanceColors)
			{
				const TArrayView<const FVertexInstanceID>
					TriangleVertexInstanceIDs =
						MeshDescription.GetTriangleVertexInstances(
							TriangleID);
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						Triangle.VertexColors[VertexIndex] = VertexInstanceColors[TriangleVertexInstanceIDs[VertexIndex]];
					}
					Triangle.bHasVertexColors = true;
				}
			}
			if (bHasPolygonGroupMaterialSlots)
			{
				const FPolygonGroupID PolygonGroupID =
					MeshDescription.GetTrianglePolygonGroup(TriangleID);
				if (MeshDescription.IsPolygonGroupValid(PolygonGroupID))
				{
					const FName MaterialSlotName = PolygonGroupMaterialSlotNames[PolygonGroupID];
					int32 MaterialIndex =
						StaticMesh.GetMaterialIndex(MaterialSlotName);
					if (MaterialIndex == INDEX_NONE)
					{
						MaterialIndex =
							StaticMesh.GetMaterialIndexFromImportedMaterialSlotName(
								MaterialSlotName);
					}
					Triangle.MaterialIndex = MaterialIndex;
				}
			}
			Triangle.Area = 0.5 * DoubleArea;
			OutTriangles.Add(Triangle);
		}

		if (OutTriangles.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("No non-degenerate triangles found in source LOD %d on Static Mesh '%s'."),
				LODIndex,
				*StaticMesh.GetName());
			return false;
		}

		return true;
	}

	FVector ProjectPointToPlane(const FVector& Point, const FVector& PlaneNormal, const double PlaneRho)
	{
		const double SignedDistance = FVector::DotProduct(PlaneNormal, Point) - PlaneRho;
		return Point - PlaneNormal * SignedDistance;
	}

	bool BuildPlaneProxyMeshDescription(
		const TArray<FSourceTriangle>& Triangles,
		const TArray<FSourceTriangle>& CaptureBoundsTriangles,
		const FPlaneProxySet& Result,
		const FPlaneProxySettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& OutStats,
		FString& OutError,
		TArray<FPlaneProxyPlaneInfo>& OutPlaneInfos)
	{
		OutMeshDescription.Empty();
		OutStats = FPlaneProxyMeshStats();
		OutError.Reset();
		OutPlaneInfos.Reset();

		if (Triangles.IsEmpty())
		{
			OutError = TEXT("No source triangles are available.");
			return false;
		}

		if (Result.Planes.IsEmpty())
		{
			OutError = TEXT("The plane cover did not produce any planes.");
			return false;
		}
		if (CaptureBoundsTriangles.Num() != Triangles.Num())
		{
			OutError = TEXT("Capture-bounds triangle count does not match the source triangle count.");
			return false;
		}

		OutStats.SourceTriangleCount = Triangles.Num();
		for (const FSourceTriangle& Triangle : Triangles)
		{
			if (Triangle.bHasSourceShadingNormal)
			{
				++OutStats.SourceShadingNormalTriangleCount;
			}
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
		TPolygonGroupAttributesRef<FName> PolygonGroupImportedMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		VertexInstanceUVs.SetNumChannels(3);

		const int32 QuadsPerPlane = Settings.bEmitBackFaceGeometry ? 2 : 1;
		OutMeshDescription.ReserveNewVertices(Result.Planes.Num() * 4 * QuadsPerPlane);
		OutMeshDescription.ReserveNewVertexInstances(Result.Planes.Num() * 4 * QuadsPerPlane);
		OutMeshDescription.ReserveNewPolygons(Result.Planes.Num() * QuadsPerPlane);

		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = TEXT("BillboardProxy");

		const double Padding = FMath::Max(0.0, Settings.ErrorTolerance * 0.5);
		const double MinHalfExtent = FMath::Max(1.0, Settings.ErrorTolerance * 0.25);
		TArray<FPreparedProxyPlane> PreparedPlanes;
		PreparedPlanes.Reserve(Result.Planes.Num());

		for (int32 SourcePlaneIndex = 0; SourcePlaneIndex < Result.Planes.Num(); ++SourcePlaneIndex)
		{
			const FPlaneProxyInput& Plane = Result.Planes[SourcePlaneIndex];
			if (Plane.TriangleIndices.IsEmpty())
			{
				continue;
			}

			FVector OrientedNormal = Plane.Normal.GetSafeNormal();
			double OrientedRho = Plane.Rho;
			FVector WeightedShadingNormal = FVector::ZeroVector;
			for (const int32 TriangleIndex : Plane.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex))
				{
					const FSourceTriangle& Triangle = Triangles[TriangleIndex];
					WeightedShadingNormal += Triangle.Area * Triangle.ShadingNormal;
				}
			}

			FVector PlaneShadingNormal = WeightedShadingNormal.GetSafeNormal();
			if (PlaneShadingNormal.IsNearlyZero())
			{
				PlaneShadingNormal = OrientedNormal;
			}

			double PlaneToShadingNormalDot = FVector::DotProduct(PlaneShadingNormal, OrientedNormal);
			if (Plane.bIsTrunkCard)
			{
				PlaneShadingNormal = OrientedNormal;
				PlaneToShadingNormalDot = 1.0;
			}
			else if (FVector::DotProduct(PlaneShadingNormal, OrientedNormal) < 0.0)
			{
				OrientedNormal *= -1.0;
				OrientedRho *= -1.0;
				PlaneToShadingNormalDot *= -1.0;
			}

			FVector AxisU = FVector::RightVector;
			FVector AxisV = FVector::UpVector;
			if (Plane.bUseFixedPlaneFrame)
			{
				AxisU = (Plane.FixedAxisU - OrientedNormal * FVector::DotProduct(Plane.FixedAxisU, OrientedNormal)).GetSafeNormal();
				AxisV = (Plane.FixedAxisV - OrientedNormal * FVector::DotProduct(Plane.FixedAxisV, OrientedNormal) - AxisU * FVector::DotProduct(Plane.FixedAxisV, AxisU)).GetSafeNormal();
				if (AxisU.IsNearlyZero() || AxisV.IsNearlyZero())
				{
					BuildPlaneFrame(OrientedNormal, AxisU, AxisV);
				}
			}
			else
			{
				BuildPlaneFrame(OrientedNormal, AxisU, AxisV);
			}

			double MinU = TNumericLimits<double>::Max();
			double MaxU = -TNumericLimits<double>::Max();
			double MinV = TNumericLimits<double>::Max();
			double MaxV = -TNumericLimits<double>::Max();

			const bool bComputedRectangle = Plane.bUseFixedPlaneFrame
				? ComputeFixedFramePlaneRectangle(CaptureBoundsTriangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, AxisU, AxisV, MinU, MaxU, MinV, MaxV)
				: ComputeMinimumAreaPlaneRectangle(CaptureBoundsTriangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, AxisU, AxisV, MinU, MaxU, MinV, MaxV);
			if (!bComputedRectangle)
			{
				continue;
			}

			double MinSignedDistance = 0.0;
			double MaxSignedDistance = 0.0;
			ComputeSignedDistanceRangeForTriangles(CaptureBoundsTriangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, MinSignedDistance, MaxSignedDistance);
			const double EnvelopeMinU = MinU;
			const double EnvelopeMaxU = MaxU;
			const double EnvelopeMinV = MinV;
			const double EnvelopeMaxV = MaxV;
			const double EnvelopeMinSignedDistance = MinSignedDistance;
			const double EnvelopeMaxSignedDistance = MaxSignedDistance;

			if (MaxU - MinU < MinHalfExtent * 2.0)
			{
				const double CenterU = 0.5 * (MinU + MaxU);
				MinU = CenterU - MinHalfExtent;
				MaxU = CenterU + MinHalfExtent;
			}
			if (MaxV - MinV < MinHalfExtent * 2.0)
			{
				const double CenterV = 0.5 * (MinV + MaxV);
				MinV = CenterV - MinHalfExtent;
				MaxV = CenterV + MinHalfExtent;
			}

			MinU -= Padding;
			MaxU += Padding;
			MinV -= Padding;
			MaxV += Padding;
			MinSignedDistance -= Padding;
			MaxSignedDistance += Padding;

			const FVector PlaneOrigin = OrientedNormal * OrientedRho;
			FVector FrontCorners[4];
			FrontCorners[0] = PlaneOrigin + AxisU * MinU + AxisV * MinV;
			FrontCorners[1] = PlaneOrigin + AxisU * MaxU + AxisV * MinV;
			FrontCorners[2] = PlaneOrigin + AxisU * MaxU + AxisV * MaxV;
			FrontCorners[3] = PlaneOrigin + AxisU * MinU + AxisV * MaxV;

			FPreparedProxyPlane& PreparedPlane = PreparedPlanes.AddDefaulted_GetRef();
			PreparedPlane.SourcePlaneIndex = SourcePlaneIndex;
			PreparedPlane.bIsTrunkCard = Plane.bIsTrunkCard;
			PreparedPlane.OrientedNormal = OrientedNormal;
			PreparedPlane.OrientedRho = OrientedRho;
			PreparedPlane.AxisU = AxisU;
			PreparedPlane.AxisV = AxisV;
			PreparedPlane.ShadingNormal = PlaneShadingNormal;
			PreparedPlane.MinU = MinU;
			PreparedPlane.MaxU = MaxU;
			PreparedPlane.MinV = MinV;
			PreparedPlane.MaxV = MaxV;
			PreparedPlane.MinSignedDistance = MinSignedDistance;
			PreparedPlane.MaxSignedDistance = MaxSignedDistance;
			PreparedPlane.EnvelopeMinU = EnvelopeMinU;
			PreparedPlane.EnvelopeMaxU = EnvelopeMaxU;
			PreparedPlane.EnvelopeMinV = EnvelopeMinV;
			PreparedPlane.EnvelopeMaxV = EnvelopeMaxV;
			PreparedPlane.EnvelopeMinSignedDistance = EnvelopeMinSignedDistance;
			PreparedPlane.EnvelopeMaxSignedDistance = EnvelopeMaxSignedDistance;
			PreparedPlane.PlaneToShadingNormalDot = FMath::Clamp(PlaneToShadingNormalDot, -1.0, 1.0);
			PreparedPlane.TriangleIndices = Plane.TriangleIndices;
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PreparedPlane.Corners[CornerIndex] = FrontCorners[CornerIndex];
			}
		}

		if (PreparedPlanes.IsEmpty())
		{
			OutError = TEXT("No non-degenerate proxy planes could be prepared.");
			return false;
		}

		ApplyPlaneProxyEnvelopeCrackReduction(Triangles, Settings, PreparedPlanes);

		const double SourceMaxDimension = ComputeSourceBoundsMaxDimension(Triangles);
		if (!PackPreparedProxyPlanesIntoAtlas(
			PreparedPlanes,
			Settings,
			SourceMaxDimension,
			OutStats.AtlasWidth,
			OutStats.AtlasHeight,
			OutStats.AtlasTileResolution,
			OutStats.AtlasTilePaddingPixels))
		{
			OutError = TEXT("Could not pack billboard textures into the configured atlas resolution.");
			return false;
		}

		UpdateWorldTexelSizeStats(PreparedPlanes, OutStats);

		double PlaneToShadingNormalDotSum = 0.0;
		for (const FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
		{
			const FVector2f MaskUV = PreparedPlane.bIsTrunkCard
				? FVector2f(0.0f, 0.0f)
				: FVector2f(1.0f, 0.0f);
			const FVector2f (&FrontBackUVs)[4] = Settings.bEmitBackFaceGeometry
				? PreparedPlane.AtlasUVs
				: PreparedPlane.BackAtlasUVs;
			if (!AddQuadPolygon(OutMeshDescription, PolygonGroupID, VertexPositions, VertexInstanceUVs, VertexInstanceNormals, VertexInstanceTangents, VertexInstanceBinormalSigns, TriangleNormals, TriangleTangents, TriangleBinormals, EdgeHardnesses, PreparedPlane.Corners, PreparedPlane.AtlasUVs, FrontBackUVs, MaskUV, PreparedPlane.ShadingNormal, false))
			{
				continue;
			}
			int32 GeneratedQuadCount = 1;
			if (Settings.bEmitBackFaceGeometry && PreparedPlane.bHasBackFaceAtlas)
			{
				if (AddQuadPolygon(OutMeshDescription, PolygonGroupID, VertexPositions, VertexInstanceUVs, VertexInstanceNormals, VertexInstanceTangents, VertexInstanceBinormalSigns, TriangleNormals, TriangleTangents, TriangleBinormals, EdgeHardnesses, PreparedPlane.Corners, PreparedPlane.BackAtlasUVs, PreparedPlane.BackAtlasUVs, MaskUV, -PreparedPlane.ShadingNormal, true))
				{
					++GeneratedQuadCount;
				}
			}

			FPlaneProxyPlaneInfo& PlaneInfo = OutPlaneInfos.AddDefaulted_GetRef();
			PlaneInfo.SourcePlaneIndex = PreparedPlane.SourcePlaneIndex;
			PlaneInfo.bIsTrunkCard = PreparedPlane.bIsTrunkCard;
			PlaneInfo.Normal = PreparedPlane.OrientedNormal;
			PlaneInfo.Rho = PreparedPlane.OrientedRho;
			PlaneInfo.AxisU = PreparedPlane.AxisU;
			PlaneInfo.AxisV = PreparedPlane.AxisV;
			PlaneInfo.ShadingNormal = PreparedPlane.ShadingNormal;
			PlaneInfo.MinU = PreparedPlane.MinU;
			PlaneInfo.MaxU = PreparedPlane.MaxU;
			PlaneInfo.MinV = PreparedPlane.MinV;
			PlaneInfo.MaxV = PreparedPlane.MaxV;
			PlaneInfo.MinSignedDistance = PreparedPlane.MinSignedDistance;
			PlaneInfo.MaxSignedDistance = PreparedPlane.MaxSignedDistance;
			PlaneInfo.EnvelopeMinU = PreparedPlane.EnvelopeMinU;
			PlaneInfo.EnvelopeMaxU = PreparedPlane.EnvelopeMaxU;
			PlaneInfo.EnvelopeMinV = PreparedPlane.EnvelopeMinV;
			PlaneInfo.EnvelopeMaxV = PreparedPlane.EnvelopeMaxV;
			PlaneInfo.EnvelopeMinSignedDistance = PreparedPlane.EnvelopeMinSignedDistance;
			PlaneInfo.EnvelopeMaxSignedDistance = PreparedPlane.EnvelopeMaxSignedDistance;
			PlaneInfo.AtlasPixelMin = PreparedPlane.AtlasPixelMin;
			PlaneInfo.AtlasTileSize = PreparedPlane.AtlasTileSize;
			PlaneInfo.BackAtlasPixelMin = PreparedPlane.BackAtlasPixelMin;
			PlaneInfo.BackAtlasTileSize = PreparedPlane.BackAtlasTileSize;
			PlaneInfo.AtlasTileResolution =
				FMath::Min(PreparedPlane.AtlasTileSize.X, PreparedPlane.AtlasTileSize.Y);
			PlaneInfo.AtlasTilePaddingPixels = PreparedPlane.AtlasTilePaddingPixels;
			PlaneInfo.bHasBackFaceAtlas = PreparedPlane.bHasBackFaceAtlas;
			PlaneInfo.TriangleIndices = PreparedPlane.TriangleIndices;
			PlaneInfo.CrackReductionProjections = PreparedPlane.CrackReductionProjections;
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PlaneInfo.Corners[CornerIndex] = PreparedPlane.Corners[CornerIndex];
				PlaneInfo.AtlasUVs[CornerIndex] = PreparedPlane.AtlasUVs[CornerIndex];
				PlaneInfo.BackAtlasUVs[CornerIndex] = PreparedPlane.BackAtlasUVs[CornerIndex];
			}

			PlaneToShadingNormalDotSum += PreparedPlane.PlaneToShadingNormalDot;
			++OutStats.PlaneCount;
			OutStats.QuadCount += GeneratedQuadCount;
		}

		OutStats.TriangleCount = OutMeshDescription.Triangles().Num();
		if (OutStats.PlaneCount == 0)
		{
			OutError = TEXT("No non-degenerate proxy planes could be built.");
			return false;
		}

		OutStats.AveragePlaneToShadingNormalDot = PlaneToShadingNormalDotSum / static_cast<double>(OutStats.PlaneCount);
		OutStats.AveragePlaneToShadingNormalAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(OutStats.AveragePlaneToShadingNormalDot, -1.0, 1.0)));

		return true;
	}

	bool RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
		const TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
		const FPlaneProxySettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& InOutStats,
		FString& OutError)
	{
		OutError.Reset();
		if (PlaneInfos.IsEmpty())
		{
			OutError = TEXT("No proxy planes are available for mesh reconstruction.");
			return false;
		}

		OutMeshDescription.Empty();
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
		TPolygonGroupAttributesRef<FName> PolygonGroupImportedMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		VertexInstanceUVs.SetNumChannels(3);
		const int32 QuadsPerPlane = Settings.bEmitBackFaceGeometry ? 2 : 1;
		OutMeshDescription.ReserveNewVertices(PlaneInfos.Num() * 4 * QuadsPerPlane);
		OutMeshDescription.ReserveNewVertexInstances(PlaneInfos.Num() * 4 * QuadsPerPlane);
		OutMeshDescription.ReserveNewPolygons(PlaneInfos.Num() * QuadsPerPlane);

		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = TEXT("BillboardProxy");

		InOutStats.PlaneCount = 0;
		InOutStats.QuadCount = 0;
		for (const FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const FVector2f AuxiliaryUV = PlaneInfo.bUseCustomAuxiliaryUV
				? PlaneInfo.AuxiliaryUV
				: PlaneInfo.bIsTrunkCard
					? FVector2f(0.0f, 0.0f)
					: FVector2f(1.0f, 0.0f);
			const FVector2f (&FrontBackUVs)[4] = Settings.bEmitBackFaceGeometry
				? PlaneInfo.AtlasUVs
				: PlaneInfo.BackAtlasUVs;
			if (!AddQuadPolygon(
				OutMeshDescription,
				PolygonGroupID,
				VertexPositions,
				VertexInstanceUVs,
				VertexInstanceNormals,
				VertexInstanceTangents,
				VertexInstanceBinormalSigns,
				TriangleNormals,
				TriangleTangents,
				TriangleBinormals,
				EdgeHardnesses,
				PlaneInfo.Corners,
				PlaneInfo.AtlasUVs,
				FrontBackUVs,
				AuxiliaryUV,
				PlaneInfo.ShadingNormal,
				false))
			{
				continue;
			}

			int32 GeneratedQuadCount = 1;
			if (Settings.bEmitBackFaceGeometry && PlaneInfo.bHasBackFaceAtlas)
			{
				if (AddQuadPolygon(
					OutMeshDescription,
					PolygonGroupID,
					VertexPositions,
					VertexInstanceUVs,
					VertexInstanceNormals,
					VertexInstanceTangents,
					VertexInstanceBinormalSigns,
					TriangleNormals,
					TriangleTangents,
					TriangleBinormals,
					EdgeHardnesses,
					PlaneInfo.Corners,
					PlaneInfo.BackAtlasUVs,
					PlaneInfo.BackAtlasUVs,
					AuxiliaryUV,
					-PlaneInfo.ShadingNormal,
					true))
				{
					++GeneratedQuadCount;
				}
			}

			++InOutStats.PlaneCount;
			InOutStats.QuadCount += GeneratedQuadCount;
		}

		InOutStats.TriangleCount = OutMeshDescription.Triangles().Num();
		if (InOutStats.PlaneCount == 0)
		{
			OutError = TEXT("No proxy planes could be reconstructed.");
			return false;
		}
		UpdateWorldTexelSizeStats(PlaneInfos, InOutStats);
		return true;
	}

	bool ApplySharedPlaneProxyBoundsAndRebuildMeshDescription(
		TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
		const FPlaneProxySettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& InOutStats,
		FString& OutError)
	{
		OutError.Reset();
		if (PlaneInfos.IsEmpty())
		{
			OutError = TEXT("No proxy planes are available for shared bounds.");
			return false;
		}

		double SharedMinU = TNumericLimits<double>::Max();
		double SharedMaxU = -TNumericLimits<double>::Max();
		double SharedMinV = TNumericLimits<double>::Max();
		double SharedMaxV = -TNumericLimits<double>::Max();
		for (const FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			SharedMinU = FMath::Min(SharedMinU, PlaneInfo.MinU);
			SharedMaxU = FMath::Max(SharedMaxU, PlaneInfo.MaxU);
			SharedMinV = FMath::Min(SharedMinV, PlaneInfo.MinV);
			SharedMaxV = FMath::Max(SharedMaxV, PlaneInfo.MaxV);
		}

		if (!FMath::IsFinite(SharedMinU)
			|| !FMath::IsFinite(SharedMaxU)
			|| !FMath::IsFinite(SharedMinV)
			|| !FMath::IsFinite(SharedMaxV)
			|| SharedMaxU <= SharedMinU
			|| SharedMaxV <= SharedMinV)
		{
			OutError = TEXT("Could not resolve valid shared proxy-plane bounds.");
			return false;
		}

		for (FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			PlaneInfo.MinU = SharedMinU;
			PlaneInfo.MaxU = SharedMaxU;
			PlaneInfo.MinV = SharedMinV;
			PlaneInfo.MaxV = SharedMaxV;
			UpdatePlaneInfoCornersFromBounds(PlaneInfo);
		}

		if (!PackSharedTwoPlaneInfosIntoAtlas(
			PlaneInfos,
			Settings,
			InOutStats.AtlasWidth,
			InOutStats.AtlasHeight,
			InOutStats.AtlasTileResolution,
			InOutStats.AtlasTilePaddingPixels))
		{
			OutError = TEXT("Could not pack shared billboard texture tiles into the configured atlas resolution.");
			return false;
		}

		return RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			PlaneInfos,
			Settings,
			OutMeshDescription,
			InOutStats,
			OutError);
	}

	bool ApplyGroupedPlaneProxyBoundsAndRebuildMeshDescription(
		TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
		const TArray<int32>& PlaneGroupIndices,
		const FPlaneProxySettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& InOutStats,
		FString& OutError)
	{
		OutError.Reset();
		if (PlaneInfos.IsEmpty())
		{
			OutError = TEXT("No proxy planes are available for grouped bounds.");
			return false;
		}
		if (PlaneGroupIndices.Num() != PlaneInfos.Num())
		{
			OutError = TEXT("Proxy-plane group count does not match proxy plane count.");
			return false;
		}

		struct FGroupedBounds
		{
			double MinU = TNumericLimits<double>::Max();
			double MaxU = -TNumericLimits<double>::Max();
			double MinV = TNumericLimits<double>::Max();
			double MaxV = -TNumericLimits<double>::Max();
		};

		TMap<int32, FGroupedBounds> BoundsByGroup;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			const int32 GroupIndex = PlaneGroupIndices[PlaneIndex];
			if (GroupIndex < 0)
			{
				OutError = TEXT("Proxy-plane group indices must be non-negative.");
				return false;
			}
			const FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			FGroupedBounds& Bounds = BoundsByGroup.FindOrAdd(GroupIndex);
			Bounds.MinU = FMath::Min(Bounds.MinU, PlaneInfo.MinU);
			Bounds.MaxU = FMath::Max(Bounds.MaxU, PlaneInfo.MaxU);
			Bounds.MinV = FMath::Min(Bounds.MinV, PlaneInfo.MinV);
			Bounds.MaxV = FMath::Max(Bounds.MaxV, PlaneInfo.MaxV);
		}

		for (const TPair<int32, FGroupedBounds>& Pair : BoundsByGroup)
		{
			const FGroupedBounds& Bounds = Pair.Value;
			if (!FMath::IsFinite(Bounds.MinU)
				|| !FMath::IsFinite(Bounds.MaxU)
				|| !FMath::IsFinite(Bounds.MinV)
				|| !FMath::IsFinite(Bounds.MaxV)
				|| Bounds.MaxU <= Bounds.MinU
				|| Bounds.MaxV <= Bounds.MinV)
			{
				OutError = FString::Printf(
					TEXT("Could not resolve valid shared proxy-plane bounds for group %d."),
					Pair.Key);
				return false;
			}
		}

		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			const FGroupedBounds& Bounds = BoundsByGroup.FindChecked(PlaneGroupIndices[PlaneIndex]);
			PlaneInfo.MinU = Bounds.MinU;
			PlaneInfo.MaxU = Bounds.MaxU;
			PlaneInfo.MinV = Bounds.MinV;
			PlaneInfo.MaxV = Bounds.MaxV;
			UpdatePlaneInfoCornersFromBounds(PlaneInfo);
		}

		if (!PackPlaneInfosIntoAtlas(
			PlaneInfos,
			Settings,
			InOutStats.AtlasWidth,
			InOutStats.AtlasHeight,
			InOutStats.AtlasTileResolution,
			InOutStats.AtlasTilePaddingPixels))
		{
			OutError = TEXT("Could not pack grouped billboard texture tiles into the configured atlas resolution.");
			return false;
		}

		return RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			PlaneInfos,
			Settings,
			OutMeshDescription,
			InOutStats,
			OutError);
	}

	bool ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
		TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
		const TArray<FPlaneProxyTileCrop>& TileCrops,
		const FPlaneProxySettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& InOutStats,
		FString& OutError,
		const bool bUseSharedTwoPlaneLayout)
	{
		OutError.Reset();
		if (PlaneInfos.IsEmpty())
		{
			OutError = TEXT("No proxy planes are available for alpha-aware tile crop.");
			return false;
		}
		if (TileCrops.Num() != PlaneInfos.Num())
		{
			OutError = TEXT("Alpha-aware tile crop count does not match proxy plane count.");
			return false;
		}

		bool bAppliedAnyCrop = false;
		for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
		{
			FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
			const FPlaneProxyTileCrop& Crop = TileCrops[PlaneIndex];
			if (!Crop.bEnabled)
			{
				UpdatePlaneInfoCornersFromBounds(PlaneInfo);
				continue;
			}

			const double MinUFraction = FMath::Clamp(Crop.MinUFraction, 0.0, 1.0);
			const double MaxUFraction = FMath::Clamp(Crop.MaxUFraction, 0.0, 1.0);
			const double MinVFraction = FMath::Clamp(Crop.MinVFraction, 0.0, 1.0);
			const double MaxVFraction = FMath::Clamp(Crop.MaxVFraction, 0.0, 1.0);
			if (MaxUFraction <= MinUFraction || MaxVFraction <= MinVFraction)
			{
				UpdatePlaneInfoCornersFromBounds(PlaneInfo);
				continue;
			}

			const double OldMinU = PlaneInfo.MinU;
			const double OldMaxU = PlaneInfo.MaxU;
			const double OldMinV = PlaneInfo.MinV;
			const double OldMaxV = PlaneInfo.MaxV;
			const bool bFlipTextureV = Settings.AtlasVConvention == EAtlasVConvention::GeometryMinVToTextureMaxV;
			const double GeometryMinVFraction = bFlipTextureV ? 1.0 - MaxVFraction : MinVFraction;
			const double GeometryMaxVFraction = bFlipTextureV ? 1.0 - MinVFraction : MaxVFraction;
			PlaneInfo.MinU = FMath::Lerp(OldMinU, OldMaxU, MinUFraction);
			PlaneInfo.MaxU = FMath::Lerp(OldMinU, OldMaxU, MaxUFraction);
			PlaneInfo.MinV = FMath::Lerp(OldMinV, OldMaxV, GeometryMinVFraction);
			PlaneInfo.MaxV = FMath::Lerp(OldMinV, OldMaxV, GeometryMaxVFraction);
			UpdatePlaneInfoCornersFromBounds(PlaneInfo);
			bAppliedAnyCrop = true;
		}

		if (!bAppliedAnyCrop)
		{
			return true;
		}

		const bool bPacked = bUseSharedTwoPlaneLayout
			? PackSharedTwoPlaneInfosIntoAtlas(
				PlaneInfos,
				Settings,
				InOutStats.AtlasWidth,
				InOutStats.AtlasHeight,
				InOutStats.AtlasTileResolution,
				InOutStats.AtlasTilePaddingPixels)
			: PackPlaneInfosIntoAtlas(
				PlaneInfos,
				Settings,
				InOutStats.AtlasWidth,
				InOutStats.AtlasHeight,
				InOutStats.AtlasTileResolution,
				InOutStats.AtlasTilePaddingPixels);
		if (!bPacked)
		{
			OutError = TEXT("Could not repack alpha-cropped billboard texture tiles into the configured atlas resolution.");
			return false;
		}

		return RebuildPlaneProxyMeshDescriptionFromPlaneInfos(
			PlaneInfos,
			Settings,
			OutMeshDescription,
			InOutStats,
			OutError);
	}

}
