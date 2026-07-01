// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "../AtomTypes.h"
#include "ChemicalBondMoleculeTypes.generated.h"

class AAtomBase;

// 拓扑结构类型，用于关卡目标配置与成环分类。
UENUM(BlueprintType)
enum class EChemicalBondTopologyType : uint8
{
	Ring4   UMETA(DisplayName = "四元环"),
	Ring5   UMETA(DisplayName = "五元环"),
	Ring6   UMETA(DisplayName = "六元环"),
	Benzene UMETA(DisplayName = "苯环"),
};

// 危害基团类型，仅用于本迭代的存在性识别与效果占位入口。
UENUM(BlueprintType)
enum class EChemicalBondDebuffType : uint8
{
	NitroNO2      UMETA(DisplayName = "-NO2"),
	CyanoCN       UMETA(DisplayName = "-C三N"),
	PeroxideOO    UMETA(DisplayName = "-O-O-"),
	IsocyanateNCO UMETA(DisplayName = "-N=C=O"),
};

// 基础元素，把 11 个元素变体归并为化学元素，便于胜利计数、危害匹配和拓扑判定。
UENUM(BlueprintType)
enum class EChemicalBondBaseElement : uint8
{
	Hydrogen   UMETA(DisplayName = "H"),
	Carbon     UMETA(DisplayName = "C"),
	Oxygen     UMETA(DisplayName = "O"),
	Nitrogen   UMETA(DisplayName = "N"),
	Phosphorus UMETA(DisplayName = "P"),
	Unknown    UMETA(DisplayName = "未知"),
};

namespace ChemicalBondElement
{
	// 把元素变体映射为基础化学元素。
	inline EChemicalBondBaseElement GetBaseElement(EAtomElementType ElementType)
	{
		switch (ElementType)
		{
		case EAtomElementType::H:
		case EAtomElementType::H_Normal:
			return EChemicalBondBaseElement::Hydrogen;
		case EAtomElementType::C_Normal:
		case EAtomElementType::C_Player:
		case EAtomElementType::C_Ring:
			return EChemicalBondBaseElement::Carbon;
		case EAtomElementType::O_Normal:
		case EAtomElementType::O_Ring:
			return EChemicalBondBaseElement::Oxygen;
		case EAtomElementType::N_Normal:
		case EAtomElementType::N_Ring:
			return EChemicalBondBaseElement::Nitrogen;
		case EAtomElementType::P_Normal:
		case EAtomElementType::P_Ring:
			return EChemicalBondBaseElement::Phosphorus;
		default:
			return EChemicalBondBaseElement::Unknown;
		}
	}

	// 把环大小映射为拓扑类型（不含苯环这种带额外约束的特化类型）。
	inline EChemicalBondTopologyType RingSizeToTopology(int32 RingSize)
	{
		switch (RingSize)
		{
		case 4:
			return EChemicalBondTopologyType::Ring4;
		case 5:
			return EChemicalBondTopologyType::Ring5;
		default:
			return EChemicalBondTopologyType::Ring6;
		}
	}
}

// 分子快照中的一条邻接边。
struct FChemicalBondMoleculeEdge
{
	int32 NeighborIndex = INDEX_NONE;
	EBondType BondType = EBondType::Single;
	FGuid BondUid;
};

// 分子快照中的一个原子节点。
struct FChemicalBondMoleculeNode
{
	TWeakObjectPtr<AAtomBase> Atom;
	FGuid AtomUid;
	EAtomElementType ElementType = EAtomElementType::C_Normal;
	EChemicalBondBaseElement BaseElement = EChemicalBondBaseElement::Unknown;
	bool bCanFormRing = false;
	TArray<FChemicalBondMoleculeEdge> Edges;
};

// 玩家本体分子团的一次性结构快照，构建后供成环、胜利、危害三遍分析共享。
struct FChemicalBondMoleculeSnapshot
{
	TArray<FChemicalBondMoleculeNode> Nodes;
	TMap<FGuid, int32> UidToIndex;
	TMap<EChemicalBondBaseElement, int32> ElementCounts;
	int32 SingleBondCount = 0;
	int32 DoubleBondCount = 0;
	int32 TripleBondCount = 0;

	int32 NumAtoms() const { return Nodes.Num(); }

	int32 IndexOfUid(const FGuid& Uid) const
	{
		const int32* Found = UidToIndex.Find(Uid);
		return Found ? *Found : INDEX_NONE;
	}

	int32 GetElementCount(EChemicalBondBaseElement Element) const
	{
		const int32* Found = ElementCounts.Find(Element);
		return Found ? *Found : 0;
	}

	int32 GetBondCount(EBondType BondType) const
	{
		switch (BondType)
		{
		case EBondType::Single:
			return SingleBondCount;
		case EBondType::Double:
			return DoubleBondCount;
		case EBondType::Triple:
			return TripleBondCount;
		default:
			return 0;
		}
	}
};

// 一个已闭合的环，节点按环序排列。
struct FChemicalBondRing
{
	TArray<int32> NodeIndices;
	int32 Size = 0;
	EChemicalBondTopologyType Topology = EChemicalBondTopologyType::Ring6;
	bool bAromatic = false;
};

// 一个成环候选：成环原子可与目标原子闭合一条新键，从而形成 RingSize 元环。
struct FChemicalBondRingCandidate
{
	FGuid RingAtomUid;
	FGuid TargetAtomUid;
	int32 RingSize = 0;
	// 从成环原子到目标原子的既有链路（含两端），闭合后即为环的节点序。
	TArray<FGuid> PathAtomUids;

	bool IsSameCandidate(const FChemicalBondRingCandidate& Other) const
	{
		return RingAtomUid == Other.RingAtomUid
			&& TargetAtomUid == Other.TargetAtomUid
			&& RingSize == Other.RingSize;
	}
};

// 一个被识别到的危害基团。
struct FChemicalBondDebuffMatch
{
	EChemicalBondDebuffType DebuffType = EChemicalBondDebuffType::NitroNO2;
	TArray<FGuid> MemberAtomUids;
};
