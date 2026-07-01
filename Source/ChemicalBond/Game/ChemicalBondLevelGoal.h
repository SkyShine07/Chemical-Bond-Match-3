// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../AtomTypes.h"
#include "ChemicalBondMoleculeTypes.h"
#include "ChemicalBondLevelGoal.generated.h"

// 目标原子需求：某基础元素至少需要的数量。
USTRUCT(BlueprintType)
struct FChemicalBondAtomRequirement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "元素"))
	EChemicalBondBaseElement Element = EChemicalBondBaseElement::Carbon;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "最小数量", ClampMin = "1"))
	int32 MinCount = 1;
};

// 目标键型需求：某键型至少需要的数量。
USTRUCT(BlueprintType)
struct FChemicalBondBondRequirement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "键型"))
	EBondType BondType = EBondType::Double;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "最小数量", ClampMin = "1"))
	int32 MinCount = 1;
};

// 目标拓扑需求：某拓扑结构至少需要的数量。
USTRUCT(BlueprintType)
struct FChemicalBondTopologyRequirement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "拓扑结构"))
	EChemicalBondTopologyType TopologyType = EChemicalBondTopologyType::Benzene;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "目标", meta = (DisplayName = "最小数量", ClampMin = "1"))
	int32 MinCount = 1;
};

// 关卡通关目标 Data Asset。策划每关创建一个资产实例，配置目标原子、键型和拓扑需求。
// 判定语义统一为“玩家本体实际数量 ≥ 需求数量”。
UCLASS(BlueprintType)
class CHEMICALBOND_API UChemicalBondLevelGoalAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// 关卡显示名，供 UI 后续读取。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "关卡目标", meta = (DisplayName = "关卡名称"))
	FText LevelDisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "关卡目标", meta = (DisplayName = "目标原子需求", TitleProperty = "Element"))
	TArray<FChemicalBondAtomRequirement> AtomRequirements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "关卡目标", meta = (DisplayName = "目标键型需求", TitleProperty = "BondType"))
	TArray<FChemicalBondBondRequirement> BondRequirements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "关卡目标", meta = (DisplayName = "目标拓扑需求", TitleProperty = "TopologyType"))
	TArray<FChemicalBondTopologyRequirement> TopologyRequirements;

	// 判定快照与闭合环集合是否满足全部目标需求。
	// OutUnmetReason 在不满足时给出第一条未达成的需求描述，便于日志与调试。
	bool EvaluateAgainst(
		const FChemicalBondMoleculeSnapshot& Snapshot,
		const TArray<FChemicalBondRing>& ClosedRings,
		FString& OutUnmetReason) const;
};
