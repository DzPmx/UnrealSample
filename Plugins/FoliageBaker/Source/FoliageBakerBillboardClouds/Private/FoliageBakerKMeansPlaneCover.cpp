#include "FoliageBakerKMeansPlaneCover.h"

#include "HAL/PlatformTime.h"

namespace UE::FoliageBaker::BillboardClouds
{
	using namespace UE::FoliageBaker::PlaneCover;

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

	void InitializeKMeansClusters(const TArray<FSourceTriangle>& Triangles, const FKMeansPlaneCoverSettings& Settings, TArray<FKMeansCluster>& OutClusters)
	{
		const int32 ClusterCount = FMath::Clamp(Settings.PlaneCount, 1, FMath::Max(1, Triangles.Num()));
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

	bool AssignTrianglesToKMeansClusters(const TArray<FSourceTriangle>& Triangles, TArray<FKMeansCluster>& Clusters)
	{
		TArray<int32> PreviousClusterByTriangle;
		PreviousClusterByTriangle.Init(INDEX_NONE, Triangles.Num());
		for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
		{
			for (const int32 TriangleIndex : Clusters[ClusterIndex].TriangleIndices)
			{
				if (PreviousClusterByTriangle.IsValidIndex(TriangleIndex))
				{
					PreviousClusterByTriangle[TriangleIndex] = ClusterIndex;
				}
			}
		}

		for (FKMeansCluster& Cluster : Clusters)
		{
			Cluster.TriangleIndices.Reset();
		}

		bool bAssignmentsChanged = false;
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
				bAssignmentsChanged |= PreviousClusterByTriangle[TriangleIndex] != BestClusterIndex;
			}
		}

		return bAssignmentsChanged;
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

	double ComputeKMeansTotalDistance(const TArray<FSourceTriangle>& Triangles, const TArray<FKMeansCluster>& Clusters)
	{
		double TotalDistance = 0.0;
		for (const FKMeansCluster& Cluster : Clusters)
		{
			TotalDistance += ComputeKMeansClusterDistance(Triangles, Cluster);
		}
		return TotalDistance;
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

	void ApplyKMeansCoverageCentroidRelaxation(const TArray<FSourceTriangle>& Triangles, const FKMeansPlaneCoverSettings&, TArray<FKMeansCluster>& Clusters)
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

	double ComputeClusterRadiusVariance(
		const TArray<FSourceTriangle>& Triangles,
		const TArray<FKMeansCluster>& Clusters)
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

	void ApplyKMeansMinimumCoverageClusterReplacement(const TArray<FSourceTriangle>& Triangles, const FKMeansPlaneCoverSettings& Settings, TArray<FKMeansCluster>& Clusters)
	{
		if (Clusters.Num() <= 2)
		{
			return;
		}

		double PreviousRadiusVariance = ComputeClusterRadiusVariance(Triangles, Clusters);
		const int32 MaxIterations = FMath::Clamp(Settings.MaxIterations, 1, 512);

		for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
		{
			TArray<FKMeansCluster> PreviousClusters = Clusters;
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

			const double CurrentRadiusVariance = ComputeClusterRadiusVariance(Triangles, Clusters);
			const double VarianceTolerance = FMath::Max(1.0, FMath::Abs(PreviousRadiusVariance)) * 1.0e-6;
			if (!FMath::IsFinite(CurrentRadiusVariance)
				|| CurrentRadiusVariance >= PreviousRadiusVariance - VarianceTolerance)
			{
				Clusters = MoveTemp(PreviousClusters);
				break;
			}

			PreviousRadiusVariance = CurrentRadiusVariance;
		}
	}

	void InitializeKMeansClustersForSolve(const TArray<FSourceTriangle>& Triangles, const FKMeansPlaneCoverSettings& Settings, TArray<FKMeansCluster>& Clusters)
	{
		InitializeKMeansClusters(Triangles, Settings, Clusters);
		AssignTrianglesToKMeansClusters(Triangles, Clusters);
		FitKMeansClusters(Triangles, Clusters);
		ApplyKMeansCoverageCentroidRelaxation(Triangles, Settings, Clusters);
		ApplyKMeansMinimumCoverageClusterReplacement(Triangles, Settings, Clusters);
	}

	FKMeansPlaneCoverResult BuildKMeansPlaneCover(const TArray<FSourceTriangle>& Triangles, const FKMeansPlaneCoverSettings& Settings)
	{
		const double TotalStartSeconds = FPlatformTime::Seconds();
		FKMeansPlaneCoverResult Result;
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
		InitializeKMeansClustersForSolve(Triangles, Settings, Clusters);
		Result.InitializationSeconds = FPlatformTime::Seconds() - InitializationStartSeconds;

		double PreviousTotalDistance = ComputeKMeansTotalDistance(Triangles, Clusters);
		int32 IterationCount = 0;
		const int32 MaxIterations = FMath::Clamp(Settings.MaxIterations, 1, 512);

		for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
		{
			TArray<FKMeansCluster> PreviousClusters = Clusters;
			const double AssignmentStartSeconds = FPlatformTime::Seconds();
			const bool bAssignmentsChanged = AssignTrianglesToKMeansClusters(Triangles, Clusters);
			Result.AssignmentSeconds += FPlatformTime::Seconds() - AssignmentStartSeconds;

			const double FitStartSeconds = FPlatformTime::Seconds();
			FitKMeansClusters(Triangles, Clusters);
			Result.PlaneRefitSeconds += FPlatformTime::Seconds() - FitStartSeconds;

			const double CurrentTotalDistance = ComputeKMeansTotalDistance(Triangles, Clusters);
			++IterationCount;

			const double DistanceTolerance = FMath::Max(1.0, FMath::Abs(PreviousTotalDistance)) * 1.0e-6;
			if (!FMath::IsFinite(CurrentTotalDistance)
				|| CurrentTotalDistance > PreviousTotalDistance + DistanceTolerance)
			{
				Clusters = MoveTemp(PreviousClusters);
				break;
			}

			const double DistanceImprovement = PreviousTotalDistance - CurrentTotalDistance;
			if (!bAssignmentsChanged || DistanceImprovement <= DistanceTolerance)
			{
				break;
			}

			PreviousTotalDistance = CurrentTotalDistance;
		}

		Result.IterationCount = IterationCount;
		Result.CoveredTriangleCount = 0;
		Result.CoveredArea = 0.0;

		for (const FKMeansCluster& Cluster : Clusters)
		{
			if (Cluster.TriangleIndices.IsEmpty())
			{
				continue;
			}

			FPlaneProxyInput& Plane = Result.Planes.AddDefaulted_GetRef();
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

	FString SummarizeKMeansPlaneCover(
		const FString& MeshName,
		const FKMeansPlaneCoverSettings& KMeansSettings,
		const FPlaneProxySettings& ProxySettings,
		const FKMeansPlaneCoverResult& Result)
	{
		const double CoveredTrianglePercent = Result.SourceTriangleCount > 0
			? 100.0 * static_cast<double>(Result.CoveredTriangleCount) / static_cast<double>(Result.SourceTriangleCount)
			: 0.0;
		const double CoveredAreaPercent = Result.SourceArea > 0.0
			? 100.0 * Result.CoveredArea / Result.SourceArea
			: 0.0;

		FString KMeansCrackReductionSummary = TEXT("off");
		if (ProxySettings.CrackReductionMode == EPlaneProxyCrackReductionMode::ScaledEnvelopeClip)
		{
			KMeansCrackReductionSummary = FString::Printf(
				TEXT("scaled envelope-clipped projections, scale=%.2f"),
				ProxySettings.CrackReductionProjectionScale);
		}

		const TCHAR* DoubleSidedBakeSummary = TEXT("off");
		switch (ProxySettings.DoubleSidedBakeMode)
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

		const FString ResolutionSummary =
			ProxySettings.TextureResolutionMode
				== EFoliageBakerTextureResolutionMode::AutoWorldTexelSize
			? FString::Printf(
				TEXT("auto %.2f texels/m, %d-%d atlas"),
				ProxySettings.TargetTexelsPerMeter,
				ProxySettings.MinimumTextureAtlasResolution,
				ProxySettings.TextureAtlasResolution)
			: FString::Printf(
				TEXT("manual %dx%d atlas"),
				ProxySettings.TextureAtlasResolution,
				ProxySettings.TextureAtlasResolution);
		const FString TextureSummary = FString::Printf(
			TEXT("on, %s, packed object-space tiles, automatic per-tile edge fill, cluster projection, crack reduction=%s, double-sided bake=%s"),
			*ResolutionSummary,
			*KMeansCrackReductionSummary,
			DoubleSidedBakeSummary);

		double TotalKMeansDistance = 0.0;
		for (const FPlaneProxyInput& Plane : Result.Planes)
		{
			TotalKMeansDistance += -Plane.Score;
		}

		FString Summary = FString::Printf(
			TEXT("%s\n  algorithm: billboard clouds K-Means best-fit plane clustering\n  triangles: %d, target planes: %d, output planes: %d\n  iterations: %d, covered: %d / %d (%.1f%%), area: %.1f%%\n  metric: budget-driven triangle-to-plane distance, total distance %.3f\n  initialization: bounding-sphere tangent planes, one-pass coverage-centroid relaxation, count-based minimum-coverage cluster replacement\n  assignment: nearest plane by summed vertex-to-plane distance\n  empty clusters: handled by minimum-coverage replacement, skipped if still empty\n  plane refit: symmetric covariance eigensolve / SVD-equivalent best-fit plane\n  billboard footprint: object-space projection minimum-area rectangle\n  texture atlas: %s\n  timing: total %.2fs, initialization %.2fs, assignment %.2fs, plane refit %.2fs"),
			*MeshName,
			Result.SourceTriangleCount,
			KMeansSettings.PlaneCount,
			Result.Planes.Num(),
			Result.IterationCount,
			Result.CoveredTriangleCount,
			Result.SourceTriangleCount,
			CoveredTrianglePercent,
			CoveredAreaPercent,
			TotalKMeansDistance,
			*TextureSummary,
			Result.TotalSeconds,
			Result.InitializationSeconds,
			Result.AssignmentSeconds,
			Result.PlaneRefitSeconds);

		const int32 PreviewPlaneCount = FMath::Min(5, Result.Planes.Num());
		for (int32 PlaneIndex = 0; PlaneIndex < PreviewPlaneCount; ++PlaneIndex)
		{
			const FPlaneProxyInput& Plane = Result.Planes[PlaneIndex];
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
}
