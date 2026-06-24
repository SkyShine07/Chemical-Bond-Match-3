// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ChemicalBondGameMode.generated.h"

class AChemicalBondGameDirector;

DECLARE_LOG_CATEGORY_EXTERN(LogChemicalBondGameMode, Log, All);

// 项目通用 GameMode 基类，负责单机局内生命周期和 GameDirector 创建。
// 关卡差异由蓝图派生类或后续数据配置承担，不通过定制多个 GameMode 实现。
UCLASS(Blueprintable)
class CHEMICALBOND_API AChemicalBondGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AChemicalBondGameMode();

	// 生命周期
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Director API
	UFUNCTION(BlueprintPure, Category="ChemicalBond|Director")
	AChemicalBondGameDirector* GetGameDirector() const;

protected:
	// Director 配置
	// 蓝图配置：Class=ChemicalBondGameMode 派生蓝图，Range=AChemicalBondGameDirector 派生类，
	// Effect=控制每局开始时由 GameMode 生成并持有的规则编排 Actor 类型。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Director")
	TSubclassOf<AChemicalBondGameDirector> GameDirectorClass;

private:
	// Director 生命周期
	void SpawnGameDirector();
	void ShutdownGameDirector();

	// 运行时状态
	UPROPERTY(Transient)
	TObjectPtr<AChemicalBondGameDirector> GameDirector = nullptr;
	
	
};
