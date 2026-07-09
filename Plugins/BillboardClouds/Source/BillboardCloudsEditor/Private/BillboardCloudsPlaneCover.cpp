#include "BillboardCloudsPlaneCover.h"

#include "Async/ParallelFor.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "MeshDescription.h"
#include "RawIndexBuffer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace UE::BillboardClouds
{
	namespace
	{
		constexpr double DegenerateTriangleTolerance = 1.0e-8;
		constexpr double RhoRangePadding = 1.0e-4;
		constexpr double PaperPenaltyWeight = 10.0;

		struct FRhoBinAccumulator
		{
			double Density = 0.0;
			FVector Normal = FVector::UpVector;
			double ThetaCenter = 0.0;
			double PhiCenter = 0.0;
			double ThetaHalfExtent = 0.0;
			double PhiHalfExtent = 0.0;
			double RhoMin = 0.0;
			double RhoMax = 0.0;
		};

		struct FPlaneSpaceDensityGrid
		{
			TArray<FRhoBinAccumulator> Bins;
			int32 ThetaSteps = 0;
			int32 PhiSteps = 0;
			int32 RhoBinCount = 0;
			double ThetaHalfExtent = 0.0;
			double PhiHalfExtent = 0.0;
			double GlobalRhoMin = 0.0;
			double RhoBinWidth = 1.0;
		};

		struct FPaperRefinementCell
		{
			double ThetaCenter = 0.0;
			double PhiCenter = UE_DOUBLE_PI * 0.5;
			double ThetaHalfExtent = 0.0;
			double PhiHalfExtent = 0.0;
			double RhoCenter = 0.0;
			double RhoHalfExtent = 0.0;
			int32 Depth = 0;
		};

		struct FPreparedCandidate
		{
			FCandidatePlane Plane;
			TArray<int32> ValidTriangleIndices;
			double ValidContribution = 0.0;
			double SelectionScore = 0.0;
			double CoveredArea = 0.0;
		};

		struct FPaperCellEvaluation
		{
			FVector Normal = FVector::UpVector;
			double Rho = 0.0;
			double Density = 0.0;
			double ValidContribution = 0.0;
			double CoveredArea = 0.0;
			int32 SimpleValidTriangleCount = 0;
			bool bCenterValidForAllSimpleTriangles = false;
			TArray<int32> SimpleValidTriangleIndices;
		};

		struct FPaperCellScore
		{
			double Density = 0.0;
			int32 SimpleValidTriangleCount = 0;
		};

		struct FAngularBinContext
		{
			double ThetaMin = 0.0;
			double ThetaMax = 0.0;
			double PhiMin = 0.0;
			double PhiMax = 0.0;
			FVector CornerNormals[4] = { FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector };
		};

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

		FVector NormalFromPlaneSpace(double Theta, double Phi);

		double UnwrapThetaNear(const double Theta, const double ReferenceTheta)
		{
			double UnwrappedTheta = Theta;
			while (UnwrappedTheta < ReferenceTheta - UE_DOUBLE_PI)
			{
				UnwrappedTheta += UE_TWO_PI;
			}
			while (UnwrappedTheta > ReferenceTheta + UE_DOUBLE_PI)
			{
				UnwrappedTheta -= UE_TWO_PI;
			}
			return UnwrappedTheta;
		}

		bool IsThetaInside(const double Theta, const double ThetaMin, const double ThetaMax)
		{
			const double CenterTheta = 0.5 * (ThetaMin + ThetaMax);
			const double UnwrappedTheta = UnwrapThetaNear(Theta, CenterTheta);
			return UnwrappedTheta >= ThetaMin - 1.0e-8 && UnwrappedTheta <= ThetaMax + 1.0e-8;
		}

		void AddAngularDotCandidate(
			const FAngularBinContext& Context,
			const FVector& Point,
			const double Theta,
			const double Phi,
			double& InOutMinDot,
			double& InOutMaxDot)
		{
			if (Phi < Context.PhiMin - 1.0e-8 || Phi > Context.PhiMax + 1.0e-8 || !IsThetaInside(Theta, Context.ThetaMin, Context.ThetaMax))
			{
				return;
			}

			const double Dot = FVector::DotProduct(Point, NormalFromPlaneSpace(Theta, Phi));
			InOutMinDot = FMath::Min(InOutMinDot, Dot);
			InOutMaxDot = FMath::Max(InOutMaxDot, Dot);
		}

		void ComputeAngularDotRangeForPoint(
			const FAngularBinContext& Context,
			const FVector& Point,
			double& OutMinDot,
			double& OutMaxDot)
		{
			OutMinDot = TNumericLimits<double>::Max();
			OutMaxDot = -TNumericLimits<double>::Max();

			for (const FVector& CornerNormal : Context.CornerNormals)
			{
				const double Dot = FVector::DotProduct(Point, CornerNormal);
				OutMinDot = FMath::Min(OutMinDot, Dot);
				OutMaxDot = FMath::Max(OutMaxDot, Dot);
			}

			if (Point.IsNearlyZero())
			{
				return;
			}

			const FVector PointDirection = Point.GetSafeNormal();
			const double PointTheta = FMath::Atan2(PointDirection.Y, PointDirection.X);
			const double PointPhi = FMath::Acos(FMath::Clamp(PointDirection.Z, -1.0, 1.0));
			const double OppositeTheta = PointTheta + UE_DOUBLE_PI;
			const double OppositePhi = UE_DOUBLE_PI - PointPhi;

			AddAngularDotCandidate(Context, Point, PointTheta, PointPhi, OutMinDot, OutMaxDot);
			AddAngularDotCandidate(Context, Point, OppositeTheta, OppositePhi, OutMinDot, OutMaxDot);
			AddAngularDotCandidate(Context, Point, PointTheta, FMath::Clamp(PointPhi, Context.PhiMin, Context.PhiMax), OutMinDot, OutMaxDot);
			AddAngularDotCandidate(Context, Point, OppositeTheta, FMath::Clamp(OppositePhi, Context.PhiMin, Context.PhiMax), OutMinDot, OutMaxDot);

			const double BoundaryThetas[2] = { Context.ThetaMin, Context.ThetaMax };
			for (const double BoundaryTheta : BoundaryThetas)
			{
				const double DeltaTheta = BoundaryTheta - PointTheta;
				const double A = FMath::Sin(PointPhi) * FMath::Cos(DeltaTheta);
				const double B = FMath::Cos(PointPhi);
				double PhiExtremum = FMath::Atan2(A, B);
				if (PhiExtremum < 0.0)
				{
					PhiExtremum += UE_DOUBLE_PI;
				}

				AddAngularDotCandidate(Context, Point, BoundaryTheta, PhiExtremum, OutMinDot, OutMaxDot);
				AddAngularDotCandidate(Context, Point, BoundaryTheta, PhiExtremum + UE_DOUBLE_PI, OutMinDot, OutMaxDot);
			}

			const double BoundaryPhis[2] = { Context.PhiMin, Context.PhiMax };
			for (const double BoundaryPhi : BoundaryPhis)
			{
				AddAngularDotCandidate(Context, Point, PointTheta, BoundaryPhi, OutMinDot, OutMaxDot);
				AddAngularDotCandidate(Context, Point, OppositeTheta, BoundaryPhi, OutMinDot, OutMaxDot);
			}
		}

		bool ComputeVertexRhoRangeForNormal(
			const FSourceTriangle& Triangle,
			const int32 VertexIndex,
			const FVector& PlaneNormal,
			const FPlaneCoverSettings& Settings,
			double& OutMinRho,
			double& OutMaxRho)
		{
			const double Rho = FVector::DotProduct(PlaneNormal, Triangle.Vertices[VertexIndex]);
			OutMinRho = Rho - Settings.ErrorTolerance;
			OutMaxRho = Rho + Settings.ErrorTolerance;
			return true;
		}

		bool ComputeValidityInterval(const FSourceTriangle& Triangle, const FVector& PlaneNormal, const FPlaneCoverSettings& Settings, double& OutValidMin, double& OutValidMax)
		{
			OutValidMin = -TNumericLimits<double>::Max();
			OutValidMax = TNumericLimits<double>::Max();
			for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
			{
				double VertexMinRho = 0.0;
				double VertexMaxRho = 0.0;
				ComputeVertexRhoRangeForNormal(Triangle, VertexIndex, PlaneNormal, Settings, VertexMinRho, VertexMaxRho);
				OutValidMin = FMath::Max(OutValidMin, VertexMinRho);
				OutValidMax = FMath::Min(OutValidMax, VertexMaxRho);
			}
			return OutValidMax >= OutValidMin;
		}

		bool IsTriangleValidForFinalPlane(const FSourceTriangle& Triangle, const FCandidatePlane& CandidatePlane, const FPlaneCoverSettings& Settings)
		{
			double ValidMin = 0.0;
			double ValidMax = 0.0;
			return ComputeValidityInterval(Triangle, CandidatePlane.Normal, Settings, ValidMin, ValidMax)
				&& CandidatePlane.Rho >= ValidMin
				&& CandidatePlane.Rho <= ValidMax;
		}

		double ComputeContribution(const FSourceTriangle& Triangle, const FVector& PlaneNormal)
		{
			return Triangle.Area * FMath::Abs(FVector::DotProduct(Triangle.Normal, PlaneNormal));
		}

		double WrapTheta(const double Theta)
		{
			double WrappedTheta = FMath::Fmod(Theta, UE_TWO_PI);
			if (WrappedTheta < 0.0)
			{
				WrappedTheta += UE_TWO_PI;
			}
			return WrappedTheta;
		}

		FVector NormalFromPlaneSpace(const double Theta, const double Phi)
		{
			const double ClampedPhi = FMath::Clamp(Phi, 0.0, UE_DOUBLE_PI);
			const double SinPhi = FMath::Sin(ClampedPhi);
			const double CosPhi = FMath::Cos(ClampedPhi);
			return FVector(SinPhi * FMath::Cos(Theta), SinPhi * FMath::Sin(Theta), CosPhi).GetSafeNormal();
		}

		FAngularBinContext MakeAngularBinContext(
			const double ThetaCenter,
			const double ThetaHalfExtent,
			const double PhiCenter,
			const double PhiHalfExtent)
		{
			FAngularBinContext Context;
			Context.ThetaMin = ThetaCenter - ThetaHalfExtent;
			Context.ThetaMax = ThetaCenter + ThetaHalfExtent;
			Context.PhiMin = FMath::Clamp(PhiCenter - PhiHalfExtent, 0.0, UE_DOUBLE_PI);
			Context.PhiMax = FMath::Clamp(PhiCenter + PhiHalfExtent, 0.0, UE_DOUBLE_PI);
			Context.CornerNormals[0] = NormalFromPlaneSpace(Context.ThetaMin, Context.PhiMin);
			Context.CornerNormals[1] = NormalFromPlaneSpace(Context.ThetaMin, Context.PhiMax);
			Context.CornerNormals[2] = NormalFromPlaneSpace(Context.ThetaMax, Context.PhiMin);
			Context.CornerNormals[3] = NormalFromPlaneSpace(Context.ThetaMax, Context.PhiMax);
			return Context;
		}

		bool IntervalsTouchOrOverlap(const double AMin, const double AMax, const double BMin, const double BMax)
		{
			return AMax >= BMin && BMax >= AMin;
		}

		bool ComputeAngularBinValidityRhoRangeForContext(
			const FSourceTriangle& Triangle,
			const FAngularBinContext& Context,
			const FPlaneCoverSettings& Settings,
			double& OutValidMin,
			double& OutValidMax)
		{
			if (Context.PhiMax < Context.PhiMin)
			{
				return false;
			}

			OutValidMin = -TNumericLimits<double>::Max();
			OutValidMax = TNumericLimits<double>::Max();

			for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
			{
				double VertexProjectionMin = TNumericLimits<double>::Max();
				double VertexProjectionMax = -TNumericLimits<double>::Max();

				ComputeAngularDotRangeForPoint(Context, Triangle.Vertices[VertexIndex], VertexProjectionMin, VertexProjectionMax);
				VertexProjectionMin -= Settings.ErrorTolerance;
				VertexProjectionMax += Settings.ErrorTolerance;

				OutValidMin = FMath::Max(OutValidMin, VertexProjectionMin);
				OutValidMax = FMath::Min(OutValidMax, VertexProjectionMax);
			}

			return OutValidMax >= OutValidMin;
		}

		bool ComputeAngularBinValidityRhoRange(
			const FSourceTriangle& Triangle,
			const double ThetaCenter,
			const double ThetaHalfExtent,
			const double PhiCenter,
			const double PhiHalfExtent,
			const FPlaneCoverSettings& Settings,
			double& OutValidMin,
			double& OutValidMax)
		{
			const FAngularBinContext Context = MakeAngularBinContext(ThetaCenter, ThetaHalfExtent, PhiCenter, PhiHalfExtent);
			return ComputeAngularBinValidityRhoRangeForContext(Triangle, Context, Settings, OutValidMin, OutValidMax);
		}

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

		int32 RoundUpToPowerOfTwoWithinLimit(const int32 Value, const int32 MaxValue)
		{
			const int32 ClampedValue = FMath::Max(1, Value);
			int32 PowerOfTwo = 1;
			while (PowerOfTwo < ClampedValue && PowerOfTwo < MaxValue)
			{
				PowerOfTwo <<= 1;
			}
			return PowerOfTwo <= MaxValue ? PowerOfTwo : 0;
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
			FVector2f OutAtlasUVs[4])
		{
			const double SafeAtlasWidth = static_cast<double>(FMath::Max(1, AtlasWidth));
			const double SafeAtlasHeight = static_cast<double>(FMath::Max(1, AtlasHeight));
			const double MinU = static_cast<double>(AtlasPixelMin.X) / SafeAtlasWidth;
			const double MinV = static_cast<double>(AtlasPixelMin.Y) / SafeAtlasHeight;
			const double MaxU = static_cast<double>(AtlasPixelMin.X + AtlasTileSize.X) / SafeAtlasWidth;
			const double MaxV = static_cast<double>(AtlasPixelMin.Y + AtlasTileSize.Y) / SafeAtlasHeight;

			OutAtlasUVs[0] = FVector2f(static_cast<float>(MinU), static_cast<float>(MinV));
			OutAtlasUVs[1] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MinV));
			OutAtlasUVs[2] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MaxV));
			OutAtlasUVs[3] = FVector2f(static_cast<float>(MinU), static_cast<float>(MaxV));
		}

		bool ShouldBakeBackFaceAtlas(const FPreparedProxyPlane& Plane, const FPlaneCoverSettings& Settings)
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
			int32 Padding = 0;
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
				OutInteriorMins[RectIndex] = FIntPoint(UsedRect.X + Rect.Padding, UsedRect.Y + Rect.Padding);
				SplitFreeRects(FreeRects, UsedRect);
			}

			return true;
		}

		bool PackPreparedProxyPlanesIntoAtlas(
			TArray<FPreparedProxyPlane>& PreparedPlanes,
			const FPlaneCoverSettings& Settings,
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
			const int32 RequestedPadding = FMath::Clamp(Settings.TextureTilePaddingPixels, 0, 128);
			const int32 AtlasResolution = FMath::Clamp(Settings.TextureAtlasResolution, 256, 8192);

			double MaxPlaneDimension = 1.0;
			for (const FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
			{
				const double AtlasResolutionScale = PreparedPlane.bIsTrunkCard
					? FMath::Max(1.0, Settings.TrunkCardAtlasScale)
					: 1.0;
				MaxPlaneDimension = FMath::Max(
					MaxPlaneDimension,
					FMath::Max(PreparedPlane.MaxU - PreparedPlane.MinU, PreparedPlane.MaxV - PreparedPlane.MinV) * AtlasResolutionScale);
			}

			auto BuildPackRects = [&](const double PixelsPerUnit, TArray<FAtlasPackRect>& OutPackRects)
			{
				OutPackRects.Reset();
				for (int32 PlaneIndex = 0; PlaneIndex < PreparedPlanes.Num(); ++PlaneIndex)
				{
					FPreparedProxyPlane& PreparedPlane = PreparedPlanes[PlaneIndex];
					const double AtlasResolutionScale = PreparedPlane.bIsTrunkCard
						? FMath::Max(1.0, Settings.TrunkCardAtlasScale)
						: 1.0;
					const double PlaneWidth = FMath::Max(PreparedPlane.MaxU - PreparedPlane.MinU, 1.0) * AtlasResolutionScale;
					const double PlaneHeight = FMath::Max(PreparedPlane.MaxV - PreparedPlane.MinV, 1.0) * AtlasResolutionScale;
					const int32 InteriorWidth = FMath::Max(1, FMath::CeilToInt(PlaneWidth * PixelsPerUnit));
					const int32 InteriorHeight = FMath::Max(1, FMath::CeilToInt(PlaneHeight * PixelsPerUnit));
					const int32 EffectivePadding = FMath::Clamp(RequestedPadding, 0, FMath::Max(0, (FMath::Min(InteriorWidth, InteriorHeight) - 1) / 2));

					FAtlasPackRect FrontRect;
					FrontRect.PlaneIndex = PlaneIndex;
					FrontRect.bBackFace = false;
					FrontRect.InteriorSize = FIntPoint(InteriorWidth, InteriorHeight);
					FrontRect.Padding = EffectivePadding;
					FrontRect.PitchSize = FIntPoint(InteriorWidth + EffectivePadding * 2, InteriorHeight + EffectivePadding * 2);
					OutPackRects.Add(FrontRect);

					if (ShouldBakeBackFaceAtlas(PreparedPlane, Settings))
					{
						FAtlasPackRect BackRect = FrontRect;
						BackRect.bBackFace = true;
						OutPackRects.Add(BackRect);
					}
				}
			};

			TArray<FAtlasPackRect> CandidatePackRects;
			TArray<FAtlasPackRect> BestPackRects;
			TArray<FIntPoint> CandidateInteriorMins;
			TArray<FIntPoint> BestInteriorMins;
			double LowScale = 0.0;
			double HighScale = static_cast<double>(AtlasResolution) / MaxPlaneDimension;
			bool bFoundPack = false;
			for (int32 Iteration = 0; Iteration < 28; ++Iteration)
			{
				const double MidScale = 0.5 * (LowScale + HighScale);
				BuildPackRects(MidScale, CandidatePackRects);
				if (TryPackAtlasRects(CandidatePackRects, AtlasResolution, CandidateInteriorMins))
				{
					LowScale = MidScale;
					BestPackRects = CandidatePackRects;
					BestInteriorMins = CandidateInteriorMins;
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
				if (!TryPackAtlasRects(CandidatePackRects, AtlasResolution, CandidateInteriorMins))
				{
					return false;
				}
				BestPackRects = CandidatePackRects;
				BestInteriorMins = CandidateInteriorMins;
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
				PreparedPlane.AtlasTilePaddingPixels = FMath::Max(PreparedPlane.AtlasTilePaddingPixels, PackRect.Padding);
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
				SetAtlasUVsFromTile(PreparedPlane.AtlasPixelMin, PreparedPlane.AtlasTileSize, OutAtlasWidth, OutAtlasHeight, PreparedPlane.AtlasUVs);
				if (PreparedPlane.bHasBackFaceAtlas)
				{
					SetAtlasUVsFromTile(PreparedPlane.BackAtlasPixelMin, PreparedPlane.BackAtlasTileSize, OutAtlasWidth, OutAtlasHeight, PreparedPlane.BackAtlasUVs);
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
			const FPlaneCoverSettings& Settings,
			int32& OutAtlasWidth,
			int32& OutAtlasHeight,
			int32& OutLargestInteriorDimension,
			int32& OutLargestPadding)
		{
			if (PlaneInfos.IsEmpty())
			{
				return false;
			}

			const int32 RequestedPadding = FMath::Clamp(Settings.TextureTilePaddingPixels, 0, 128);
			const int32 AtlasResolution = FMath::Clamp(Settings.TextureAtlasResolution, 256, 8192);

			double MaxPlaneDimension = 1.0;
			for (const FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
			{
				const double AtlasResolutionScale = PlaneInfo.bIsTrunkCard
					? FMath::Max(1.0, Settings.TrunkCardAtlasScale)
					: 1.0;
				MaxPlaneDimension = FMath::Max(
					MaxPlaneDimension,
					FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, PlaneInfo.MaxV - PlaneInfo.MinV) * AtlasResolutionScale);
			}

			auto BuildPackRects = [&](const double PixelsPerUnit, TArray<FAtlasPackRect>& OutPackRects)
			{
				OutPackRects.Reset();
				for (int32 PlaneIndex = 0; PlaneIndex < PlaneInfos.Num(); ++PlaneIndex)
				{
					FPlaneProxyPlaneInfo& PlaneInfo = PlaneInfos[PlaneIndex];
					const double AtlasResolutionScale = PlaneInfo.bIsTrunkCard
						? FMath::Max(1.0, Settings.TrunkCardAtlasScale)
						: 1.0;
					const double PlaneWidth = FMath::Max(PlaneInfo.MaxU - PlaneInfo.MinU, 1.0) * AtlasResolutionScale;
					const double PlaneHeight = FMath::Max(PlaneInfo.MaxV - PlaneInfo.MinV, 1.0) * AtlasResolutionScale;
					const int32 InteriorWidth = FMath::Max(1, FMath::CeilToInt(PlaneWidth * PixelsPerUnit));
					const int32 InteriorHeight = FMath::Max(1, FMath::CeilToInt(PlaneHeight * PixelsPerUnit));
					const int32 EffectivePadding = FMath::Clamp(RequestedPadding, 0, FMath::Max(0, (FMath::Min(InteriorWidth, InteriorHeight) - 1) / 2));

					FAtlasPackRect FrontRect;
					FrontRect.PlaneIndex = PlaneIndex;
					FrontRect.bBackFace = false;
					FrontRect.InteriorSize = FIntPoint(InteriorWidth, InteriorHeight);
					FrontRect.Padding = EffectivePadding;
					FrontRect.PitchSize = FIntPoint(InteriorWidth + EffectivePadding * 2, InteriorHeight + EffectivePadding * 2);
					OutPackRects.Add(FrontRect);

					if (PlaneInfo.bHasBackFaceAtlas)
					{
						FAtlasPackRect BackRect = FrontRect;
						BackRect.bBackFace = true;
						OutPackRects.Add(BackRect);
					}
				}
			};

			TArray<FAtlasPackRect> CandidatePackRects;
			TArray<FAtlasPackRect> BestPackRects;
			TArray<FIntPoint> CandidateInteriorMins;
			TArray<FIntPoint> BestInteriorMins;
			double LowScale = 0.0;
			double HighScale = static_cast<double>(AtlasResolution) / MaxPlaneDimension;
			bool bFoundPack = false;
			for (int32 Iteration = 0; Iteration < 28; ++Iteration)
			{
				const double MidScale = 0.5 * (LowScale + HighScale);
				BuildPackRects(MidScale, CandidatePackRects);
				if (TryPackAtlasRects(CandidatePackRects, AtlasResolution, CandidateInteriorMins))
				{
					LowScale = MidScale;
					BestPackRects = CandidatePackRects;
					BestInteriorMins = CandidateInteriorMins;
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
				if (!TryPackAtlasRects(CandidatePackRects, AtlasResolution, CandidateInteriorMins))
				{
					return false;
				}
				BestPackRects = CandidatePackRects;
				BestInteriorMins = CandidateInteriorMins;
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
				PlaneInfo.AtlasTilePaddingPixels = FMath::Max(PlaneInfo.AtlasTilePaddingPixels, PackRect.Padding);
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
				SetAtlasUVsFromTile(PlaneInfo.AtlasPixelMin, PlaneInfo.AtlasTileSize, OutAtlasWidth, OutAtlasHeight, PlaneInfo.AtlasUVs);
				if (PlaneInfo.bHasBackFaceAtlas)
				{
					SetAtlasUVsFromTile(PlaneInfo.BackAtlasPixelMin, PlaneInfo.BackAtlasTileSize, OutAtlasWidth, OutAtlasHeight, PlaneInfo.BackAtlasUVs);
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

		bool IsPointInsidePreparedPlaneEnvelope(const FPreparedProxyPlane& Plane, const FVector& Point, const double Tolerance)
		{
			const double U = FVector::DotProduct(Point, Plane.AxisU);
			const double V = FVector::DotProduct(Point, Plane.AxisV);
			const double SignedDistance = FVector::DotProduct(Plane.OrientedNormal, Point) - Plane.OrientedRho;
			return U >= Plane.EnvelopeMinU - Tolerance
				&& U <= Plane.EnvelopeMaxU + Tolerance
				&& V >= Plane.EnvelopeMinV - Tolerance
				&& V <= Plane.EnvelopeMaxV + Tolerance
				&& SignedDistance >= Plane.EnvelopeMinSignedDistance - Tolerance
				&& SignedDistance <= Plane.EnvelopeMaxSignedDistance + Tolerance;
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

		void ApplyKMeansEnvelopeCrackReduction(
			const TArray<FSourceTriangle>& Triangles,
			const FPlaneCoverSettings& Settings,
			TArray<FPreparedProxyPlane>& PreparedPlanes)
		{
			if (Settings.Technique != EPlaneCoverTechnique::KMeansClustering
				|| Settings.KMeansCrackReductionMode == EKMeansCrackReductionMode::Off
				|| PreparedPlanes.Num() <= 1)
			{
				return;
			}

			const double ProjectionScale = FMath::Clamp(Settings.KMeansCrackReductionProjectionScale, 0.0, 1.0);
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
			const FVector Corners[4],
			const FVector2f CornerUVs[4],
			const FVector2f BackCornerUVs[4],
			const FVector2f MaskUV,
			const FVector& InShadingNormal)
		{
			static constexpr int32 UnrealFrontFaceOrder[4] = { 0, 3, 2, 1 };

			const FVector TangentU = Corners[1] - Corners[0];
			const FVector TangentV = Corners[3] - Corners[0];
			const FVector GeometryTangent = TangentU.GetSafeNormal();
			const FVector IntendedFaceNormal = FVector::CrossProduct(TangentU, Corners[2] - Corners[0]).GetSafeNormal();
			const FVector DesiredBinormal = TangentV.GetSafeNormal();
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
				const int32 CornerIndex = UnrealFrontFaceOrder[VertexOrderIndex];
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

		void AddIntervalToRhoBins(
			TArray<FRhoBinAccumulator>& RhoBins,
			const int32 DirectionIndex,
			const int32 RhoBinCount,
			const double GlobalRhoMin,
			const double RhoBinWidth,
			const double IntervalMin,
			const double IntervalMax,
			const double DensityContribution)
		{
			if (DensityContribution == 0.0 || IntervalMax < IntervalMin)
			{
				return;
			}

			const int32 FirstBin = FMath::Clamp(FMath::FloorToInt((IntervalMin - GlobalRhoMin) / RhoBinWidth), 0, RhoBinCount - 1);
			const int32 LastBin = FMath::Clamp(FMath::FloorToInt((IntervalMax - GlobalRhoMin) / RhoBinWidth), 0, RhoBinCount - 1);

			for (int32 RhoBinIndex = FirstBin; RhoBinIndex <= LastBin; ++RhoBinIndex)
			{
				const double BinMin = GlobalRhoMin + static_cast<double>(RhoBinIndex) * RhoBinWidth;
				const double BinMax = BinMin + RhoBinWidth;
				if (!IntervalsTouchOrOverlap(IntervalMin, IntervalMax, BinMin, BinMax))
				{
					continue;
				}

				const double OverlapMin = FMath::Max(IntervalMin, BinMin);
				const double OverlapMax = FMath::Min(IntervalMax, BinMax);
				const double Overlap = FMath::Max(0.0, OverlapMax - OverlapMin);
				const double Weight = DensityContribution * (Overlap / RhoBinWidth);
				FRhoBinAccumulator& Bin = RhoBins[DirectionIndex * RhoBinCount + RhoBinIndex];
				Bin.Density += Weight;
			}
		}

		TArray<FVector> BuildSampledNormals(const FPlaneCoverSettings& Settings)
		{
			const int32 ThetaSteps = FMath::Max(4, Settings.NormalThetaSteps);
			const int32 PhiSteps = FMath::Max(3, Settings.NormalPhiSteps);
			TArray<FVector> Normals;
			Normals.Reserve(ThetaSteps * PhiSteps);

			for (int32 PhiIndex = 0; PhiIndex < PhiSteps; ++PhiIndex)
			{
				const double Phi = UE_DOUBLE_PI * (static_cast<double>(PhiIndex) + 0.5) / static_cast<double>(PhiSteps);
				for (int32 ThetaIndex = 0; ThetaIndex < ThetaSteps; ++ThetaIndex)
				{
					const double Theta = UE_TWO_PI * (static_cast<double>(ThetaIndex) + 0.5) / static_cast<double>(ThetaSteps);
					Normals.Add(NormalFromPlaneSpace(Theta, Phi));
				}
			}

			return Normals;
		}

		void ComputeProjectionRange(const TArray<FSourceTriangle>& Triangles, const TBitArray<>& CoveredTriangles, const TArray<FVector>& Normals, double& OutRhoMin, double& OutRhoMax);

		void InitializeDensityGridBins(
			FPlaneSpaceDensityGrid& Grid,
			const TArray<FSourceTriangle>& Triangles,
			const FPlaneCoverSettings& Settings)
		{
			const TArray<FVector> Normals = BuildSampledNormals(Settings);
			Grid.ThetaSteps = FMath::Max(4, Settings.NormalThetaSteps);
			Grid.PhiSteps = FMath::Max(3, Settings.NormalPhiSteps);
			Grid.RhoBinCount = FMath::Max(8, Settings.RhoBinCount);
			Grid.ThetaHalfExtent = 0.5 * UE_TWO_PI / static_cast<double>(Grid.ThetaSteps);
			Grid.PhiHalfExtent = 0.5 * UE_DOUBLE_PI / static_cast<double>(Grid.PhiSteps);

			TBitArray<> UncoveredTriangles;
			UncoveredTriangles.Init(false, Triangles.Num());
			double GlobalRhoMax = 0.0;
			ComputeProjectionRange(Triangles, UncoveredTriangles, Normals, Grid.GlobalRhoMin, GlobalRhoMax);
			Grid.GlobalRhoMin -= Settings.ErrorTolerance + RhoRangePadding;
			GlobalRhoMax += Settings.ErrorTolerance + RhoRangePadding;
			Grid.RhoBinWidth = FMath::Max((GlobalRhoMax - Grid.GlobalRhoMin) / static_cast<double>(Grid.RhoBinCount), KINDA_SMALL_NUMBER);

			Grid.Bins.SetNumZeroed(Grid.ThetaSteps * Grid.PhiSteps * Grid.RhoBinCount);
			for (int32 PhiIndex = 0; PhiIndex < Grid.PhiSteps; ++PhiIndex)
			{
				const double PhiCenter = UE_DOUBLE_PI * (static_cast<double>(PhiIndex) + 0.5) / static_cast<double>(Grid.PhiSteps);
				for (int32 ThetaIndex = 0; ThetaIndex < Grid.ThetaSteps; ++ThetaIndex)
				{
					const double ThetaCenter = UE_TWO_PI * (static_cast<double>(ThetaIndex) + 0.5) / static_cast<double>(Grid.ThetaSteps);
					const int32 DirectionIndex = PhiIndex * Grid.ThetaSteps + ThetaIndex;
					const FVector Normal = NormalFromPlaneSpace(ThetaCenter, PhiCenter);
					for (int32 RhoBinIndex = 0; RhoBinIndex < Grid.RhoBinCount; ++RhoBinIndex)
					{
						FRhoBinAccumulator& Bin = Grid.Bins[DirectionIndex * Grid.RhoBinCount + RhoBinIndex];
						Bin.Normal = Normal;
						Bin.ThetaCenter = ThetaCenter;
						Bin.PhiCenter = PhiCenter;
						Bin.ThetaHalfExtent = Grid.ThetaHalfExtent;
						Bin.PhiHalfExtent = Grid.PhiHalfExtent;
						Bin.RhoMin = Grid.GlobalRhoMin + static_cast<double>(RhoBinIndex) * Grid.RhoBinWidth;
						Bin.RhoMax = Bin.RhoMin + Grid.RhoBinWidth;
					}
				}
			}
		}

		void ComputeProjectionRange(const TArray<FSourceTriangle>& Triangles, const TBitArray<>& CoveredTriangles, const TArray<FVector>& Normals, double& OutRhoMin, double& OutRhoMax)
		{
			TArray<double> MinByNormal;
			TArray<double> MaxByNormal;
			MinByNormal.Init(TNumericLimits<double>::Max(), Normals.Num());
			MaxByNormal.Init(-TNumericLimits<double>::Max(), Normals.Num());

			ParallelFor(Normals.Num(), [&Triangles, &CoveredTriangles, &Normals, &MinByNormal, &MaxByNormal](const int32 NormalIndex)
			{
				const FVector& Normal = Normals[NormalIndex];
				double LocalMin = TNumericLimits<double>::Max();
				double LocalMax = -TNumericLimits<double>::Max();
				for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
				{
					if (CoveredTriangles[TriangleIndex])
					{
						continue;
					}

					const FSourceTriangle& Triangle = Triangles[TriangleIndex];
					for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
					{
						const double Rho = FVector::DotProduct(Normal, Triangle.Vertices[VertexIndex]);
						LocalMin = FMath::Min(LocalMin, Rho);
						LocalMax = FMath::Max(LocalMax, Rho);
					}
				}

				MinByNormal[NormalIndex] = LocalMin;
				MaxByNormal[NormalIndex] = LocalMax;
			});

			OutRhoMin = TNumericLimits<double>::Max();
			OutRhoMax = -TNumericLimits<double>::Max();
			for (int32 NormalIndex = 0; NormalIndex < Normals.Num(); ++NormalIndex)
			{
				OutRhoMin = FMath::Min(OutRhoMin, MinByNormal[NormalIndex]);
				OutRhoMax = FMath::Max(OutRhoMax, MaxByNormal[NormalIndex]);
			}

			if (!FMath::IsFinite(OutRhoMin) || !FMath::IsFinite(OutRhoMax) || OutRhoMax <= OutRhoMin)
			{
				OutRhoMin = -1.0;
				OutRhoMax = 1.0;
			}
		}

		void AccumulateTriangleDensityForDirection(
			FPlaneSpaceDensityGrid& Grid,
			const FSourceTriangle& Triangle,
			const FPlaneCoverSettings& Settings,
			const int32 DirectionIndex,
			const FAngularBinContext& AngularBinContext,
			const FVector& Normal,
			const double Sign)
		{
			double ValidMin = 0.0;
			double ValidMax = 0.0;
			if (!ComputeAngularBinValidityRhoRangeForContext(
				Triangle,
				AngularBinContext,
				Settings,
				ValidMin,
				ValidMax))
			{
				return;
			}

			const double Contribution = ComputeContribution(Triangle, Normal);
			AddIntervalToRhoBins(Grid.Bins, DirectionIndex, Grid.RhoBinCount, Grid.GlobalRhoMin, Grid.RhoBinWidth, ValidMin, ValidMax, Sign * Contribution);
			AddIntervalToRhoBins(Grid.Bins, DirectionIndex, Grid.RhoBinCount, Grid.GlobalRhoMin, Grid.RhoBinWidth, ValidMin - Settings.ErrorTolerance, ValidMin, Sign * -Contribution * PaperPenaltyWeight);
		}

		void InitializeDensityGrid(
			FPlaneSpaceDensityGrid& Grid,
			const TArray<FSourceTriangle>& Triangles,
			const FPlaneCoverSettings& Settings)
		{
			InitializeDensityGridBins(Grid, Triangles, Settings);

			const int32 DirectionCount = Grid.ThetaSteps * Grid.PhiSteps;
			ParallelFor(DirectionCount, [&Grid, &Triangles, &Settings](const int32 DirectionIndex)
			{
				const int32 PhiIndex = DirectionIndex / Grid.ThetaSteps;
				const int32 ThetaIndex = DirectionIndex % Grid.ThetaSteps;
				const double PhiCenter = UE_DOUBLE_PI * (static_cast<double>(PhiIndex) + 0.5) / static_cast<double>(Grid.PhiSteps);
				const double ThetaCenter = UE_TWO_PI * (static_cast<double>(ThetaIndex) + 0.5) / static_cast<double>(Grid.ThetaSteps);
				const FVector Normal = NormalFromPlaneSpace(ThetaCenter, PhiCenter);
				const FAngularBinContext AngularBinContext = MakeAngularBinContext(ThetaCenter, Grid.ThetaHalfExtent, PhiCenter, Grid.PhiHalfExtent);
				for (const FSourceTriangle& Triangle : Triangles)
				{
					AccumulateTriangleDensityForDirection(Grid, Triangle, Settings, DirectionIndex, AngularBinContext, Normal, 1.0);
				}
			});
		}

		void RemoveTrianglesFromDensityGrid(
			FPlaneSpaceDensityGrid& Grid,
			const TArray<FSourceTriangle>& Triangles,
			const TArray<int32>& TriangleIndices,
			const FPlaneCoverSettings& Settings)
		{
			if (TriangleIndices.IsEmpty())
			{
				return;
			}

			const int32 DirectionCount = Grid.ThetaSteps * Grid.PhiSteps;
			ParallelFor(DirectionCount, [&Grid, &Triangles, &TriangleIndices, &Settings](const int32 DirectionIndex)
			{
				const int32 PhiIndex = DirectionIndex / Grid.ThetaSteps;
				const int32 ThetaIndex = DirectionIndex % Grid.ThetaSteps;
				const double PhiCenter = UE_DOUBLE_PI * (static_cast<double>(PhiIndex) + 0.5) / static_cast<double>(Grid.PhiSteps);
				const double ThetaCenter = UE_TWO_PI * (static_cast<double>(ThetaIndex) + 0.5) / static_cast<double>(Grid.ThetaSteps);
				const FVector Normal = NormalFromPlaneSpace(ThetaCenter, PhiCenter);
				const FAngularBinContext AngularBinContext = MakeAngularBinContext(ThetaCenter, Grid.ThetaHalfExtent, PhiCenter, Grid.PhiHalfExtent);
				for (const int32 TriangleIndex : TriangleIndices)
				{
					if (Triangles.IsValidIndex(TriangleIndex))
					{
						AccumulateTriangleDensityForDirection(Grid, Triangles[TriangleIndex], Settings, DirectionIndex, AngularBinContext, Normal, -1.0);
					}
				}
			});
		}

		FVector GetCellNormal(const FPaperRefinementCell& Cell)
		{
			return NormalFromPlaneSpace(Cell.ThetaCenter, Cell.PhiCenter);
		}

		void CollectSimpleValidTriangleIndices(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const double ThetaCenter,
			const double ThetaHalfExtent,
			const double PhiCenter,
			const double PhiHalfExtent,
			const double RhoMin,
			const double RhoMax,
			const FPlaneCoverSettings& Settings,
			TArray<int32>& OutTriangleIndices)
		{
			OutTriangleIndices.Reset();

			TArray<uint8> ValidFlags;
			ValidFlags.SetNumZeroed(Triangles.Num());
			const FAngularBinContext AngularBinContext = MakeAngularBinContext(ThetaCenter, ThetaHalfExtent, PhiCenter, PhiHalfExtent);

			ParallelFor(Triangles.Num(), [&Triangles, &CoveredTriangles, &AngularBinContext, RhoMin, RhoMax, &Settings, &ValidFlags](const int32 TriangleIndex)
			{
				if (CoveredTriangles[TriangleIndex])
				{
					return;
				}

				double ValidMin = 0.0;
				double ValidMax = 0.0;
				if (ComputeAngularBinValidityRhoRangeForContext(Triangles[TriangleIndex], AngularBinContext, Settings, ValidMin, ValidMax)
					&& IntervalsTouchOrOverlap(ValidMin, ValidMax, RhoMin, RhoMax))
				{
					ValidFlags[TriangleIndex] = 1;
				}
			});

			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				if (ValidFlags[TriangleIndex] != 0)
				{
					OutTriangleIndices.Add(TriangleIndex);
				}
			}
		}

		FPaperCellEvaluation EvaluatePaperRefinementCell(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FPaperRefinementCell& Cell,
			const TArray<int32>& CandidateTriangleIndices,
			const FPlaneCoverSettings& Settings,
			const bool bCollectSimpleValidTriangleIndices)
		{
			FPaperCellEvaluation Evaluation;
			Evaluation.Normal = GetCellNormal(Cell);
			Evaluation.Rho = Cell.RhoCenter;
			Evaluation.bCenterValidForAllSimpleTriangles = true;
			if (bCollectSimpleValidTriangleIndices)
			{
				Evaluation.SimpleValidTriangleIndices.Reserve(CandidateTriangleIndices.Num());
			}

			const double CellRhoMin = Cell.RhoCenter - Cell.RhoHalfExtent;
			const double CellRhoMax = Cell.RhoCenter + Cell.RhoHalfExtent;
			const double CellRhoWidth = FMath::Max(CellRhoMax - CellRhoMin, KINDA_SMALL_NUMBER);
			const FAngularBinContext AngularBinContext = MakeAngularBinContext(Cell.ThetaCenter, Cell.ThetaHalfExtent, Cell.PhiCenter, Cell.PhiHalfExtent);

			for (const int32 TriangleIndex : CandidateTriangleIndices)
			{
				if (CoveredTriangles[TriangleIndex])
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				double SimpleValidMin = 0.0;
				double SimpleValidMax = 0.0;
				if (!ComputeAngularBinValidityRhoRangeForContext(
					Triangle,
					AngularBinContext,
					Settings,
					SimpleValidMin,
					SimpleValidMax))
				{
					continue;
				}

				const bool bValidOverlap = IntervalsTouchOrOverlap(SimpleValidMin, SimpleValidMax, CellRhoMin, CellRhoMax);
				const bool bPenaltyOverlap = IntervalsTouchOrOverlap(SimpleValidMin - Settings.ErrorTolerance, SimpleValidMin, CellRhoMin, CellRhoMax);
				if (!bValidOverlap && !bPenaltyOverlap)
				{
					continue;
				}

				const double Contribution = ComputeContribution(Triangle, Evaluation.Normal);
				if (bValidOverlap)
				{
					const double ValidOverlapMin = FMath::Max(SimpleValidMin, CellRhoMin);
					const double ValidOverlapMax = FMath::Min(SimpleValidMax, CellRhoMax);
					const double ValidOverlap = FMath::Max(0.0, ValidOverlapMax - ValidOverlapMin);
					++Evaluation.SimpleValidTriangleCount;
					if (bCollectSimpleValidTriangleIndices)
					{
						Evaluation.SimpleValidTriangleIndices.Add(TriangleIndex);
					}
					Evaluation.Density += Contribution * (ValidOverlap / CellRhoWidth);

					double CenterValidMin = 0.0;
					double CenterValidMax = 0.0;
					if (ComputeValidityInterval(Triangle, Evaluation.Normal, Settings, CenterValidMin, CenterValidMax)
						&& Evaluation.Rho >= CenterValidMin
						&& Evaluation.Rho <= CenterValidMax)
					{
						Evaluation.ValidContribution += Contribution;
						Evaluation.CoveredArea += Triangle.Area;
					}
					else
					{
						Evaluation.bCenterValidForAllSimpleTriangles = false;
					}
				}

				if (bPenaltyOverlap)
				{
					const double PenaltyOverlapMin = FMath::Max(SimpleValidMin - Settings.ErrorTolerance, CellRhoMin);
					const double PenaltyOverlapMax = FMath::Min(SimpleValidMin, CellRhoMax);
					const double PenaltyOverlap = FMath::Max(0.0, PenaltyOverlapMax - PenaltyOverlapMin);
					Evaluation.Density -= PaperPenaltyWeight * Contribution * (PenaltyOverlap / CellRhoWidth);
				}
			}

			if (Evaluation.SimpleValidTriangleCount == 0)
			{
				Evaluation.bCenterValidForAllSimpleTriangles = false;
			}

			return Evaluation;
		}

		FPaperCellScore EvaluatePaperRefinementCellScoreOnly(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FPaperRefinementCell& Cell,
			const TArray<int32>& CandidateTriangleIndices,
			const FPlaneCoverSettings& Settings)
		{
			FPaperCellScore Score;
			const FVector CellNormal = GetCellNormal(Cell);
			const double CellRhoMin = Cell.RhoCenter - Cell.RhoHalfExtent;
			const double CellRhoMax = Cell.RhoCenter + Cell.RhoHalfExtent;
			const double CellRhoWidth = FMath::Max(CellRhoMax - CellRhoMin, KINDA_SMALL_NUMBER);
			const FAngularBinContext AngularBinContext = MakeAngularBinContext(Cell.ThetaCenter, Cell.ThetaHalfExtent, Cell.PhiCenter, Cell.PhiHalfExtent);

			for (const int32 TriangleIndex : CandidateTriangleIndices)
			{
				if (CoveredTriangles[TriangleIndex])
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				double SimpleValidMin = 0.0;
				double SimpleValidMax = 0.0;
				if (!ComputeAngularBinValidityRhoRangeForContext(
					Triangle,
					AngularBinContext,
					Settings,
					SimpleValidMin,
					SimpleValidMax))
				{
					continue;
				}

				const bool bValidOverlap = IntervalsTouchOrOverlap(SimpleValidMin, SimpleValidMax, CellRhoMin, CellRhoMax);
				const bool bPenaltyOverlap = IntervalsTouchOrOverlap(SimpleValidMin - Settings.ErrorTolerance, SimpleValidMin, CellRhoMin, CellRhoMax);
				if (!bValidOverlap && !bPenaltyOverlap)
				{
					continue;
				}

				const double Contribution = ComputeContribution(Triangle, CellNormal);
				if (bValidOverlap)
				{
					const double ValidOverlapMin = FMath::Max(SimpleValidMin, CellRhoMin);
					const double ValidOverlapMax = FMath::Min(SimpleValidMax, CellRhoMax);
					const double ValidOverlap = FMath::Max(0.0, ValidOverlapMax - ValidOverlapMin);
					++Score.SimpleValidTriangleCount;
					Score.Density += Contribution * (ValidOverlap / CellRhoWidth);
				}

				if (bPenaltyOverlap)
				{
					const double PenaltyOverlapMin = FMath::Max(SimpleValidMin - Settings.ErrorTolerance, CellRhoMin);
					const double PenaltyOverlapMax = FMath::Min(SimpleValidMin, CellRhoMax);
					const double PenaltyOverlap = FMath::Max(0.0, PenaltyOverlapMax - PenaltyOverlapMin);
					Score.Density -= PaperPenaltyWeight * Contribution * (PenaltyOverlap / CellRhoWidth);
				}
			}

			return Score;
		}

		FPaperCellEvaluation RefinePaperCellRecursive(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FPaperRefinementCell& Cell,
			const TArray<int32>& CandidateTriangleIndices,
			const FPlaneCoverSettings& Settings)
		{
			FPaperRefinementCell CurrentCell = Cell;
			FPaperCellEvaluation Evaluation = EvaluatePaperRefinementCell(Triangles, CoveredTriangles, CurrentCell, CandidateTriangleIndices, Settings, true);

			while (Evaluation.SimpleValidTriangleCount > 0
				&& !Evaluation.bCenterValidForAllSimpleTriangles
				&& CurrentCell.Depth < Settings.AdaptiveRefinementDepth)
			{
				TArray<FPaperRefinementCell> ChildCells;
				ChildCells.Reserve(27 * 8);
				for (int32 ThetaNeighbor = -1; ThetaNeighbor <= 1; ++ThetaNeighbor)
				{
					for (int32 PhiNeighbor = -1; PhiNeighbor <= 1; ++PhiNeighbor)
					{
						for (int32 RhoNeighbor = -1; RhoNeighbor <= 1; ++RhoNeighbor)
						{
							const double NeighborThetaCenter = WrapTheta(CurrentCell.ThetaCenter + static_cast<double>(ThetaNeighbor) * 2.0 * CurrentCell.ThetaHalfExtent);
							const double NeighborPhiCenter = CurrentCell.PhiCenter + static_cast<double>(PhiNeighbor) * 2.0 * CurrentCell.PhiHalfExtent;
							const double NeighborRhoCenter = CurrentCell.RhoCenter + static_cast<double>(RhoNeighbor) * 2.0 * CurrentCell.RhoHalfExtent;

							for (int32 ThetaChild = 0; ThetaChild < 2; ++ThetaChild)
							{
								for (int32 PhiChild = 0; PhiChild < 2; ++PhiChild)
								{
									for (int32 RhoChild = 0; RhoChild < 2; ++RhoChild)
									{
										FPaperRefinementCell ChildCell;
										ChildCell.Depth = CurrentCell.Depth + 1;
										ChildCell.ThetaHalfExtent = CurrentCell.ThetaHalfExtent * 0.5;
										ChildCell.PhiHalfExtent = CurrentCell.PhiHalfExtent * 0.5;
										ChildCell.RhoHalfExtent = CurrentCell.RhoHalfExtent * 0.5;
										ChildCell.ThetaCenter = WrapTheta(NeighborThetaCenter + (ThetaChild == 0 ? -ChildCell.ThetaHalfExtent : ChildCell.ThetaHalfExtent));
										ChildCell.PhiCenter = NeighborPhiCenter + (PhiChild == 0 ? -ChildCell.PhiHalfExtent : ChildCell.PhiHalfExtent);
										ChildCell.RhoCenter = NeighborRhoCenter + (RhoChild == 0 ? -ChildCell.RhoHalfExtent : ChildCell.RhoHalfExtent);
										if (ChildCell.PhiCenter <= 0.0 || ChildCell.PhiCenter >= UE_DOUBLE_PI)
										{
											continue;
										}

										ChildCells.Add(MoveTemp(ChildCell));
									}
								}
							}
						}
					}
				}

				if (ChildCells.IsEmpty())
				{
					return Evaluation;
				}

				const TArray<int32> ParentSimpleValidTriangleIndices = MoveTemp(Evaluation.SimpleValidTriangleIndices);
				TArray<FPaperCellScore> ChildScores;
				ChildScores.SetNum(ChildCells.Num());
				ParallelFor(ChildCells.Num(), [&Triangles, &CoveredTriangles, &ChildCells, &ChildScores, &ParentSimpleValidTriangleIndices, &Settings](const int32 ChildIndex)
				{
					ChildScores[ChildIndex] = EvaluatePaperRefinementCellScoreOnly(Triangles, CoveredTriangles, ChildCells[ChildIndex], ParentSimpleValidTriangleIndices, Settings);
				});

				int32 BestSubCellIndex = INDEX_NONE;
				double BestSubCellDensity = -TNumericLimits<double>::Max();
				for (int32 ChildIndex = 0; ChildIndex < ChildScores.Num(); ++ChildIndex)
				{
					const FPaperCellScore& ChildScore = ChildScores[ChildIndex];
					if (ChildScore.SimpleValidTriangleCount > 0 && ChildScore.Density > BestSubCellDensity)
					{
						BestSubCellIndex = ChildIndex;
						BestSubCellDensity = ChildScore.Density;
					}
				}

				if (BestSubCellIndex == INDEX_NONE)
				{
					return Evaluation;
				}

				CurrentCell = ChildCells[BestSubCellIndex];
				Evaluation = EvaluatePaperRefinementCell(Triangles, CoveredTriangles, CurrentCell, ParentSimpleValidTriangleIndices, Settings, true);
			}

			return Evaluation;
		}

		FCandidatePlane RefinePaperDensityBin(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FRhoBinAccumulator& Bin,
			const FPlaneCoverSettings& Settings)
		{
			TArray<int32> RootTriangleIndices;
			CollectSimpleValidTriangleIndices(
				Triangles,
				CoveredTriangles,
				Bin.ThetaCenter,
				Bin.ThetaHalfExtent,
				Bin.PhiCenter,
				Bin.PhiHalfExtent,
				Bin.RhoMin,
				Bin.RhoMax,
				Settings,
				RootTriangleIndices);

			if (RootTriangleIndices.IsEmpty())
			{
				FCandidatePlane Candidate;
				Candidate.Normal = Bin.Normal;
				Candidate.Rho = 0.5 * (Bin.RhoMin + Bin.RhoMax);
				Candidate.EstimatedDensity = Bin.Density;
				return Candidate;
			}

			FPaperRefinementCell RootCell;
			RootCell.ThetaCenter = Bin.ThetaCenter;
			RootCell.PhiCenter = Bin.PhiCenter;
			RootCell.ThetaHalfExtent = Bin.ThetaHalfExtent;
			RootCell.PhiHalfExtent = Bin.PhiHalfExtent;
			RootCell.RhoHalfExtent = 0.5 * (Bin.RhoMax - Bin.RhoMin);
			RootCell.RhoCenter = 0.5 * (Bin.RhoMin + Bin.RhoMax);
			const FPaperCellEvaluation BestEvaluation = RefinePaperCellRecursive(Triangles, CoveredTriangles, RootCell, RootTriangleIndices, Settings);

			FCandidatePlane Candidate;
			Candidate.Normal = BestEvaluation.Normal.GetSafeNormal();
			Candidate.Rho = BestEvaluation.Rho;
			Candidate.EstimatedDensity = BestEvaluation.Density;
			if (Candidate.Normal.IsNearlyZero())
			{
				Candidate.Normal = Bin.Normal;
				Candidate.Rho = 0.5 * (Bin.RhoMin + Bin.RhoMax);
				Candidate.EstimatedDensity = Bin.Density;
			}

			return Candidate;
		}

		bool CandidateCoversAnyTriangle(
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FCandidatePlane& CandidatePlane,
			const FPlaneCoverSettings& Settings)
		{
			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				if (CoveredTriangles[TriangleIndex])
				{
					continue;
				}

				if (IsTriangleValidForFinalPlane(Triangles[TriangleIndex], CandidatePlane, Settings))
				{
					return true;
				}
			}

			return false;
		}

		TArray<FCandidatePlane> BuildCandidatePlanesFromGrid(
			const FPlaneSpaceDensityGrid& Grid,
			const TArray<FSourceTriangle>& Triangles,
			const TBitArray<>& CoveredTriangles,
			const FPlaneCoverSettings& Settings)
		{
			TArray<FCandidatePlane> CandidatePlanes;
			if (Triangles.IsEmpty())
			{
				return CandidatePlanes;
			}

			TBitArray<> TriedBins;
			TriedBins.Init(false, Grid.Bins.Num());
			CandidatePlanes.Reserve(1);
			for (int32 TriedDensityBins = 0; TriedDensityBins < Grid.Bins.Num(); ++TriedDensityBins)
			{
				int32 BestBinIndex = INDEX_NONE;
				double BestDensity = 0.0;
				for (int32 BinIndex = 0; BinIndex < Grid.Bins.Num(); ++BinIndex)
				{
					if (TriedBins[BinIndex] || Grid.Bins[BinIndex].Density <= 0.0)
					{
						continue;
					}

					if (Grid.Bins[BinIndex].Density > BestDensity)
					{
						BestDensity = Grid.Bins[BinIndex].Density;
						BestBinIndex = BinIndex;
					}
				}

				if (BestBinIndex == INDEX_NONE || BestDensity <= 0.0)
				{
					break;
				}

				TriedBins[BestBinIndex] = true;
				const FRhoBinAccumulator& Bin = Grid.Bins[BestBinIndex];
				FCandidatePlane Candidate = RefinePaperDensityBin(Triangles, CoveredTriangles, Bin, Settings);
				if (Candidate.EstimatedDensity > 0.0 && CandidateCoversAnyTriangle(Triangles, CoveredTriangles, Candidate, Settings))
				{
					CandidatePlanes.Add(Candidate);
					break;
				}
			}

			return CandidatePlanes;
		}

		double ComputeIrregularityFromBounds(const FBox2D& Bounds)
		{
			if (!Bounds.bIsValid)
			{
				return 0.0;
			}

			const FVector2D Size = Bounds.GetSize();
			const double Area = FMath::Max(Size.X * Size.Y, 1.0e-6);
			const double Perimeter = 2.0 * (FMath::Max(Size.X, 0.0) + FMath::Max(Size.Y, 0.0));
			return (Perimeter * Perimeter) / Area;
		}

		double CompactnessAdjustedScore(const double Contribution, const FBox2D& Bounds, const FPlaneCoverSettings& Settings)
		{
			if (Contribution <= 0.0 || Settings.TextureCompactnessWeight <= 0.0)
			{
				return Contribution;
			}

			const double Irregularity = ComputeIrregularityFromBounds(Bounds);
			const double NormalizedExcess = FMath::Max(0.0, Irregularity / 16.0 - 1.0);
			return Contribution / (1.0 + Settings.TextureCompactnessWeight * NormalizedExcess);
		}

		void ApplyTextureCompactness(
			const TArray<FSourceTriangle>& Triangles,
			const FPlaneCoverSettings& Settings,
			FPreparedCandidate& Candidate)
		{
			Candidate.SelectionScore = Candidate.ValidContribution;
			if (Settings.TextureCompactnessWeight <= 0.0 || Candidate.ValidTriangleIndices.Num() <= 1)
			{
				return;
			}

			FVector AxisU = FVector::RightVector;
			FVector AxisV = FVector::UpVector;
			BuildPlaneFrame(Candidate.Plane.Normal, AxisU, AxisV);

			struct FProjectedTriangle
			{
				int32 TriangleIndex = INDEX_NONE;
				FVector2D Centroid = FVector2D::ZeroVector;
				double Contribution = 0.0;
				double Area = 0.0;
			};

			TArray<FProjectedTriangle> ProjectedTriangles;
			ProjectedTriangles.Reserve(Candidate.ValidTriangleIndices.Num());

			FBox2D FullBounds(ForceInit);
			for (const int32 TriangleIndex : Candidate.ValidTriangleIndices)
			{
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}

				const FSourceTriangle& Triangle = Triangles[TriangleIndex];
				const FVector Centroid3D = (Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
				FProjectedTriangle& ProjectedTriangle = ProjectedTriangles.AddDefaulted_GetRef();
				ProjectedTriangle.TriangleIndex = TriangleIndex;
				ProjectedTriangle.Centroid = FVector2D(
					FVector::DotProduct(Centroid3D, AxisU),
					FVector::DotProduct(Centroid3D, AxisV));
				ProjectedTriangle.Contribution = ComputeContribution(Triangle, Candidate.Plane.Normal);
				ProjectedTriangle.Area = Triangle.Area;
				FullBounds += ProjectedTriangle.Centroid;
			}

			Candidate.SelectionScore = CompactnessAdjustedScore(Candidate.ValidContribution, FullBounds, Settings);
			if (ProjectedTriangles.Num() <= 1 || !FullBounds.bIsValid)
			{
				return;
			}

			const FVector2D FullSize = FullBounds.GetSize();
			const double BucketSize = FMath::Max3(
				FMath::Max(FullSize.X, FullSize.Y) / 16.0,
				Settings.ErrorTolerance * 2.0,
				1.0);

			TMap<FIntPoint, TArray<int32>> BucketTriangleIndices;
			for (int32 ProjectedIndex = 0; ProjectedIndex < ProjectedTriangles.Num(); ++ProjectedIndex)
			{
				const FVector2D Relative = ProjectedTriangles[ProjectedIndex].Centroid - FullBounds.Min;
				const FIntPoint Bucket(
					FMath::FloorToInt(Relative.X / BucketSize),
					FMath::FloorToInt(Relative.Y / BucketSize));
				BucketTriangleIndices.FindOrAdd(Bucket).Add(ProjectedIndex);
			}

			TSet<FIntPoint> VisitedBuckets;
			double BestClusterScore = -TNumericLimits<double>::Max();
			double BestClusterContribution = 0.0;
			double BestClusterArea = 0.0;
			TArray<int32> BestClusterTriangleIndices;

			for (const TPair<FIntPoint, TArray<int32>>& BucketPair : BucketTriangleIndices)
			{
				if (VisitedBuckets.Contains(BucketPair.Key))
				{
					continue;
				}

				TArray<FIntPoint> Stack;
				Stack.Add(BucketPair.Key);
				VisitedBuckets.Add(BucketPair.Key);

				FBox2D ClusterBounds(ForceInit);
				double ClusterContribution = 0.0;
				double ClusterArea = 0.0;
				TArray<int32> ClusterTriangleIndices;

				while (!Stack.IsEmpty())
				{
					const FIntPoint Bucket = Stack.Pop(EAllowShrinking::No);
					if (const TArray<int32>* BucketProjectedIndices = BucketTriangleIndices.Find(Bucket))
					{
						for (const int32 ProjectedIndex : *BucketProjectedIndices)
						{
							const FProjectedTriangle& ProjectedTriangle = ProjectedTriangles[ProjectedIndex];
							ClusterBounds += ProjectedTriangle.Centroid;
							ClusterContribution += ProjectedTriangle.Contribution;
							ClusterArea += ProjectedTriangle.Area;
							ClusterTriangleIndices.Add(ProjectedTriangle.TriangleIndex);
						}
					}

					for (int32 NeighborY = -1; NeighborY <= 1; ++NeighborY)
					{
						for (int32 NeighborX = -1; NeighborX <= 1; ++NeighborX)
						{
							if (NeighborX == 0 && NeighborY == 0)
							{
								continue;
							}

							const FIntPoint NeighborBucket(Bucket.X + NeighborX, Bucket.Y + NeighborY);
							if (!VisitedBuckets.Contains(NeighborBucket) && BucketTriangleIndices.Contains(NeighborBucket))
							{
								VisitedBuckets.Add(NeighborBucket);
								Stack.Add(NeighborBucket);
							}
						}
					}
				}

				const double ClusterScore = CompactnessAdjustedScore(ClusterContribution, ClusterBounds, Settings);
				if (ClusterScore > BestClusterScore)
				{
					BestClusterScore = ClusterScore;
					BestClusterContribution = ClusterContribution;
					BestClusterArea = ClusterArea;
					BestClusterTriangleIndices = MoveTemp(ClusterTriangleIndices);
				}
			}

			if (!BestClusterTriangleIndices.IsEmpty() && BestClusterScore > Candidate.SelectionScore)
			{
				Candidate.ValidTriangleIndices = MoveTemp(BestClusterTriangleIndices);
				Candidate.ValidContribution = BestClusterContribution;
				Candidate.CoveredArea = BestClusterArea;
				Candidate.SelectionScore = BestClusterScore;
			}
		}

		TArray<FPreparedCandidate> PrepareCandidates(const TArray<FSourceTriangle>& Triangles, const TBitArray<>& CoveredTriangles, const TArray<FCandidatePlane>& CandidatePlanes, const FPlaneCoverSettings& Settings)
		{
			TArray<FPreparedCandidate> PreparedCandidates;
			PreparedCandidates.Reserve(CandidatePlanes.Num());

			for (const FCandidatePlane& CandidatePlane : CandidatePlanes)
			{
				FPreparedCandidate PreparedCandidate;
				PreparedCandidate.Plane = CandidatePlane;

				TArray<uint8> ValidFlags;
				ValidFlags.SetNumZeroed(Triangles.Num());
				TArray<double> Contributions;
				Contributions.SetNumZeroed(Triangles.Num());

				ParallelFor(Triangles.Num(), [&Triangles, &CoveredTriangles, &CandidatePlane, &Settings, &ValidFlags, &Contributions](const int32 TriangleIndex)
				{
					if (CoveredTriangles[TriangleIndex])
					{
						return;
					}

					const FSourceTriangle& Triangle = Triangles[TriangleIndex];
					if (!IsTriangleValidForFinalPlane(Triangle, CandidatePlane, Settings))
					{
						return;
					}

					const double Contribution = ComputeContribution(Triangle, CandidatePlane.Normal);
					ValidFlags[TriangleIndex] = 1;
					Contributions[TriangleIndex] = Contribution;
				});

				for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
				{
					if (ValidFlags[TriangleIndex] != 0)
					{
						PreparedCandidate.ValidTriangleIndices.Add(TriangleIndex);
						PreparedCandidate.ValidContribution += Contributions[TriangleIndex];
						PreparedCandidate.CoveredArea += Triangles[TriangleIndex].Area;
					}
				}

				if (!PreparedCandidate.ValidTriangleIndices.IsEmpty())
				{
					ApplyTextureCompactness(Triangles, Settings, PreparedCandidate);
					PreparedCandidates.Add(MoveTemp(PreparedCandidate));
				}
			}

			return PreparedCandidates;
		}

		bool ExtractTrianglesFromRenderData(const UStaticMesh* StaticMesh, const int32 LODIndex, TArray<FSourceTriangle>& OutTriangles)
		{
			OutTriangles.Reset();
			if (!StaticMesh || !StaticMesh->GetRenderData() || !StaticMesh->GetRenderData()->LODResources.IsValidIndex(LODIndex))
			{
				return false;
			}

			const FStaticMeshLODResources& LODResources = StaticMesh->GetRenderData()->LODResources[LODIndex];
			const FPositionVertexBuffer& PositionBuffer = LODResources.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& StaticMeshVertexBuffer = LODResources.VertexBuffers.StaticMeshVertexBuffer;
			const FIndexArrayView Indices = LODResources.IndexBuffer.GetArrayView();
			if (PositionBuffer.GetNumVertices() == 0 || StaticMeshVertexBuffer.GetNumVertices() == 0 || Indices.Num() < 3)
			{
				return false;
			}

			const int32 SourceUVChannelCount = StaticMeshVertexBuffer.GetNumTexCoords();
			const bool bHasUVs = SourceUVChannelCount > 0;
			const int32 StoredUVChannelCount = bHasUVs
				? FMath::Min(SourceUVChannelCount, UE::BillboardClouds::MaxMaterialBakeUVChannels)
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
							// Derive a fallback tangent perpendicular to the normal
							const FVector ReferenceAxis = FMath::Abs(SourceNormal.Z) < 0.95 ? FVector::UpVector : FVector::ForwardVector;
							SourceTangent = FVector::CrossProduct(ReferenceAxis, SourceNormal).GetSafeNormal();
							if (SourceTangent.IsNearlyZero())
							{
								SourceTangent = FVector::ForwardVector;
							}
						}
						Triangle.VertexTangents[VertexIndex] = SourceTangent;

						// Derive binormal sign from render TangentY
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
					OutTriangles.Add(Triangle);
				}
			}

			return !OutTriangles.IsEmpty();
		}
	}

	bool ExtractTrianglesFromStaticMesh(const UStaticMesh* StaticMesh, int32 LODIndex, TArray<FSourceTriangle>& OutTriangles, FString& OutError)
	{
		OutTriangles.Reset();
		OutError.Reset();

		if (!StaticMesh)
		{
			OutError = TEXT("StaticMesh is null.");
			return false;
		}

		if (ExtractTrianglesFromRenderData(StaticMesh, LODIndex, OutTriangles))
		{
			return true;
		}

		const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(LODIndex);
		if (!MeshDescription)
		{
			OutError = FString::Printf(TEXT("LOD %d has no MeshDescription source data."), LODIndex);
			return false;
		}

		const TVertexAttributesConstRef<FVector3f> VertexPositions = MeshDescription->GetVertexPositions();
		const FStaticMeshConstAttributes MeshAttributes(*MeshDescription);
		const bool bHasVertexInstanceNormals = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Normal);
		const bool bHasVertexInstanceTangents = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Tangent);
		const bool bHasVertexInstanceBinormalSigns = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::BinormalSign);
		const bool bHasVertexInstanceUVs = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::TextureCoordinate);
		const bool bHasPolygonGroupMaterialSlots = MeshDescription->PolygonGroupAttributes().HasAttribute(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
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
		TPolygonGroupAttributesConstRef<FName> PolygonGroupMaterialSlotNames;
		if (bHasPolygonGroupMaterialSlots)
		{
			PolygonGroupMaterialSlotNames = MeshAttributes.GetPolygonGroupMaterialSlotNames();
		}
		OutTriangles.Reserve(MeshDescription->Triangles().Num());

		for (const FTriangleID TriangleID : MeshDescription->Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> TriangleVertexIDs = MeshDescription->GetTriangleVertices(TriangleID);
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
				const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs = MeshDescription->GetTriangleVertexInstances(TriangleID);
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
				const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs = MeshDescription->GetTriangleVertexInstances(TriangleID);
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
				const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs = MeshDescription->GetTriangleVertexInstances(TriangleID);
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					const int32 StoredUVChannelCount = FMath::Min(VertexInstanceUVs.GetNumChannels(), UE::BillboardClouds::MaxMaterialBakeUVChannels);
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
			if (bHasPolygonGroupMaterialSlots)
			{
				const FPolygonGroupID PolygonGroupID = MeshDescription->GetTrianglePolygonGroup(TriangleID);
				if (MeshDescription->IsPolygonGroupValid(PolygonGroupID))
				{
					const FName MaterialSlotName = PolygonGroupMaterialSlotNames[PolygonGroupID];
					int32 MaterialIndex = StaticMesh->GetMaterialIndex(MaterialSlotName);
					if (MaterialIndex == INDEX_NONE)
					{
						MaterialIndex = StaticMesh->GetMaterialIndexFromImportedMaterialSlotName(MaterialSlotName);
					}
					Triangle.MaterialIndex = MaterialIndex;
				}
			}
			Triangle.Area = 0.5 * DoubleArea;
			OutTriangles.Add(Triangle);
		}

		if (OutTriangles.IsEmpty())
		{
			OutError = TEXT("No non-degenerate triangles found in LOD 0 source data.");
			return false;
		}

		return true;
	}

	FVector ProjectPointToPlane(const FVector& Point, const FVector& PlaneNormal, const double PlaneRho)
	{
		const double SignedDistance = FVector::DotProduct(PlaneNormal, Point) - PlaneRho;
		return Point - PlaneNormal * SignedDistance;
	}

	bool IsPointWithinPlaneError(const FVector& Point, const FVector& PlaneNormal, const double PlaneRho, const FPlaneCoverSettings& Settings)
	{
		return FMath::Abs(FVector::DotProduct(PlaneNormal, Point) - PlaneRho) <= Settings.ErrorTolerance;
	}

	bool IsTriangleInsideGodOfWarThickSlice(const FSourceTriangle& Triangle, const FVector& PlaneNormal, const double PlaneRho, const FPlaneCoverSettings& Settings)
	{
		for (const FVector& Vertex : Triangle.Vertices)
		{
			if (!IsPointWithinPlaneError(Vertex, PlaneNormal, PlaneRho, Settings))
			{
				return false;
			}
		}
		return true;
	}

	bool IsTriangleValidForPlane(const FSourceTriangle& Triangle, const FVector& PlaneNormal, const double PlaneRho, const FPlaneCoverSettings& Settings)
	{
		double ValidMin = 0.0;
		double ValidMax = 0.0;
		return ComputeValidityInterval(Triangle, PlaneNormal, Settings, ValidMin, ValidMax)
			&& PlaneRho >= ValidMin
			&& PlaneRho <= ValidMax;
	}

	bool DoesTriangleIntersectPlaneValidZone(const FSourceTriangle& Triangle, const FVector& PlaneNormal, const double PlaneRho, const FPlaneCoverSettings& Settings)
	{
		if (IsTriangleValidForPlane(Triangle, PlaneNormal, PlaneRho, Settings))
		{
			return true;
		}

		double MinDistance = TNumericLimits<double>::Max();
		double MaxDistance = -TNumericLimits<double>::Max();
		for (const FVector& Vertex : Triangle.Vertices)
		{
			const double SignedDistance = FVector::DotProduct(PlaneNormal, Vertex) - PlaneRho;
			MinDistance = FMath::Min(MinDistance, SignedDistance);
			MaxDistance = FMath::Max(MaxDistance, SignedDistance);
		}
		return MinDistance <= Settings.ErrorTolerance && MaxDistance >= -Settings.ErrorTolerance;
	}

	struct FKMeansCluster
	{
		FVector Normal = FVector::UpVector;
		double Rho = 0.0;
		TArray<int32> TriangleIndices;
	};

	FVector ComputeTriangleCentroid(const FSourceTriangle& Triangle)
	{
		return (Triangle.Vertices[0] + Triangle.Vertices[1] + Triangle.Vertices[2]) / 3.0;
	}

	double ComputeTriangleToPlaneDistance(const FSourceTriangle& Triangle, const FVector& PlaneNormal, const double PlaneRho)
	{
		const FVector SafeNormal = PlaneNormal.GetSafeNormal();
		return FMath::Abs(FVector::DotProduct(SafeNormal, Triangle.Vertices[0]) - PlaneRho)
			+ FMath::Abs(FVector::DotProduct(SafeNormal, Triangle.Vertices[1]) - PlaneRho)
			+ FMath::Abs(FVector::DotProduct(SafeNormal, Triangle.Vertices[2]) - PlaneRho);
	}

	FVector ComputeSmallestSymmetricEigenVector(double Matrix[3][3])
	{
		double EigenVectors[3][3] =
		{
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 }
		};

		for (int32 Iteration = 0; Iteration < 32; ++Iteration)
		{
			int32 P = 0;
			int32 Q = 1;
			double MaxOffDiagonal = FMath::Abs(Matrix[0][1]);
			if (FMath::Abs(Matrix[0][2]) > MaxOffDiagonal)
			{
				P = 0;
				Q = 2;
				MaxOffDiagonal = FMath::Abs(Matrix[0][2]);
			}
			if (FMath::Abs(Matrix[1][2]) > MaxOffDiagonal)
			{
				P = 1;
				Q = 2;
				MaxOffDiagonal = FMath::Abs(Matrix[1][2]);
			}

			if (MaxOffDiagonal <= 1.0e-12)
			{
				break;
			}

			const double App = Matrix[P][P];
			const double Aqq = Matrix[Q][Q];
			const double Apq = Matrix[P][Q];
			const double Angle = 0.5 * FMath::Atan2(2.0 * Apq, Aqq - App);
			const double C = FMath::Cos(Angle);
			const double S = FMath::Sin(Angle);

			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (Index == P || Index == Q)
				{
					continue;
				}

				const double Aip = Matrix[Index][P];
				const double Aiq = Matrix[Index][Q];
				Matrix[Index][P] = C * Aip - S * Aiq;
				Matrix[P][Index] = Matrix[Index][P];
				Matrix[Index][Q] = S * Aip + C * Aiq;
				Matrix[Q][Index] = Matrix[Index][Q];
			}

			Matrix[P][P] = C * C * App - 2.0 * S * C * Apq + S * S * Aqq;
			Matrix[Q][Q] = S * S * App + 2.0 * S * C * Apq + C * C * Aqq;
			Matrix[P][Q] = 0.0;
			Matrix[Q][P] = 0.0;

			for (int32 Row = 0; Row < 3; ++Row)
			{
				const double Vip = EigenVectors[Row][P];
				const double Viq = EigenVectors[Row][Q];
				EigenVectors[Row][P] = C * Vip - S * Viq;
				EigenVectors[Row][Q] = S * Vip + C * Viq;
			}
		}

		int32 SmallestIndex = 0;
		if (Matrix[1][1] < Matrix[SmallestIndex][SmallestIndex])
		{
			SmallestIndex = 1;
		}
		if (Matrix[2][2] < Matrix[SmallestIndex][SmallestIndex])
		{
			SmallestIndex = 2;
		}

		return FVector(EigenVectors[0][SmallestIndex], EigenVectors[1][SmallestIndex], EigenVectors[2][SmallestIndex]).GetSafeNormal();
	}

	bool FitBestPlaneToTriangles(const TArray<FSourceTriangle>& Triangles, const TArray<int32>& TriangleIndices, FVector& OutNormal, double& OutRho)
	{
		if (TriangleIndices.IsEmpty())
		{
			return false;
		}

		FVector Centroid = FVector::ZeroVector;
		int32 VertexCount = 0;
		FVector WeightedTriangleNormal = FVector::ZeroVector;
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Centroid += Vertex;
				++VertexCount;
			}
			WeightedTriangleNormal += Triangle.Area * Triangle.Normal;
		}

		if (VertexCount <= 0)
		{
			return false;
		}

		Centroid /= static_cast<double>(VertexCount);

		double Covariance[3][3] =
		{
			{ 0.0, 0.0, 0.0 },
			{ 0.0, 0.0, 0.0 },
			{ 0.0, 0.0, 0.0 }
		};

		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			for (const FVector& Vertex : Triangle.Vertices)
			{
				const FVector Delta = Vertex - Centroid;
				Covariance[0][0] += Delta.X * Delta.X;
				Covariance[0][1] += Delta.X * Delta.Y;
				Covariance[0][2] += Delta.X * Delta.Z;
				Covariance[1][1] += Delta.Y * Delta.Y;
				Covariance[1][2] += Delta.Y * Delta.Z;
				Covariance[2][2] += Delta.Z * Delta.Z;
			}
		}

		Covariance[1][0] = Covariance[0][1];
		Covariance[2][0] = Covariance[0][2];
		Covariance[2][1] = Covariance[1][2];

		FVector Normal = ComputeSmallestSymmetricEigenVector(Covariance);
		if (Normal.IsNearlyZero())
		{
			Normal = WeightedTriangleNormal.GetSafeNormal();
		}
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}

		const FVector PreferredNormal = WeightedTriangleNormal.GetSafeNormal();
		if (!PreferredNormal.IsNearlyZero() && FVector::DotProduct(Normal, PreferredNormal) < 0.0)
		{
			Normal *= -1.0;
		}

		OutNormal = Normal;
		OutRho = FVector::DotProduct(OutNormal, Centroid);
		return true;
	}

	void GenerateKMeansSphereDirections(const int32 DirectionCount, TArray<FVector>& OutDirections)
	{
		OutDirections.Reset(DirectionCount);
		if (DirectionCount <= 0)
		{
			return;
		}

		const double GoldenAngle = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 DirectionIndex = 0; DirectionIndex < DirectionCount; ++DirectionIndex)
		{
			const double T = (static_cast<double>(DirectionIndex) + 0.5) / static_cast<double>(DirectionCount);
			const double Z = 1.0 - 2.0 * T;
			const double Radius = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
			const double Theta = GoldenAngle * static_cast<double>(DirectionIndex);
			OutDirections.Add(FVector(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z).GetSafeNormal());
		}

		if (DirectionCount > 512)
		{
			return;
		}

		for (int32 RelaxationIteration = 0; RelaxationIteration < 12; ++RelaxationIteration)
		{
			TArray<FVector> Forces;
			Forces.Init(FVector::ZeroVector, DirectionCount);
			for (int32 A = 0; A < DirectionCount; ++A)
			{
				for (int32 B = A + 1; B < DirectionCount; ++B)
				{
					const FVector Delta = OutDirections[A] - OutDirections[B];
					const double DistanceSquared = FMath::Max(Delta.SizeSquared(), 1.0e-6);
					const FVector Force = Delta / (DistanceSquared * FMath::Sqrt(DistanceSquared));
					Forces[A] += Force;
					Forces[B] -= Force;
				}
			}

			for (int32 DirectionIndex = 0; DirectionIndex < DirectionCount; ++DirectionIndex)
			{
				const FVector Normal = OutDirections[DirectionIndex];
				const FVector TangentForce = Forces[DirectionIndex] - Normal * FVector::DotProduct(Forces[DirectionIndex], Normal);
				OutDirections[DirectionIndex] = (Normal + TangentForce * 0.01).GetSafeNormal();
				if (OutDirections[DirectionIndex].IsNearlyZero())
				{
					OutDirections[DirectionIndex] = Normal;
				}
			}
		}
	}

	void InitializeKMeansClusters(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings, TArray<FKMeansCluster>& OutClusters)
	{
		const int32 ClusterCount = FMath::Clamp(Settings.KMeansPlaneCount, 1, FMath::Max(1, Triangles.Num()));
		OutClusters.Reset(ClusterCount);

		FBox Bounds(ForceInit);
		for (const FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Bounds += Vertex;
			}
		}

		const FVector BoundsCenter = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
		const double BoundsRadius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Length(), 1.0) : 1.0;

		TArray<FVector> Directions;
		GenerateKMeansSphereDirections(ClusterCount, Directions);
		for (int32 ClusterIndex = 0; ClusterIndex < ClusterCount; ++ClusterIndex)
		{
			const FVector Normal = Directions.IsValidIndex(ClusterIndex) ? Directions[ClusterIndex] : FVector::UpVector;
			FKMeansCluster& Cluster = OutClusters.AddDefaulted_GetRef();
			Cluster.Normal = Normal.GetSafeNormal();
			Cluster.Rho = FVector::DotProduct(Cluster.Normal, BoundsCenter + Cluster.Normal * BoundsRadius);
		}
	}

	void AssignTrianglesToKMeansClusters(const TArray<FSourceTriangle>& Triangles, TArray<FKMeansCluster>& Clusters)
	{
		for (FKMeansCluster& Cluster : Clusters)
		{
			Cluster.TriangleIndices.Reset();
		}

		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			int32 BestClusterIndex = INDEX_NONE;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				const FKMeansCluster& Cluster = Clusters[ClusterIndex];
				const double Distance = ComputeTriangleToPlaneDistance(Triangles[TriangleIndex], Cluster.Normal, Cluster.Rho);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BestClusterIndex = ClusterIndex;
				}
			}

			if (Clusters.IsValidIndex(BestClusterIndex))
			{
				Clusters[BestClusterIndex].TriangleIndices.Add(TriangleIndex);
			}
		}
	}

	void FitKMeansClusters(const TArray<FSourceTriangle>& Triangles, TArray<FKMeansCluster>& Clusters)
	{
		for (FKMeansCluster& Cluster : Clusters)
		{
			FVector FittedNormal = Cluster.Normal;
			double FittedRho = Cluster.Rho;
			if (FitBestPlaneToTriangles(Triangles, Cluster.TriangleIndices, FittedNormal, FittedRho))
			{
				Cluster.Normal = FittedNormal;
				Cluster.Rho = FittedRho;
			}
		}
	}

	double ComputeKMeansClusterDistance(const TArray<FSourceTriangle>& Triangles, const FKMeansCluster& Cluster)
	{
		double TotalDistance = 0.0;
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (Triangles.IsValidIndex(TriangleIndex))
			{
				TotalDistance += ComputeTriangleToPlaneDistance(Triangles[TriangleIndex], Cluster.Normal, Cluster.Rho);
			}
		}
		return TotalDistance;
	}

	double ComputeKMeansClusterCentroidDistance(const TArray<FSourceTriangle>& Triangles, const FKMeansCluster& Cluster)
	{
		double TotalDistance = 0.0;
		const FVector SafeNormal = Cluster.Normal.GetSafeNormal();
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (Triangles.IsValidIndex(TriangleIndex))
			{
				TotalDistance += FMath::Abs(FVector::DotProduct(SafeNormal, ComputeTriangleCentroid(Triangles[TriangleIndex])) - Cluster.Rho);
			}
		}
		return TotalDistance;
	}

	void ComputeKMeansClusterCentroidDistances(const TArray<FSourceTriangle>& Triangles, const TArray<FKMeansCluster>& Clusters, TArray<double>& OutDistances)
	{
		OutDistances.Reset(Clusters.Num());
		for (const FKMeansCluster& Cluster : Clusters)
		{
			OutDistances.Add(ComputeKMeansClusterCentroidDistance(Triangles, Cluster));
		}
	}

	FVector ComputeClusterCoverageCentroid(const TArray<FSourceTriangle>& Triangles, const FKMeansCluster& Cluster)
	{
		if (Cluster.TriangleIndices.IsEmpty())
		{
			return Cluster.Normal.GetSafeNormal() * Cluster.Rho;
		}

		const FVector SafeNormal = Cluster.Normal.GetSafeNormal();
		FVector ProjectedVertexCentroid = FVector::ZeroVector;
		int32 ProjectedVertexCount = 0;
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			for (const FVector& Vertex : Triangle.Vertices)
			{
				ProjectedVertexCentroid += ProjectPointToPlane(Vertex, SafeNormal, Cluster.Rho);
				++ProjectedVertexCount;
			}
		}

		if (ProjectedVertexCount <= 0)
		{
			return FVector::ZeroVector;
		}

		ProjectedVertexCentroid /= static_cast<double>(ProjectedVertexCount);

		FVector BestTriangleCentroid = ProjectedVertexCentroid;
		double BestDistanceSquared = TNumericLimits<double>::Max();
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const FVector ProjectedTriangleCentroid =
				(ProjectPointToPlane(Triangle.Vertices[0], SafeNormal, Cluster.Rho)
					+ ProjectPointToPlane(Triangle.Vertices[1], SafeNormal, Cluster.Rho)
					+ ProjectPointToPlane(Triangle.Vertices[2], SafeNormal, Cluster.Rho)) / 3.0;
			const double DistanceSquared = FVector::DistSquared(ProjectedTriangleCentroid, ProjectedVertexCentroid);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestTriangleCentroid = ComputeTriangleCentroid(Triangle);
			}
		}

		return BestTriangleCentroid;
	}

	double ComputeTriangleToCoverageCentroidDistance(const FSourceTriangle& Triangle, const FVector& Centroid)
	{
		return FVector::Distance(Triangle.Vertices[0], Centroid)
			+ FVector::Distance(Triangle.Vertices[1], Centroid)
			+ FVector::Distance(Triangle.Vertices[2], Centroid);
	}

	void ComputeCoverageCentroids(const TArray<FSourceTriangle>& Triangles, const TArray<FKMeansCluster>& Clusters, TArray<FVector>& OutCentroids)
	{
		OutCentroids.Reset(Clusters.Num());
		for (const FKMeansCluster& Cluster : Clusters)
		{
			OutCentroids.Add(ComputeClusterCoverageCentroid(Triangles, Cluster));
		}
	}

	void AssignTrianglesToCoverageCentroids(const TArray<FSourceTriangle>& Triangles, const TArray<FVector>& Centroids, TArray<FKMeansCluster>& Clusters)
	{
		for (FKMeansCluster& Cluster : Clusters)
		{
			Cluster.TriangleIndices.Reset();
		}

		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			int32 BestClusterIndex = INDEX_NONE;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				if (!Centroids.IsValidIndex(ClusterIndex))
				{
					continue;
				}

				const double Distance = ComputeTriangleToCoverageCentroidDistance(Triangles[TriangleIndex], Centroids[ClusterIndex]);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BestClusterIndex = ClusterIndex;
				}
			}

			if (Clusters.IsValidIndex(BestClusterIndex))
			{
				Clusters[BestClusterIndex].TriangleIndices.Add(TriangleIndex);
			}
		}
	}

	void ApplyKMeansCoverageCentroidRelaxation(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings&, TArray<FKMeansCluster>& Clusters)
	{
		if (Clusters.Num() <= 1)
		{
			return;
		}

		TArray<FVector> Centroids;
		ComputeCoverageCentroids(Triangles, Clusters, Centroids);
		AssignTrianglesToCoverageCentroids(Triangles, Centroids, Clusters);
	}

	double ComputeClusterCoverageRadius(const TArray<FSourceTriangle>& Triangles, const FKMeansCluster& Cluster, const FVector& Centroid)
	{
		double Radius = 0.0;
		for (const int32 TriangleIndex : Cluster.TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex))
			{
				continue;
			}

			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Radius = FMath::Max(Radius, FVector::Distance(Vertex, Centroid));
			}
		}
		return Radius;
	}

	double ComputeClusterRadiusVariance(const TArray<FSourceTriangle>& Triangles, const TArray<FKMeansCluster>& Clusters, TArray<FVector>* OutCentroids = nullptr, TArray<double>* OutRadii = nullptr)
	{
		TArray<FVector> Centroids;
		ComputeCoverageCentroids(Triangles, Clusters, Centroids);

		TArray<double> Radii;
		Radii.Reset(Clusters.Num());
		double MeanRadius = 0.0;
		int32 ValidClusterCount = 0;
		for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
		{
			const double Radius = Clusters[ClusterIndex].TriangleIndices.IsEmpty()
				? 0.0
				: ComputeClusterCoverageRadius(Triangles, Clusters[ClusterIndex], Centroids[ClusterIndex]);
			Radii.Add(Radius);
			if (!Clusters[ClusterIndex].TriangleIndices.IsEmpty())
			{
				MeanRadius += Radius;
				++ValidClusterCount;
			}
		}

		if (ValidClusterCount <= 0)
		{
			if (OutCentroids)
			{
				*OutCentroids = MoveTemp(Centroids);
			}
			if (OutRadii)
			{
				*OutRadii = MoveTemp(Radii);
			}
			return 0.0;
		}

		MeanRadius /= static_cast<double>(ValidClusterCount);
		double Variance = 0.0;
		for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
		{
			if (Clusters[ClusterIndex].TriangleIndices.IsEmpty())
			{
				continue;
			}

			const double Delta = Radii[ClusterIndex] - MeanRadius;
			Variance += Delta * Delta;
		}
		Variance /= static_cast<double>(ValidClusterCount);

		if (OutCentroids)
		{
			*OutCentroids = MoveTemp(Centroids);
		}
		if (OutRadii)
		{
			*OutRadii = MoveTemp(Radii);
		}
		return Variance;
	}

	void RedistributeTrianglesToNearestClusters(const TArray<FSourceTriangle>& Triangles, const TArray<int32>& TriangleIndices, TArray<FKMeansCluster>& Clusters)
	{
		for (const int32 TriangleIndex : TriangleIndices)
		{
			if (!Triangles.IsValidIndex(TriangleIndex) || Clusters.IsEmpty())
			{
				continue;
			}

			int32 BestClusterIndex = INDEX_NONE;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				const FKMeansCluster& Cluster = Clusters[ClusterIndex];
				const double Distance = ComputeTriangleToPlaneDistance(Triangles[TriangleIndex], Cluster.Normal, Cluster.Rho);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BestClusterIndex = ClusterIndex;
				}
			}

			if (Clusters.IsValidIndex(BestClusterIndex))
			{
				Clusters[BestClusterIndex].TriangleIndices.Add(TriangleIndex);
			}
		}
	}

	bool MakeClusterPlaneFromTriangle(const FSourceTriangle& Triangle, FKMeansCluster& OutCluster)
	{
		FVector TriangleNormal = Triangle.Normal.GetSafeNormal();
		if (TriangleNormal.IsNearlyZero())
		{
			TriangleNormal = FVector::CrossProduct(Triangle.Vertices[1] - Triangle.Vertices[0], Triangle.Vertices[2] - Triangle.Vertices[0]).GetSafeNormal();
		}
		if (TriangleNormal.IsNearlyZero())
		{
			return false;
		}

		OutCluster.Normal = TriangleNormal;
		OutCluster.Rho = FVector::DotProduct(TriangleNormal, ComputeTriangleCentroid(Triangle));
		OutCluster.TriangleIndices.Reset();
		return true;
	}

	void ApplyKMeansMinimumCoverageClusterReplacement(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings, TArray<FKMeansCluster>& Clusters)
	{
		if (Clusters.Num() <= 2)
		{
			return;
		}

		TArray<double> PreviousRadii;
		PreviousRadii.Init(TNumericLimits<double>::Max(), Clusters.Num());
		bool bFirstLoop = true;
		const int32 MaxIterations = FMath::Clamp(Settings.KMeansMaxIterations, 1, 512);

		for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
		{
			int32 SmallestClusterIndex = INDEX_NONE;
			int32 SmallestTriangleCount = TNumericLimits<int32>::Max();
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				if (Clusters[ClusterIndex].TriangleIndices.Num() < SmallestTriangleCount)
				{
					SmallestTriangleCount = Clusters[ClusterIndex].TriangleIndices.Num();
					SmallestClusterIndex = ClusterIndex;
				}
			}

			if (!Clusters.IsValidIndex(SmallestClusterIndex))
			{
				break;
			}

			const TArray<int32> RedistributeTriangleIndices = Clusters[SmallestClusterIndex].TriangleIndices;
			Clusters.RemoveAt(SmallestClusterIndex, 1, EAllowShrinking::No);
			RedistributeTrianglesToNearestClusters(Triangles, RedistributeTriangleIndices, Clusters);
			FitKMeansClusters(Triangles, Clusters);

			int32 LargestClusterIndex = INDEX_NONE;
			int32 LargestTriangleCount = -1;
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				if (Clusters[ClusterIndex].TriangleIndices.Num() > LargestTriangleCount)
				{
					LargestTriangleCount = Clusters[ClusterIndex].TriangleIndices.Num();
					LargestClusterIndex = ClusterIndex;
				}
			}

			if (!Clusters.IsValidIndex(LargestClusterIndex) || Clusters[LargestClusterIndex].TriangleIndices.IsEmpty())
			{
				break;
			}

			int32 FarthestTriangleArrayIndex = INDEX_NONE;
			double FarthestDistance = -TNumericLimits<double>::Max();
			const FVector LargestCentroid = ComputeClusterCoverageCentroid(Triangles, Clusters[LargestClusterIndex]);
			for (int32 TriangleArrayIndex = 0; TriangleArrayIndex < Clusters[LargestClusterIndex].TriangleIndices.Num(); ++TriangleArrayIndex)
			{
				const int32 TriangleIndex = Clusters[LargestClusterIndex].TriangleIndices[TriangleArrayIndex];
				if (!Triangles.IsValidIndex(TriangleIndex))
				{
					continue;
				}

				const double Distance = FVector::Distance(ComputeTriangleCentroid(Triangles[TriangleIndex]), LargestCentroid);
				if (Distance > FarthestDistance)
				{
					FarthestDistance = Distance;
					FarthestTriangleArrayIndex = TriangleArrayIndex;
				}
			}

			if (!Clusters[LargestClusterIndex].TriangleIndices.IsValidIndex(FarthestTriangleArrayIndex))
			{
				break;
			}

			const int32 SeedTriangleIndex = Clusters[LargestClusterIndex].TriangleIndices[FarthestTriangleArrayIndex];
			FKMeansCluster NewCluster;
			if (!Triangles.IsValidIndex(SeedTriangleIndex) || !MakeClusterPlaneFromTriangle(Triangles[SeedTriangleIndex], NewCluster))
			{
				break;
			}

			NewCluster.TriangleIndices.Add(SeedTriangleIndex);
			Clusters[LargestClusterIndex].TriangleIndices.RemoveAt(FarthestTriangleArrayIndex, 1, EAllowShrinking::No);
			Clusters.Add(NewCluster);
			FitKMeansClusters(Triangles, Clusters);

			TArray<double> CurrentRadii;
			ComputeClusterRadiusVariance(Triangles, Clusters, nullptr, &CurrentRadii);

			bool bStop = true;
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				if (!PreviousRadii.IsValidIndex(ClusterIndex))
				{
					PreviousRadii.Add(TNumericLimits<double>::Max());
				}
				if (CurrentRadii.IsValidIndex(ClusterIndex) && CurrentRadii[ClusterIndex] > PreviousRadii[ClusterIndex])
				{
					bStop = false;
				}
				if (CurrentRadii.IsValidIndex(ClusterIndex))
				{
					PreviousRadii[ClusterIndex] = CurrentRadii[ClusterIndex];
				}
			}

			if (bFirstLoop)
			{
				bStop = false;
				bFirstLoop = false;
			}

			if (bStop)
			{
				break;
			}
		}
	}

	void InitializeKMeansClustersFromPaper(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings, TArray<FKMeansCluster>& Clusters)
	{
		InitializeKMeansClusters(Triangles, Settings, Clusters);
		AssignTrianglesToKMeansClusters(Triangles, Clusters);
		FitKMeansClusters(Triangles, Clusters);
		ApplyKMeansCoverageCentroidRelaxation(Triangles, Settings, Clusters);
		ApplyKMeansMinimumCoverageClusterReplacement(Triangles, Settings, Clusters);
	}

	FPlaneCoverResult BuildKMeansPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings)
	{
		const double TotalStartSeconds = FPlatformTime::Seconds();
		FPlaneCoverResult Result;
		Result.SourceTriangleCount = Triangles.Num();
		for (const FSourceTriangle& Triangle : Triangles)
		{
			Result.SourceArea += Triangle.Area;
		}

		if (Triangles.IsEmpty())
		{
			Result.TotalSeconds = FPlatformTime::Seconds() - TotalStartSeconds;
			return Result;
		}

		const double InitializationStartSeconds = FPlatformTime::Seconds();
		TArray<FKMeansCluster> Clusters;
		InitializeKMeansClustersFromPaper(Triangles, Settings, Clusters);
		Result.DensityBuildSeconds = FPlatformTime::Seconds() - InitializationStartSeconds;

		TArray<double> PreviousClusterDistances;
		PreviousClusterDistances.Init(TNumericLimits<double>::Max(), Clusters.Num());
		bool bFirstLoop = true;
		int32 IterationCount = 0;
		const int32 MaxIterations = FMath::Clamp(Settings.KMeansMaxIterations, 1, 512);

		for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
		{
			const double AssignmentStartSeconds = FPlatformTime::Seconds();
			AssignTrianglesToKMeansClusters(Triangles, Clusters);
			Result.CandidateSearchSeconds += FPlatformTime::Seconds() - AssignmentStartSeconds;

			const double FitStartSeconds = FPlatformTime::Seconds();
			FitKMeansClusters(Triangles, Clusters);
			Result.CandidatePlaneBuildSeconds += FPlatformTime::Seconds() - FitStartSeconds;

			TArray<double> CurrentClusterDistances;
			ComputeKMeansClusterCentroidDistances(Triangles, Clusters, CurrentClusterDistances);
			++IterationCount;

			bool bStop = true;
			for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
			{
				if (!PreviousClusterDistances.IsValidIndex(ClusterIndex))
				{
					PreviousClusterDistances.Add(TNumericLimits<double>::Max());
				}
				if (CurrentClusterDistances.IsValidIndex(ClusterIndex) && CurrentClusterDistances[ClusterIndex] > PreviousClusterDistances[ClusterIndex])
				{
					bStop = false;
				}
				if (CurrentClusterDistances.IsValidIndex(ClusterIndex))
				{
					PreviousClusterDistances[ClusterIndex] = CurrentClusterDistances[ClusterIndex];
				}
			}

			if (bFirstLoop)
			{
				bStop = false;
				bFirstLoop = false;
			}

			if (bStop)
			{
				break;
			}
		}

		Result.GreedyIterationCount = IterationCount;
		Result.MaxIterationCandidatePlaneCount = Clusters.Num();
		Result.TotalCandidatePlaneCount = Clusters.Num() * IterationCount;
		Result.CandidatePlaneCount = Clusters.Num();
		Result.CoveredTriangleCount = 0;
		Result.CoveredArea = 0.0;

		for (const FKMeansCluster& Cluster : Clusters)
		{
			if (Cluster.TriangleIndices.IsEmpty())
			{
				continue;
			}

			FPlaneCoverPlane& Plane = Result.Planes.AddDefaulted_GetRef();
			Plane.Normal = Cluster.Normal.GetSafeNormal();
			Plane.Rho = Cluster.Rho;
			Plane.Score = -ComputeKMeansClusterDistance(Triangles, Cluster);
			Plane.TriangleIndices = Cluster.TriangleIndices;

			for (const int32 TriangleIndex : Plane.TriangleIndices)
			{
				if (Triangles.IsValidIndex(TriangleIndex))
				{
					Plane.CoveredArea += Triangles[TriangleIndex].Area;
				}
			}

			Result.CoveredTriangleCount += Plane.TriangleIndices.Num();
			Result.CoveredArea += Plane.CoveredArea;
		}

		Result.TotalSeconds = FPlatformTime::Seconds() - TotalStartSeconds;
		return Result;
	}

	FBox ComputeSourceBounds(const TArray<FSourceTriangle>& Triangles)
	{
		FBox Bounds(ForceInit);
		for (const FSourceTriangle& Triangle : Triangles)
		{
			for (const FVector& Vertex : Triangle.Vertices)
			{
				Bounds += Vertex;
			}
		}
		return Bounds;
	}

	int32 AddGeodesicVertex(TArray<FVector>& Vertices, const FVector& Vertex)
	{
		Vertices.Add(Vertex.GetSafeNormal());
		return Vertices.Num() - 1;
	}

	uint64 MakeGeodesicEdgeKey(const int32 A, const int32 B)
	{
		const uint32 MinIndex = static_cast<uint32>(FMath::Min(A, B));
		const uint32 MaxIndex = static_cast<uint32>(FMath::Max(A, B));
		return (static_cast<uint64>(MinIndex) << 32) | static_cast<uint64>(MaxIndex);
	}

	int32 GetOrAddGeodesicMidpoint(TArray<FVector>& Vertices, TMap<uint64, int32>& MidpointCache, const int32 A, const int32 B)
	{
		const uint64 EdgeKey = MakeGeodesicEdgeKey(A, B);
		if (const int32* ExistingIndex = MidpointCache.Find(EdgeKey))
		{
			return *ExistingIndex;
		}

		const int32 MidpointIndex = AddGeodesicVertex(Vertices, (Vertices[A] + Vertices[B]) * 0.5);
		MidpointCache.Add(EdgeKey, MidpointIndex);
		return MidpointIndex;
	}

	FVector CanonicalizeHemisphereDirection(FVector Direction)
	{
		Direction = Direction.GetSafeNormal();
		constexpr double Epsilon = 1.0e-8;
		if (Direction.Z < -Epsilon
			|| (FMath::Abs(Direction.Z) <= Epsilon
				&& (Direction.Y < -Epsilon
					|| (FMath::Abs(Direction.Y) <= Epsilon && Direction.X < -Epsilon))))
		{
			Direction *= -1.0;
		}
		return Direction;
	}

	FIntVector MakeDirectionKey(const FVector& Direction)
	{
		constexpr double QuantizeScale = 1000000.0;
		return FIntVector(
			FMath::RoundToInt(Direction.X * QuantizeScale),
			FMath::RoundToInt(Direction.Y * QuantizeScale),
			FMath::RoundToInt(Direction.Z * QuantizeScale));
	}

	void GenerateGeodesicHemisphereDirections(const int32 Subdivisions, TArray<FVector>& OutDirections)
	{
		TArray<FVector> Vertices;
		TArray<FIntVector> Faces;
		Vertices.Reserve(12);
		Faces.Reserve(20);

		const double T = (1.0 + FMath::Sqrt(5.0)) * 0.5;
		AddGeodesicVertex(Vertices, FVector(-1.0, T, 0.0));
		AddGeodesicVertex(Vertices, FVector(1.0, T, 0.0));
		AddGeodesicVertex(Vertices, FVector(-1.0, -T, 0.0));
		AddGeodesicVertex(Vertices, FVector(1.0, -T, 0.0));
		AddGeodesicVertex(Vertices, FVector(0.0, -1.0, T));
		AddGeodesicVertex(Vertices, FVector(0.0, 1.0, T));
		AddGeodesicVertex(Vertices, FVector(0.0, -1.0, -T));
		AddGeodesicVertex(Vertices, FVector(0.0, 1.0, -T));
		AddGeodesicVertex(Vertices, FVector(T, 0.0, -1.0));
		AddGeodesicVertex(Vertices, FVector(T, 0.0, 1.0));
		AddGeodesicVertex(Vertices, FVector(-T, 0.0, -1.0));
		AddGeodesicVertex(Vertices, FVector(-T, 0.0, 1.0));

		Faces.Add(FIntVector(0, 11, 5));
		Faces.Add(FIntVector(0, 5, 1));
		Faces.Add(FIntVector(0, 1, 7));
		Faces.Add(FIntVector(0, 7, 10));
		Faces.Add(FIntVector(0, 10, 11));
		Faces.Add(FIntVector(1, 5, 9));
		Faces.Add(FIntVector(5, 11, 4));
		Faces.Add(FIntVector(11, 10, 2));
		Faces.Add(FIntVector(10, 7, 6));
		Faces.Add(FIntVector(7, 1, 8));
		Faces.Add(FIntVector(3, 9, 4));
		Faces.Add(FIntVector(3, 4, 2));
		Faces.Add(FIntVector(3, 2, 6));
		Faces.Add(FIntVector(3, 6, 8));
		Faces.Add(FIntVector(3, 8, 9));
		Faces.Add(FIntVector(4, 9, 5));
		Faces.Add(FIntVector(2, 4, 11));
		Faces.Add(FIntVector(6, 2, 10));
		Faces.Add(FIntVector(8, 6, 7));
		Faces.Add(FIntVector(9, 8, 1));

		const int32 SafeSubdivisions = FMath::Clamp(Subdivisions, 0, 5);
		for (int32 SubdivisionIndex = 0; SubdivisionIndex < SafeSubdivisions; ++SubdivisionIndex)
		{
			TMap<uint64, int32> MidpointCache;
			TArray<FIntVector> RefinedFaces;
			RefinedFaces.Reserve(Faces.Num() * 4);
			for (const FIntVector& Face : Faces)
			{
				const int32 A = Face.X;
				const int32 B = Face.Y;
				const int32 C = Face.Z;
				const int32 AB = GetOrAddGeodesicMidpoint(Vertices, MidpointCache, A, B);
				const int32 BC = GetOrAddGeodesicMidpoint(Vertices, MidpointCache, B, C);
				const int32 CA = GetOrAddGeodesicMidpoint(Vertices, MidpointCache, C, A);
				RefinedFaces.Add(FIntVector(A, AB, CA));
				RefinedFaces.Add(FIntVector(B, BC, AB));
				RefinedFaces.Add(FIntVector(C, CA, BC));
				RefinedFaces.Add(FIntVector(AB, BC, CA));
			}
			Faces = MoveTemp(RefinedFaces);
		}

		TSet<FIntVector> DirectionKeys;
		OutDirections.Reset();
		OutDirections.Reserve(Vertices.Num() / 2 + 1);
		for (const FVector& Vertex : Vertices)
		{
			const FVector Direction = CanonicalizeHemisphereDirection(Vertex);
			if (Direction.IsNearlyZero())
			{
				continue;
			}

			const FIntVector DirectionKey = MakeDirectionKey(Direction);
			if (DirectionKeys.Contains(DirectionKey))
			{
				continue;
			}

			DirectionKeys.Add(DirectionKey);
			OutDirections.Add(Direction);
		}

		OutDirections.Sort([](const FVector& A, const FVector& B)
		{
			if (!FMath::IsNearlyEqual(A.Z, B.Z, 1.0e-8))
			{
				return A.Z > B.Z;
			}
			if (!FMath::IsNearlyEqual(A.Y, B.Y, 1.0e-8))
			{
				return A.Y > B.Y;
			}
			return A.X > B.X;
		});
	}

	void ComputeBoundsProjectionRange(const FBox& Bounds, const FVector& Normal, double& OutMinRho, double& OutMaxRho)
	{
		const FVector Center = Bounds.GetCenter();
		const FVector Extent = Bounds.GetExtent();
		OutMinRho = TNumericLimits<double>::Max();
		OutMaxRho = -TNumericLimits<double>::Max();

		for (int32 ZSign = -1; ZSign <= 1; ZSign += 2)
		{
			for (int32 YSign = -1; YSign <= 1; YSign += 2)
			{
				for (int32 XSign = -1; XSign <= 1; XSign += 2)
				{
					const FVector Corner = Center + FVector(
						static_cast<double>(XSign) * Extent.X,
						static_cast<double>(YSign) * Extent.Y,
						static_cast<double>(ZSign) * Extent.Z);
					const double Rho = FVector::DotProduct(Normal, Corner);
					OutMinRho = FMath::Min(OutMinRho, Rho);
					OutMaxRho = FMath::Max(OutMaxRho, Rho);
				}
			}
		}
	}

	double ComputeGodOfWarCandidateSpacing(const FPlaneCoverSettings& Settings)
	{
		return FMath::Max(Settings.ErrorTolerance * FMath::Max(0.1, Settings.GodOfWarCandidateSpacingMultiplier), 0.1);
	}

	void BuildGodOfWarCandidateRhos(
		const TArray<FVector>& Directions,
		const FBox& Bounds,
		const double CandidateSpacing,
		TArray<TArray<double>>& OutRhosByDirection)
	{
		OutRhosByDirection.Reset(Directions.Num());
		if (!Bounds.IsValid || CandidateSpacing <= 0.0)
		{
			return;
		}

		const FVector BoundsCenter = Bounds.GetCenter();
		for (const FVector& Direction : Directions)
		{
			TArray<double>& Rhos = OutRhosByDirection.AddDefaulted_GetRef();
			double MinRho = 0.0;
			double MaxRho = 0.0;
			ComputeBoundsProjectionRange(Bounds, Direction, MinRho, MaxRho);
			const double CenterRho = FVector::DotProduct(Direction, BoundsCenter);
			const int32 StartStep = FMath::CeilToInt((MinRho - CenterRho) / CandidateSpacing);
			const int32 EndStep = FMath::FloorToInt((MaxRho - CenterRho) / CandidateSpacing);

			for (int32 Step = StartStep; Step <= EndStep; ++Step)
			{
				Rhos.Add(CenterRho + static_cast<double>(Step) * CandidateSpacing);
			}

			if (Rhos.IsEmpty())
			{
				Rhos.Add(FMath::Clamp(CenterRho, MinRho, MaxRho));
			}
		}
	}

	void PrecomputeGodOfWarProjectedAreas(
		const TArray<FSourceTriangle>& Triangles,
		const TArray<FVector>& Directions,
		TArray<double>& OutProjectedAreas)
	{
		OutProjectedAreas.SetNumZeroed(Directions.Num() * Triangles.Num());
		for (int32 DirectionIndex = 0; DirectionIndex < Directions.Num(); ++DirectionIndex)
		{
			const FVector& Direction = Directions[DirectionIndex];
			const int32 DirectionOffset = DirectionIndex * Triangles.Num();
			for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
			{
				OutProjectedAreas[DirectionOffset + TriangleIndex] = ComputeContribution(Triangles[TriangleIndex], Direction);
			}
		}
	}

	struct FGodOfWarBestCard
	{
		int32 DirectionIndex = INDEX_NONE;
		double Rho = 0.0;
		double Score = 0.0;
		double CoveredArea = 0.0;
		int32 TriangleCount = 0;
	};

	FGodOfWarBestCard FindBestGodOfWarCard(
		const TArray<FSourceTriangle>& Triangles,
		const TBitArray<>& CoveredTriangles,
		const FPlaneCoverSettings& Settings,
		const TArray<FVector>& Directions,
		const TArray<TArray<double>>& RhosByDirection,
		const TArray<double>& ProjectedAreas,
		int32& OutCandidatesTested)
	{
		FGodOfWarBestCard BestCard;
		OutCandidatesTested = 0;
		const int32 TriangleCount = Triangles.Num();

		for (int32 DirectionIndex = 0; DirectionIndex < Directions.Num(); ++DirectionIndex)
		{
			if (!RhosByDirection.IsValidIndex(DirectionIndex))
			{
				continue;
			}

			const FVector& Direction = Directions[DirectionIndex];
			const int32 DirectionOffset = DirectionIndex * TriangleCount;
			for (const double Rho : RhosByDirection[DirectionIndex])
			{
				++OutCandidatesTested;
				double CandidateScore = 0.0;
				double CandidateArea = 0.0;
				int32 CandidateTriangleCount = 0;

				for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
				{
					if (CoveredTriangles[TriangleIndex])
					{
						continue;
					}

					if (!IsTriangleInsideGodOfWarThickSlice(Triangles[TriangleIndex], Direction, Rho, Settings))
					{
						continue;
					}

					CandidateScore += ProjectedAreas[DirectionOffset + TriangleIndex];
					CandidateArea += Triangles[TriangleIndex].Area;
					++CandidateTriangleCount;
				}

				if (CandidateTriangleCount > 0 && CandidateScore > BestCard.Score)
				{
					BestCard.DirectionIndex = DirectionIndex;
					BestCard.Rho = Rho;
					BestCard.Score = CandidateScore;
					BestCard.CoveredArea = CandidateArea;
					BestCard.TriangleCount = CandidateTriangleCount;
				}
			}
		}

		return BestCard;
	}

	void CollectGodOfWarCardTriangles(
		const TArray<FSourceTriangle>& Triangles,
		const TBitArray<>& CoveredTriangles,
		const FPlaneCoverSettings& Settings,
		const FVector& Direction,
		const double Rho,
		const TArray<double>& ProjectedAreas,
		const int32 DirectionIndex,
		TArray<int32>& OutTriangleIndices,
		double& OutScore,
		double& OutCoveredArea)
	{
		OutTriangleIndices.Reset();
		OutScore = 0.0;
		OutCoveredArea = 0.0;
		const int32 DirectionOffset = DirectionIndex * Triangles.Num();
		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			if (CoveredTriangles[TriangleIndex])
			{
				continue;
			}

			if (!IsTriangleInsideGodOfWarThickSlice(Triangles[TriangleIndex], Direction, Rho, Settings))
			{
				continue;
			}

			OutTriangleIndices.Add(TriangleIndex);
			OutScore += ProjectedAreas[DirectionOffset + TriangleIndex];
			OutCoveredArea += Triangles[TriangleIndex].Area;
		}
	}

	void ApplyGodOfWarFinalFaceReclaim(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings, FPlaneCoverResult& Result)
	{
		if (Settings.Technique != EPlaneCoverTechnique::GodOfWarCards || Result.Planes.Num() <= 1 || Triangles.IsEmpty())
		{
			return;
		}

		TArray<int32> OriginalPlaneByTriangle;
		OriginalPlaneByTriangle.Init(INDEX_NONE, Triangles.Num());
		for (int32 PlaneIndex = 0; PlaneIndex < Result.Planes.Num(); ++PlaneIndex)
		{
			for (const int32 TriangleIndex : Result.Planes[PlaneIndex].TriangleIndices)
			{
				if (OriginalPlaneByTriangle.IsValidIndex(TriangleIndex) && OriginalPlaneByTriangle[TriangleIndex] == INDEX_NONE)
				{
					OriginalPlaneByTriangle[TriangleIndex] = PlaneIndex;
				}
			}
		}

		TArray<TArray<int32>> ReclaimedTriangleIndices;
		ReclaimedTriangleIndices.SetNum(Result.Planes.Num());
		TArray<double> ReclaimedScores;
		ReclaimedScores.Init(0.0, Result.Planes.Num());
		TArray<double> ReclaimedAreas;
		ReclaimedAreas.Init(0.0, Result.Planes.Num());

		int32 CoveredTriangleCount = 0;
		int32 ReassignedTriangleCount = 0;
		double CoveredArea = 0.0;

		for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
		{
			const FSourceTriangle& Triangle = Triangles[TriangleIndex];
			const int32 OriginalPlaneIndex = OriginalPlaneByTriangle[TriangleIndex];
			if (!Result.Planes.IsValidIndex(OriginalPlaneIndex))
			{
				continue;
			}

			int32 BestPlaneIndex = INDEX_NONE;
			double BestScore = -TNumericLimits<double>::Max();

			for (int32 PlaneIndex = 0; PlaneIndex < Result.Planes.Num(); ++PlaneIndex)
			{
				const FPlaneCoverPlane& Plane = Result.Planes[PlaneIndex];
				if (!IsTriangleInsideGodOfWarThickSlice(Triangle, Plane.Normal, Plane.Rho, Settings))
				{
					continue;
				}

				const double Score = ComputeContribution(Triangle, Plane.Normal);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestPlaneIndex = PlaneIndex;
				}
			}

			if (BestPlaneIndex == INDEX_NONE)
			{
				BestPlaneIndex = OriginalPlaneIndex;
				BestScore = ComputeContribution(Triangle, Result.Planes[OriginalPlaneIndex].Normal);
			}

			if (!Result.Planes.IsValidIndex(BestPlaneIndex))
			{
				continue;
			}

			ReclaimedTriangleIndices[BestPlaneIndex].Add(TriangleIndex);
			ReclaimedScores[BestPlaneIndex] += BestScore;
			ReclaimedAreas[BestPlaneIndex] += Triangle.Area;
			++CoveredTriangleCount;
			CoveredArea += Triangle.Area;

			if (OriginalPlaneIndex != BestPlaneIndex)
			{
				++ReassignedTriangleCount;
			}
		}

		for (int32 PlaneIndex = 0; PlaneIndex < Result.Planes.Num(); ++PlaneIndex)
		{
			Result.Planes[PlaneIndex].TriangleIndices = MoveTemp(ReclaimedTriangleIndices[PlaneIndex]);
			Result.Planes[PlaneIndex].Score = ReclaimedScores[PlaneIndex];
			Result.Planes[PlaneIndex].CoveredArea = ReclaimedAreas[PlaneIndex];
		}

		for (int32 PlaneIndex = Result.Planes.Num() - 1; PlaneIndex >= 0; --PlaneIndex)
		{
			if (Result.Planes[PlaneIndex].TriangleIndices.IsEmpty())
			{
				Result.Planes.RemoveAt(PlaneIndex, 1, EAllowShrinking::No);
			}
		}

		Result.CoveredTriangleCount = CoveredTriangleCount;
		Result.CoveredArea = CoveredArea;
		Result.FinalReassignedTriangleCount = ReassignedTriangleCount;
	}

	FPlaneCoverResult BuildGodOfWarPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings)
	{
		const double TotalStartSeconds = FPlatformTime::Seconds();
		FPlaneCoverResult Result;
		Result.SourceTriangleCount = Triangles.Num();
		for (const FSourceTriangle& Triangle : Triangles)
		{
			Result.SourceArea += Triangle.Area;
		}

		if (Triangles.IsEmpty())
		{
			Result.TotalSeconds = FPlatformTime::Seconds() - TotalStartSeconds;
			return Result;
		}

		const double CandidateBuildStartSeconds = FPlatformTime::Seconds();
		const FBox Bounds = ComputeSourceBounds(Triangles);
		const double CandidateSpacing = ComputeGodOfWarCandidateSpacing(Settings);
		TArray<FVector> Directions;
		GenerateGeodesicHemisphereDirections(Settings.GodOfWarGeodesicSubdivisions, Directions);

		TArray<TArray<double>> RhosByDirection;
		BuildGodOfWarCandidateRhos(Directions, Bounds, CandidateSpacing, RhosByDirection);

		TArray<double> ProjectedAreas;
		PrecomputeGodOfWarProjectedAreas(Triangles, Directions, ProjectedAreas);
		Result.DensityBuildSeconds = FPlatformTime::Seconds() - CandidateBuildStartSeconds;
		Result.CandidatePlaneCount = Directions.Num();

		TBitArray<> CoveredTriangles;
		CoveredTriangles.Init(false, Triangles.Num());
		int32 RemainingTriangleCount = Triangles.Num();
		while (RemainingTriangleCount > 0)
		{
			const double SearchStartSeconds = FPlatformTime::Seconds();
			int32 CandidatesTested = 0;
			const FGodOfWarBestCard BestCard = FindBestGodOfWarCard(
				Triangles,
				CoveredTriangles,
				Settings,
				Directions,
				RhosByDirection,
				ProjectedAreas,
				CandidatesTested);
			Result.CandidateSearchSeconds += FPlatformTime::Seconds() - SearchStartSeconds;
			Result.TotalCandidatePlaneCount += CandidatesTested;
			Result.MaxIterationCandidatePlaneCount = FMath::Max(Result.MaxIterationCandidatePlaneCount, CandidatesTested);
			++Result.GreedyIterationCount;

			FPlaneCoverPlane NewPlane;
			if (Directions.IsValidIndex(BestCard.DirectionIndex))
			{
				double Score = 0.0;
				double CoveredArea = 0.0;
				TArray<int32> ClaimedTriangleIndices;
				CollectGodOfWarCardTriangles(
					Triangles,
					CoveredTriangles,
					Settings,
					Directions[BestCard.DirectionIndex],
					BestCard.Rho,
					ProjectedAreas,
					BestCard.DirectionIndex,
					ClaimedTriangleIndices,
					Score,
					CoveredArea);

				NewPlane.Normal = Directions[BestCard.DirectionIndex];
				NewPlane.Rho = BestCard.Rho;
				NewPlane.Score = Score;
				NewPlane.CoveredArea = CoveredArea;
				NewPlane.TriangleIndices = MoveTemp(ClaimedTriangleIndices);
			}
			else
			{
				break;
			}

			if (NewPlane.TriangleIndices.IsEmpty())
			{
				break;
			}

			FPlaneCoverPlane& ResultPlane = Result.Planes.AddDefaulted_GetRef();
			ResultPlane = MoveTemp(NewPlane);
			for (const int32 TriangleIndex : ResultPlane.TriangleIndices)
			{
				if (!CoveredTriangles[TriangleIndex])
				{
					CoveredTriangles[TriangleIndex] = true;
					--RemainingTriangleCount;
					++Result.CoveredTriangleCount;
					Result.CoveredArea += Triangles[TriangleIndex].Area;
				}
			}
		}

		ApplyGodOfWarFinalFaceReclaim(Triangles, Settings, Result);
		Result.CandidatePrepareSeconds = 0.0;
		Result.CandidatePlaneBuildSeconds = Result.DensityBuildSeconds;
		Result.TotalSeconds = FPlatformTime::Seconds() - TotalStartSeconds;
		return Result;
	}

	FPlaneCoverResult BuildGreedyPlaneCoverSingle(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings)
	{
		const double TotalStartSeconds = FPlatformTime::Seconds();
		FPlaneCoverResult Result;
		Result.SourceTriangleCount = Triangles.Num();

		for (const FSourceTriangle& Triangle : Triangles)
		{
			Result.SourceArea += Triangle.Area;
		}

		TBitArray<> CoveredTriangles;
		CoveredTriangles.Init(false, Triangles.Num());

		FPlaneSpaceDensityGrid DensityGrid;
		const double DensityBuildStartSeconds = FPlatformTime::Seconds();
		InitializeDensityGrid(DensityGrid, Triangles, Settings);
		Result.DensityBuildSeconds = FPlatformTime::Seconds() - DensityBuildStartSeconds;

		int32 RemainingTriangleCount = Triangles.Num();
		while (RemainingTriangleCount > 0)
		{
			const double CandidateSearchStartSeconds = FPlatformTime::Seconds();
			const double CandidatePlaneBuildStartSeconds = FPlatformTime::Seconds();
			const TArray<FCandidatePlane> CandidatePlanes = BuildCandidatePlanesFromGrid(DensityGrid, Triangles, CoveredTriangles, Settings);
			Result.CandidatePlaneBuildSeconds += FPlatformTime::Seconds() - CandidatePlaneBuildStartSeconds;

			const double CandidatePrepareStartSeconds = FPlatformTime::Seconds();
			const TArray<FPreparedCandidate> PreparedCandidates = PrepareCandidates(Triangles, CoveredTriangles, CandidatePlanes, Settings);
			Result.CandidatePrepareSeconds += FPlatformTime::Seconds() - CandidatePrepareStartSeconds;

			Result.GreedyIterationCount++;
			Result.TotalCandidatePlaneCount += PreparedCandidates.Num();
			Result.MaxIterationCandidatePlaneCount = FMath::Max(Result.MaxIterationCandidatePlaneCount, PreparedCandidates.Num());

			int32 BestCandidateIndex = INDEX_NONE;
			double BestScore = 0.0;
			TArray<int32> BestTriangleIndices;

			for (int32 CandidateIndex = 0; CandidateIndex < PreparedCandidates.Num(); ++CandidateIndex)
			{
				const FPreparedCandidate& Candidate = PreparedCandidates[CandidateIndex];
				const double Score = Candidate.SelectionScore;
				if (Score > BestScore && !Candidate.ValidTriangleIndices.IsEmpty())
				{
					BestCandidateIndex = CandidateIndex;
					BestScore = Score;
					BestTriangleIndices = Candidate.ValidTriangleIndices;
				}
			}
			Result.CandidateSearchSeconds += FPlatformTime::Seconds() - CandidateSearchStartSeconds;

			if (BestCandidateIndex == INDEX_NONE)
			{
				break;
			}

			const FPreparedCandidate& BestCandidate = PreparedCandidates[BestCandidateIndex];
			FVector FinalPlaneNormal = BestCandidate.Plane.Normal;
			double FinalPlaneRho = BestCandidate.Plane.Rho;

			FPlaneCoverPlane& NewPlane = Result.Planes.AddDefaulted_GetRef();
			NewPlane.Normal = FinalPlaneNormal;
			NewPlane.Rho = FinalPlaneRho;
			NewPlane.Score = BestScore;
			NewPlane.CoveredArea = BestCandidate.CoveredArea;
			NewPlane.TriangleIndices = MoveTemp(BestTriangleIndices);

			const double DensityUpdateStartSeconds = FPlatformTime::Seconds();
			RemoveTrianglesFromDensityGrid(DensityGrid, Triangles, NewPlane.TriangleIndices, Settings);
			Result.DensityUpdateSeconds += FPlatformTime::Seconds() - DensityUpdateStartSeconds;

			for (const int32 TriangleIndex : NewPlane.TriangleIndices)
			{
				if (!CoveredTriangles[TriangleIndex])
				{
					CoveredTriangles[TriangleIndex] = true;
					--RemainingTriangleCount;
					++Result.CoveredTriangleCount;
					Result.CoveredArea += Triangles[TriangleIndex].Area;
				}
			}
		}

		Result.CandidatePlaneCount = Result.MaxIterationCandidatePlaneCount;
		Result.TotalSeconds = FPlatformTime::Seconds() - TotalStartSeconds;
		return Result;
	}

	FPlaneCoverResult BuildGreedyPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings)
	{
		return BuildGreedyPlaneCoverSingle(Triangles, Settings);
	}

	FPlaneCoverResult BuildPlaneCover(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverSettings& Settings)
	{
		if (Settings.Technique == EPlaneCoverTechnique::KMeansClustering)
		{
			return BuildKMeansPlaneCover(Triangles, Settings);
		}
		if (Settings.Technique == EPlaneCoverTechnique::GodOfWarCards)
		{
			return BuildGodOfWarPlaneCover(Triangles, Settings);
		}

		return BuildGreedyPlaneCover(Triangles, Settings);
	}

	bool BuildPlaneProxyMeshDescription(const TArray<FSourceTriangle>& Triangles, const FPlaneCoverResult& Result, const FPlaneCoverSettings& Settings, FMeshDescription& OutMeshDescription, FPlaneProxyMeshStats& OutStats, FString& OutError, TArray<FPlaneProxyPlaneInfo>* OutPlaneInfos)
	{
		OutMeshDescription.Empty();
		OutStats = FPlaneProxyMeshStats();
		OutError.Reset();
		if (OutPlaneInfos)
		{
			OutPlaneInfos->Reset();
		}

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

		OutMeshDescription.ReserveNewVertices(Result.Planes.Num() * 4);
		OutMeshDescription.ReserveNewVertexInstances(Result.Planes.Num() * 4);
		OutMeshDescription.ReserveNewPolygons(Result.Planes.Num());

		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = TEXT("BillboardProxy");

		const double Padding = FMath::Max(0.0, Settings.ErrorTolerance * 0.5);
		const double MinHalfExtent = FMath::Max(1.0, Settings.ErrorTolerance * 0.25);
		TArray<FPreparedProxyPlane> PreparedPlanes;
		PreparedPlanes.Reserve(Result.Planes.Num());

		for (int32 SourcePlaneIndex = 0; SourcePlaneIndex < Result.Planes.Num(); ++SourcePlaneIndex)
		{
			const FPlaneCoverPlane& Plane = Result.Planes[SourcePlaneIndex];
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
				? ComputeFixedFramePlaneRectangle(Triangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, AxisU, AxisV, MinU, MaxU, MinV, MaxV)
				: ComputeMinimumAreaPlaneRectangle(Triangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, AxisU, AxisV, MinU, MaxU, MinV, MaxV);
			if (!bComputedRectangle)
			{
				continue;
			}

			double MinSignedDistance = 0.0;
			double MaxSignedDistance = 0.0;
			ComputeSignedDistanceRangeForTriangles(Triangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, MinSignedDistance, MaxSignedDistance);
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

		ApplyKMeansEnvelopeCrackReduction(Triangles, Settings, PreparedPlanes);

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

		double PlaneToShadingNormalDotSum = 0.0;
		for (const FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
		{
			const FVector2f MaskUV = PreparedPlane.bIsTrunkCard
				? FVector2f(0.0f, 0.0f)
				: FVector2f(1.0f, 0.0f);
			if (!AddQuadPolygon(OutMeshDescription, PolygonGroupID, VertexPositions, VertexInstanceUVs, VertexInstanceNormals, VertexInstanceTangents, VertexInstanceBinormalSigns, TriangleNormals, TriangleTangents, TriangleBinormals, EdgeHardnesses, PreparedPlane.Corners, PreparedPlane.AtlasUVs, PreparedPlane.BackAtlasUVs, MaskUV, PreparedPlane.ShadingNormal))
			{
				continue;
			}

			if (OutPlaneInfos)
			{
				FPlaneProxyPlaneInfo& PlaneInfo = OutPlaneInfos->AddDefaulted_GetRef();
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
				PlaneInfo.AtlasTileResolution = FMath::Min(PreparedPlane.AtlasTileSize.X, PreparedPlane.AtlasTileSize.Y);
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
			}

			PlaneToShadingNormalDotSum += PreparedPlane.PlaneToShadingNormalDot;
			++OutStats.PlaneCount;
			++OutStats.QuadCount;
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

	bool ApplyPlaneProxyTileCropsAndRebuildMeshDescription(
		TArray<FPlaneProxyPlaneInfo>& PlaneInfos,
		const TArray<FPlaneProxyTileCrop>& TileCrops,
		const FPlaneCoverSettings& Settings,
		FMeshDescription& OutMeshDescription,
		FPlaneProxyMeshStats& InOutStats,
		FString& OutError)
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
			PlaneInfo.MinU = FMath::Lerp(OldMinU, OldMaxU, MinUFraction);
			PlaneInfo.MaxU = FMath::Lerp(OldMinU, OldMaxU, MaxUFraction);
			PlaneInfo.MinV = FMath::Lerp(OldMinV, OldMaxV, MinVFraction);
			PlaneInfo.MaxV = FMath::Lerp(OldMinV, OldMaxV, MaxVFraction);
			UpdatePlaneInfoCornersFromBounds(PlaneInfo);
			bAppliedAnyCrop = true;
		}

		if (!bAppliedAnyCrop)
		{
			return true;
		}

		if (!PackPlaneInfosIntoAtlas(
			PlaneInfos,
			Settings,
			InOutStats.AtlasWidth,
			InOutStats.AtlasHeight,
			InOutStats.AtlasTileResolution,
			InOutStats.AtlasTilePaddingPixels))
		{
			OutError = TEXT("Could not repack alpha-cropped billboard texture tiles into the configured atlas resolution.");
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
		OutMeshDescription.ReserveNewVertices(PlaneInfos.Num() * 4);
		OutMeshDescription.ReserveNewVertexInstances(PlaneInfos.Num() * 4);
		OutMeshDescription.ReserveNewPolygons(PlaneInfos.Num());

		const FPolygonGroupID PolygonGroupID = OutMeshDescription.CreatePolygonGroup();
		PolygonGroupImportedMaterialSlotNames[PolygonGroupID] = TEXT("BillboardProxy");

		InOutStats.PlaneCount = 0;
		InOutStats.QuadCount = 0;
		for (const FPlaneProxyPlaneInfo& PlaneInfo : PlaneInfos)
		{
			const FVector2f MaskUV = PlaneInfo.bIsTrunkCard
				? FVector2f(0.0f, 0.0f)
				: FVector2f(1.0f, 0.0f);
			if (!AddQuadPolygon(OutMeshDescription, PolygonGroupID, VertexPositions, VertexInstanceUVs, VertexInstanceNormals, VertexInstanceTangents, VertexInstanceBinormalSigns, TriangleNormals, TriangleTangents, TriangleBinormals, EdgeHardnesses, PlaneInfo.Corners, PlaneInfo.AtlasUVs, PlaneInfo.BackAtlasUVs, MaskUV, PlaneInfo.ShadingNormal))
			{
				continue;
			}

			++InOutStats.PlaneCount;
			++InOutStats.QuadCount;
		}

		InOutStats.TriangleCount = OutMeshDescription.Triangles().Num();
		if (InOutStats.PlaneCount == 0)
		{
			OutError = TEXT("No alpha-cropped proxy planes could be rebuilt.");
			return false;
		}

		return true;
	}

	FString SummarizePlaneCover(const FString& MeshName, const FPlaneCoverSettings& Settings, const FPlaneCoverResult& Result)
	{
		const double CoveredTrianglePercent = Result.SourceTriangleCount > 0
			? 100.0 * static_cast<double>(Result.CoveredTriangleCount) / static_cast<double>(Result.SourceTriangleCount)
			: 0.0;
		const double CoveredAreaPercent = Result.SourceArea > 0.0
			? 100.0 * Result.CoveredArea / Result.SourceArea
			: 0.0;

		const FString MetricSummary = FString::Printf(TEXT("object-space, epsilon=%.2f cm"), Settings.ErrorTolerance);
		const FString PenaltySummary = FString::Printf(TEXT("on, weight=%.2f, directional missed set"), PaperPenaltyWeight);
		FString KMeansCrackReductionSummary = TEXT("off");
		if (Settings.KMeansCrackReductionMode == EKMeansCrackReductionMode::ScaledEnvelopeClip)
		{
			KMeansCrackReductionSummary = FString::Printf(
				TEXT("scaled envelope-clipped projections, scale=%.2f"),
				Settings.KMeansCrackReductionProjectionScale);
		}
		FString TextureSummary;
		const TCHAR* DoubleSidedBakeSummary = TEXT("off");
		switch (Settings.DoubleSidedBakeMode)
		{
		case EDoubleSidedBakeMode::TrunkCardsOnly:
			DoubleSidedBakeSummary = TEXT("trunk cards only");
			break;
		case EDoubleSidedBakeMode::BillboardPlanesOnly:
			DoubleSidedBakeSummary = TEXT("billboard planes only");
			break;
		case EDoubleSidedBakeMode::AllPlanes:
			DoubleSidedBakeSummary = TEXT("all planes");
			break;
		case EDoubleSidedBakeMode::Off:
		default:
			break;
		}
		if (Settings.Technique == EPlaneCoverTechnique::KMeansClustering)
		{
			TextureSummary = FString::Printf(
				TEXT("on, fixed %dx%d atlas, auto-scaled packed object-space tiles, padding=%d px, cluster projection, crack reduction=%s, double-sided bake=%s"),
				Settings.TextureAtlasResolution,
				Settings.TextureAtlasResolution,
				Settings.TextureTilePaddingPixels,
				*KMeansCrackReductionSummary,
				DoubleSidedBakeSummary);
		}
		else if (Settings.Technique == EPlaneCoverTechnique::GodOfWarCards)
		{
			TextureSummary = FString::Printf(
				TEXT("on, fixed %dx%d atlas, auto-scaled packed object-space tiles, padding=%d px, per-card ortho bounds, closeness clipped, reclaimed faces only, double-sided bake=%s"),
				Settings.TextureAtlasResolution,
				Settings.TextureAtlasResolution,
				Settings.TextureTilePaddingPixels,
				DoubleSidedBakeSummary);
		}
		else
		{
			TextureSummary = FString::Printf(
				TEXT("on, fixed %dx%d atlas, auto-scaled packed object-space tiles, padding=%d px, clipped valid-zone shooting, double-sided bake=%s"),
				Settings.TextureAtlasResolution,
				Settings.TextureAtlasResolution,
				Settings.TextureTilePaddingPixels,
				DoubleSidedBakeSummary);
		}

		if (Settings.Technique == EPlaneCoverTechnique::KMeansClustering)
		{
			double TotalKMeansDistance = 0.0;
			for (const FPlaneCoverPlane& Plane : Result.Planes)
			{
				TotalKMeansDistance += -Plane.Score;
			}

			FString Summary = FString::Printf(
				TEXT("%s\n  algorithm: improved billboard clouds k-means best-fit plane clustering\n  triangles: %d, target planes: %d, output planes: %d\n  iterations: %d, covered: %d / %d (%.1f%%), area: %.1f%%\n  metric: budget-driven triangle-to-plane distance, total distance %.3f\n  initialization: bounding-sphere tangent planes, one-pass coverage-centroid relaxation, count-based minimum-coverage cluster replacement\n  assignment: nearest plane by summed vertex-to-plane distance\n  empty clusters: handled by minimum-coverage replacement, skipped if still empty\n  plane refit: symmetric covariance eigensolve / SVD-equivalent best-fit plane\n  billboard footprint: object-space projection minimum-area rectangle\n  texture atlas: %s\n  timing: total %.2fs, initialization %.2fs, assignment %.2fs, plane refit %.2fs"),
				*MeshName,
				Result.SourceTriangleCount,
				Settings.KMeansPlaneCount,
				Result.Planes.Num(),
				Result.GreedyIterationCount,
				Result.CoveredTriangleCount,
				Result.SourceTriangleCount,
				CoveredTrianglePercent,
				CoveredAreaPercent,
				TotalKMeansDistance,
				*TextureSummary,
				Result.TotalSeconds,
				Result.DensityBuildSeconds,
				Result.CandidateSearchSeconds,
				Result.CandidatePlaneBuildSeconds);

			const int32 PreviewPlaneCount = FMath::Min(5, Result.Planes.Num());
			for (int32 PlaneIndex = 0; PlaneIndex < PreviewPlaneCount; ++PlaneIndex)
			{
				const FPlaneCoverPlane& Plane = Result.Planes[PlaneIndex];
				Summary += FString::Printf(
					TEXT("\n    plane %d: tris=%d, area=%.1f, distance=%.1f, n=(%.2f %.2f %.2f), rho=%.2f"),
					PlaneIndex,
					Plane.TriangleIndices.Num(),
					Plane.CoveredArea,
					-Plane.Score,
					Plane.Normal.X,
					Plane.Normal.Y,
					Plane.Normal.Z,
					Plane.Rho);
			}

			return Summary;
		}

		if (Settings.Technique == EPlaneCoverTechnique::GodOfWarCards)
		{
			double TotalScore = 0.0;
			for (const FPlaneCoverPlane& Plane : Result.Planes)
			{
				TotalScore += Plane.Score;
			}

			FString Summary = FString::Printf(
				TEXT("%s\n  algorithm: God of War greedy card capture\n  triangles: %d, cards: %d, covered: %d / %d (%.1f%%), area: %.1f%%\n  metric: thick object-space slice, closeness=%.2f cm, all triangle vertices must satisfy abs(dot(N, vertex) - rho) <= closeness\n  candidate space: 6D card planes reduced to direction + distance from bounds center\n  directions: geodesic hemisphere subdivisions=%d, directions=%d\n  distance sampling: spacing %.2fx closeness, candidate planes tested total=%d, max %d/iteration\n  score: sum projected triangle areas, world area * abs(dot(triangle normal, card normal)), total %.1f\n  greedy claim: best sampled card claims close faces, repeat until no sampled card can claim remaining faces\n  fallback cards: off; increase geodesic subdivisions, reduce spacing, or increase closeness if coverage is below 100%%\n  final reclaim: retested claimed faces against close final cards, moved=%d\n  billboard footprint: object-space projection minimum-area rectangle\n  texture atlas: %s\n  timing: total %.2fs, candidate setup %.2fs, search %.2fs"),
				*MeshName,
				Result.SourceTriangleCount,
				Result.Planes.Num(),
				Result.CoveredTriangleCount,
				Result.SourceTriangleCount,
				CoveredTrianglePercent,
				CoveredAreaPercent,
				Settings.ErrorTolerance,
				Settings.GodOfWarGeodesicSubdivisions,
				Result.CandidatePlaneCount,
				Settings.GodOfWarCandidateSpacingMultiplier,
				Result.TotalCandidatePlaneCount,
				Result.MaxIterationCandidatePlaneCount,
				TotalScore,
				Result.FinalReassignedTriangleCount,
				*TextureSummary,
				Result.TotalSeconds,
				Result.DensityBuildSeconds,
				Result.CandidateSearchSeconds);

			const int32 PreviewPlaneCount = FMath::Min(5, Result.Planes.Num());
			for (int32 PlaneIndex = 0; PlaneIndex < PreviewPlaneCount; ++PlaneIndex)
			{
				const FPlaneCoverPlane& Plane = Result.Planes[PlaneIndex];
				Summary += FString::Printf(
					TEXT("\n    card %d: tris=%d, area=%.1f, score=%.1f, n=(%.2f %.2f %.2f), rho=%.2f"),
					PlaneIndex,
					Plane.TriangleIndices.Num(),
					Plane.CoveredArea,
					Plane.Score,
					Plane.Normal.X,
					Plane.Normal.Y,
					Plane.Normal.Z,
					Plane.Rho);
			}

			return Summary;
		}

		FString Summary = FString::Printf(
			TEXT("%s\n  algorithm: RR-4485 greedy plane-space cover\n  triangles: %d, picked candidates: max %d/iter, total %d\n  iterations: %d, planes: %d, covered: %d / %d (%.1f%%), area: %.1f%%\n  metric: %s\n  grid: theta bins=%d, phi bins=%d, rho bins=%d\n  density: angular-bin conservative rho range, rho-overlap weighted contribution, compactness weight %.2f\n  density update: persistent grid, incremental remove collapsed faces, parallel direction slices\n  adaptive refinement: theta/phi/rho 27-neighbor subdivision, parallel score-only sub-bin evaluation, shared candidate sets, winner-only index collection, safety depth %d\n  validity: angular-bin vertex extrema rho range, per-cell angular context\n  final validity: strict oriented final set\n  plane placement: refined density plane used directly\n  billboard footprint: object-space projection minimum-area rectangle\n  texture atlas: %s\n  timing: total %.2fs, density init %.2fs, search/refine %.2fs, candidate build/refine %.2fs, prepare %.2fs, density update %.2fs\n  penalty: %s"),
			*MeshName,
			Result.SourceTriangleCount,
			Result.MaxIterationCandidatePlaneCount,
			Result.TotalCandidatePlaneCount,
			Result.GreedyIterationCount,
			Result.Planes.Num(),
			Result.CoveredTriangleCount,
			Result.SourceTriangleCount,
			CoveredTrianglePercent,
			CoveredAreaPercent,
			*MetricSummary,
			Settings.NormalThetaSteps,
			Settings.NormalPhiSteps,
			Settings.RhoBinCount,
			Settings.TextureCompactnessWeight,
			Settings.AdaptiveRefinementDepth,
			*TextureSummary,
			Result.TotalSeconds,
			Result.DensityBuildSeconds,
			Result.CandidateSearchSeconds,
			Result.CandidatePlaneBuildSeconds,
			Result.CandidatePrepareSeconds,
			Result.DensityUpdateSeconds,
			*PenaltySummary);

		const int32 PreviewPlaneCount = FMath::Min(5, Result.Planes.Num());
		for (int32 PlaneIndex = 0; PlaneIndex < PreviewPlaneCount; ++PlaneIndex)
		{
			const FPlaneCoverPlane& Plane = Result.Planes[PlaneIndex];
			Summary += FString::Printf(
				TEXT("\n    plane %d: tris=%d, area=%.1f, score=%.1f, n=(%.2f %.2f %.2f), rho=%.2f"),
				PlaneIndex,
				Plane.TriangleIndices.Num(),
				Plane.CoveredArea,
				Plane.Score,
				Plane.Normal.X,
				Plane.Normal.Y,
				Plane.Normal.Z,
				Plane.Rho);
		}

		return Summary;
	}
}
