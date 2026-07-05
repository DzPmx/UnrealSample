#include "BillboardCloudsPlaneCover.h"

#include "Async/ParallelFor.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

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
			FVector OrientedNormal = FVector::UpVector;
			double OrientedRho = 0.0;
			FVector AxisU = FVector::RightVector;
			FVector AxisV = FVector::UpVector;
			FVector ShadingNormal = FVector::UpVector;
			double MinU = 0.0;
			double MaxU = 0.0;
			double MinV = 0.0;
			double MaxV = 0.0;
			FVector Corners[4] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
			FVector2f AtlasUVs[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
			FIntPoint AtlasPixelMin = FIntPoint::ZeroValue;
			FIntPoint AtlasTileSize = FIntPoint::ZeroValue;
			int32 AtlasTilePaddingPixels = 0;
			double PlaneToShadingNormalDot = 1.0;
			TArray<int32> TriangleIndices;
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

		void SetPreparedPlaneAtlasUVs(FPreparedProxyPlane& Plane, const int32 AtlasWidth, const int32 AtlasHeight)
		{
			const double SafeAtlasWidth = static_cast<double>(FMath::Max(1, AtlasWidth));
			const double SafeAtlasHeight = static_cast<double>(FMath::Max(1, AtlasHeight));
			const double MinU = static_cast<double>(Plane.AtlasPixelMin.X) / SafeAtlasWidth;
			const double MinV = static_cast<double>(Plane.AtlasPixelMin.Y) / SafeAtlasHeight;
			const double MaxU = static_cast<double>(Plane.AtlasPixelMin.X + Plane.AtlasTileSize.X) / SafeAtlasWidth;
			const double MaxV = static_cast<double>(Plane.AtlasPixelMin.Y + Plane.AtlasTileSize.Y) / SafeAtlasHeight;

			Plane.AtlasUVs[0] = FVector2f(static_cast<float>(MinU), static_cast<float>(MinV));
			Plane.AtlasUVs[1] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MinV));
			Plane.AtlasUVs[2] = FVector2f(static_cast<float>(MaxU), static_cast<float>(MaxV));
			Plane.AtlasUVs[3] = FVector2f(static_cast<float>(MinU), static_cast<float>(MaxV));
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

			const int32 RequestedPadding = FMath::Clamp(Settings.TextureTilePaddingPixels, 0, 128);
			const int32 RequestedLargestInteriorDimension = FMath::Clamp(Settings.TextureTileResolution, 16, 1024);
			const int32 MaxAtlasResolution = FMath::Max(256, Settings.TextureAtlasMaxResolution);
			const double SafeSourceMaxDimension = FMath::Max(SourceMaxDimension, 1.0);
			const double BasePixelsPerUnit = static_cast<double>(RequestedLargestInteriorDimension) / SafeSourceMaxDimension;

			for (double Scale = 1.0; Scale >= 1.0 / 64.0; Scale *= 0.5)
			{
				TArray<FIntPoint> PitchSizes;
				PitchSizes.SetNum(PreparedPlanes.Num());
				TArray<int32> SortedPlaneIndices;
				SortedPlaneIndices.Reserve(PreparedPlanes.Num());
				int32 MaxPitchWidth = 1;

				for (int32 PlaneIndex = 0; PlaneIndex < PreparedPlanes.Num(); ++PlaneIndex)
				{
					FPreparedProxyPlane& PreparedPlane = PreparedPlanes[PlaneIndex];
					const double PlaneWidth = FMath::Max(PreparedPlane.MaxU - PreparedPlane.MinU, 1.0);
					const double PlaneHeight = FMath::Max(PreparedPlane.MaxV - PreparedPlane.MinV, 1.0);
					const int32 InteriorWidth = FMath::Clamp(FMath::CeilToInt(PlaneWidth * BasePixelsPerUnit * Scale), 1, RequestedLargestInteriorDimension);
					const int32 InteriorHeight = FMath::Clamp(FMath::CeilToInt(PlaneHeight * BasePixelsPerUnit * Scale), 1, RequestedLargestInteriorDimension);
					const int32 EffectivePadding = FMath::Clamp(RequestedPadding, 0, FMath::Max(0, (FMath::Min(InteriorWidth, InteriorHeight) - 1) / 2));
					PreparedPlane.AtlasTileSize = FIntPoint(InteriorWidth, InteriorHeight);
					PreparedPlane.AtlasTilePaddingPixels = EffectivePadding;

					const FIntPoint PitchSize(InteriorWidth + EffectivePadding * 2, InteriorHeight + EffectivePadding * 2);
					PitchSizes[PlaneIndex] = PitchSize;
					MaxPitchWidth = FMath::Max(MaxPitchWidth, PitchSize.X);
					SortedPlaneIndices.Add(PlaneIndex);
				}

				SortedPlaneIndices.Sort([&PitchSizes](const int32 A, const int32 B)
				{
					if (PitchSizes[A].Y != PitchSizes[B].Y)
					{
						return PitchSizes[A].Y > PitchSizes[B].Y;
					}
					return PitchSizes[A].X > PitchSizes[B].X;
				});

				int32 BestAtlasWidth = 0;
				int32 BestAtlasHeight = 0;
				double BestAtlasScore = TNumericLimits<double>::Max();
				TArray<FIntPoint> BestInteriorMins;

				for (int32 CandidateWidth = 1; CandidateWidth <= MaxAtlasResolution; CandidateWidth <<= 1)
				{
					if (CandidateWidth < MaxPitchWidth)
					{
						continue;
					}

					TArray<FIntPoint> CandidateInteriorMins;
					CandidateInteriorMins.SetNum(PreparedPlanes.Num());
					int32 CurrentX = 0;
					int32 CurrentY = 0;
					int32 RowHeight = 0;

					for (const int32 PlaneIndex : SortedPlaneIndices)
					{
						const FIntPoint PitchSize = PitchSizes[PlaneIndex];
						if (CurrentX > 0 && CurrentX + PitchSize.X > CandidateWidth)
						{
							CurrentY += RowHeight;
							CurrentX = 0;
							RowHeight = 0;
						}

						const int32 Padding = PreparedPlanes[PlaneIndex].AtlasTilePaddingPixels;
						CandidateInteriorMins[PlaneIndex] = FIntPoint(CurrentX + Padding, CurrentY + Padding);
						CurrentX += PitchSize.X;
						RowHeight = FMath::Max(RowHeight, PitchSize.Y);
					}

					const int32 RequiredHeight = CurrentY + RowHeight;
					const int32 CandidateHeight = RoundUpToPowerOfTwoWithinLimit(RequiredHeight, MaxAtlasResolution);
					if (CandidateHeight <= 0)
					{
						continue;
					}

					const int64 CandidateArea = static_cast<int64>(CandidateWidth) * static_cast<int64>(CandidateHeight);
					const double AspectRatio = static_cast<double>(FMath::Max(CandidateWidth, CandidateHeight))
						/ static_cast<double>(FMath::Max(1, FMath::Min(CandidateWidth, CandidateHeight)));
					const double AspectPenalty = AspectRatio * AspectRatio;
					const double CandidateScore = static_cast<double>(CandidateArea) * AspectPenalty;
					if (CandidateScore < BestAtlasScore)
					{
						BestAtlasWidth = CandidateWidth;
						BestAtlasHeight = CandidateHeight;
						BestAtlasScore = CandidateScore;
						BestInteriorMins = MoveTemp(CandidateInteriorMins);
					}
				}

				if (BestAtlasWidth <= 0 || BestAtlasHeight <= 0)
				{
					continue;
				}

				OutAtlasWidth = BestAtlasWidth;
				OutAtlasHeight = BestAtlasHeight;
				OutLargestInteriorDimension = 0;
				OutLargestPadding = 0;
				for (int32 PlaneIndex = 0; PlaneIndex < PreparedPlanes.Num(); ++PlaneIndex)
				{
					FPreparedProxyPlane& PreparedPlane = PreparedPlanes[PlaneIndex];
					PreparedPlane.AtlasPixelMin = BestInteriorMins[PlaneIndex];
					SetPreparedPlaneAtlasUVs(PreparedPlane, OutAtlasWidth, OutAtlasHeight);
					OutLargestInteriorDimension = FMath::Max(OutLargestInteriorDimension, FMath::Max(PreparedPlane.AtlasTileSize.X, PreparedPlane.AtlasTileSize.Y));
					OutLargestPadding = FMath::Max(OutLargestPadding, PreparedPlane.AtlasTilePaddingPixels);
				}

				return true;
			}

			return false;
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

		const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(LODIndex);
		if (!MeshDescription)
		{
			OutError = FString::Printf(TEXT("LOD %d has no MeshDescription source data."), LODIndex);
			return false;
		}

		const TVertexAttributesConstRef<FVector3f> VertexPositions = MeshDescription->GetVertexPositions();
		const FStaticMeshConstAttributes MeshAttributes(*MeshDescription);
		const bool bHasVertexInstanceNormals = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Normal);
		const bool bHasVertexInstanceUVs = MeshDescription->VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::TextureCoordinate);
		const bool bHasPolygonGroupMaterialSlots = MeshDescription->PolygonGroupAttributes().HasAttribute(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
		TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceNormals;
		if (bHasVertexInstanceNormals)
		{
			VertexInstanceNormals = MeshAttributes.GetVertexInstanceNormals();
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
			if (bHasVertexInstanceNormals)
			{
				const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs = MeshDescription->GetTriangleVertexInstances(TriangleID);
				FVector AveragedNormal = FVector::ZeroVector;
				for (const FVertexInstanceID VertexInstanceID : TriangleVertexInstanceIDs)
				{
					AveragedNormal += FVector(VertexInstanceNormals[VertexInstanceID]);
				}

				if (!AveragedNormal.IsNearlyZero())
				{
					Triangle.ShadingNormal = AveragedNormal.GetSafeNormal();
					Triangle.bHasSourceShadingNormal = true;
				}
			}
			if (bHasVertexInstanceUVs && VertexInstanceUVs.GetNumChannels() > 0)
			{
				const TArrayView<const FVertexInstanceID> TriangleVertexInstanceIDs = MeshDescription->GetTriangleVertexInstances(TriangleID);
				if (TriangleVertexInstanceIDs.Num() == 3)
				{
					Triangle.UVs[0] = VertexInstanceUVs.Get(TriangleVertexInstanceIDs[0], 0);
					Triangle.UVs[1] = VertexInstanceUVs.Get(TriangleVertexInstanceIDs[1], 0);
					Triangle.UVs[2] = VertexInstanceUVs.Get(TriangleVertexInstanceIDs[2], 0);
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

		VertexInstanceUVs.SetNumChannels(1);

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
			if (FVector::DotProduct(PlaneShadingNormal, OrientedNormal) < 0.0)
			{
				OrientedNormal *= -1.0;
				OrientedRho *= -1.0;
				PlaneToShadingNormalDot *= -1.0;
			}

			FVector AxisU = FVector::RightVector;
			FVector AxisV = FVector::UpVector;
			BuildPlaneFrame(OrientedNormal, AxisU, AxisV);

			double MinU = TNumericLimits<double>::Max();
			double MaxU = -TNumericLimits<double>::Max();
			double MinV = TNumericLimits<double>::Max();
			double MaxV = -TNumericLimits<double>::Max();

			if (!ComputeMinimumAreaPlaneRectangle(Triangles, Plane.TriangleIndices, OrientedNormal, OrientedRho, AxisU, AxisV, MinU, MaxU, MinV, MaxV))
			{
				continue;
			}

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

			const FVector PlaneOrigin = OrientedNormal * OrientedRho;
			FVector FrontCorners[4];
			FrontCorners[0] = PlaneOrigin + AxisU * MinU + AxisV * MinV;
			FrontCorners[1] = PlaneOrigin + AxisU * MaxU + AxisV * MinV;
			FrontCorners[2] = PlaneOrigin + AxisU * MaxU + AxisV * MaxV;
			FrontCorners[3] = PlaneOrigin + AxisU * MinU + AxisV * MaxV;

			FPreparedProxyPlane& PreparedPlane = PreparedPlanes.AddDefaulted_GetRef();
			PreparedPlane.SourcePlaneIndex = SourcePlaneIndex;
			PreparedPlane.OrientedNormal = OrientedNormal;
			PreparedPlane.OrientedRho = OrientedRho;
			PreparedPlane.AxisU = AxisU;
			PreparedPlane.AxisV = AxisV;
			PreparedPlane.ShadingNormal = PlaneShadingNormal;
			PreparedPlane.MinU = MinU;
			PreparedPlane.MaxU = MaxU;
			PreparedPlane.MinV = MinV;
			PreparedPlane.MaxV = MaxV;
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
			OutError = TEXT("Could not pack billboard textures into the configured maximum atlas resolution.");
			return false;
		}

		double PlaneToShadingNormalDotSum = 0.0;
		for (const FPreparedProxyPlane& PreparedPlane : PreparedPlanes)
		{
			if (!AddQuadPolygon(OutMeshDescription, PolygonGroupID, VertexPositions, VertexInstanceUVs, VertexInstanceNormals, VertexInstanceTangents, VertexInstanceBinormalSigns, TriangleNormals, TriangleTangents, TriangleBinormals, EdgeHardnesses, PreparedPlane.Corners, PreparedPlane.AtlasUVs, PreparedPlane.ShadingNormal))
			{
				continue;
			}

			if (OutPlaneInfos)
			{
				FPlaneProxyPlaneInfo& PlaneInfo = OutPlaneInfos->AddDefaulted_GetRef();
				PlaneInfo.SourcePlaneIndex = PreparedPlane.SourcePlaneIndex;
				PlaneInfo.Normal = PreparedPlane.OrientedNormal;
				PlaneInfo.Rho = PreparedPlane.OrientedRho;
				PlaneInfo.AxisU = PreparedPlane.AxisU;
				PlaneInfo.AxisV = PreparedPlane.AxisV;
				PlaneInfo.ShadingNormal = PreparedPlane.ShadingNormal;
				PlaneInfo.MinU = PreparedPlane.MinU;
				PlaneInfo.MaxU = PreparedPlane.MaxU;
				PlaneInfo.MinV = PreparedPlane.MinV;
				PlaneInfo.MaxV = PreparedPlane.MaxV;
				PlaneInfo.AtlasPixelMin = PreparedPlane.AtlasPixelMin;
				PlaneInfo.AtlasTileSize = PreparedPlane.AtlasTileSize;
				PlaneInfo.AtlasTileResolution = FMath::Min(PreparedPlane.AtlasTileSize.X, PreparedPlane.AtlasTileSize.Y);
				PlaneInfo.AtlasTilePaddingPixels = PreparedPlane.AtlasTilePaddingPixels;
				PlaneInfo.TriangleIndices = PreparedPlane.TriangleIndices;
				for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
				{
					PlaneInfo.Corners[CornerIndex] = PreparedPlane.Corners[CornerIndex];
					PlaneInfo.AtlasUVs[CornerIndex] = PreparedPlane.AtlasUVs[CornerIndex];
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
		const FString TextureSummary = FString::Printf(
			TEXT("on, object-space variable tiles up to %d px, padding=%d px, max atlas=%d, clipped valid-zone shooting"),
			Settings.TextureTileResolution,
			Settings.TextureTilePaddingPixels,
			Settings.TextureAtlasMaxResolution);

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
