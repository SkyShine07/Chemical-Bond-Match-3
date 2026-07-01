// Fill out your copyright notice in the Description page of Project Settings.

#include "ChemicalBondLevelGoal.h"

namespace
{
	// 统计闭合环集合中某拓扑类型的数量。苯环同时计入六元环需求。
	int32 CountTopology(const TArray<FChemicalBondRing>& ClosedRings, EChemicalBondTopologyType TopologyType)
	{
		int32 Count = 0;
		for (const FChemicalBondRing& Ring : ClosedRings)
		{
			if (Ring.Topology == TopologyType)
			{
				++Count;
			}
			else if (TopologyType == EChemicalBondTopologyType::Ring6
				&& Ring.Topology == EChemicalBondTopologyType::Benzene)
			{
				// 苯环本质上是六元环，满足六元环需求。
				++Count;
			}
		}
		return Count;
	}
}

bool UChemicalBondLevelGoalAsset::EvaluateAgainst(
	const FChemicalBondMoleculeSnapshot& Snapshot,
	const TArray<FChemicalBondRing>& ClosedRings,
	FString& OutUnmetReason) const
{
	OutUnmetReason.Reset();

	for (const FChemicalBondAtomRequirement& Requirement : AtomRequirements)
	{
		const int32 Actual = Snapshot.GetElementCount(Requirement.Element);
		if (Actual < Requirement.MinCount)
		{
			OutUnmetReason = FString::Printf(
				TEXT("Atom requirement unmet. Element=%d Need=%d Have=%d"),
				static_cast<int32>(Requirement.Element),
				Requirement.MinCount,
				Actual);
			return false;
		}
	}

	for (const FChemicalBondBondRequirement& Requirement : BondRequirements)
	{
		const int32 Actual = Snapshot.GetBondCount(Requirement.BondType);
		if (Actual < Requirement.MinCount)
		{
			OutUnmetReason = FString::Printf(
				TEXT("Bond requirement unmet. BondType=%d Need=%d Have=%d"),
				static_cast<int32>(Requirement.BondType),
				Requirement.MinCount,
				Actual);
			return false;
		}
	}

	for (const FChemicalBondTopologyRequirement& Requirement : TopologyRequirements)
	{
		const int32 Actual = CountTopology(ClosedRings, Requirement.TopologyType);
		if (Actual < Requirement.MinCount)
		{
			OutUnmetReason = FString::Printf(
				TEXT("Topology requirement unmet. Topology=%d Need=%d Have=%d"),
				static_cast<int32>(Requirement.TopologyType),
				Requirement.MinCount,
				Actual);
			return false;
		}
	}

	return true;
}
