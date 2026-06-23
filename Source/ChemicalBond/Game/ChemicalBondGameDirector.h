// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../AtomTypes.h"
#include "ChemicalBondGameDirector.generated.h"

class AAtomBase;
class AChemicalBondGameMode;

DECLARE_LOG_CATEGORY_EXTERN(LogChemicalBondDirector, Log, All);

USTRUCT(BlueprintType)
struct FChemicalBondRegistryRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	FGuid BondUid;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	EBondType BondType = EBondType::Single;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	FGuid AtomAUid;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	FGuid AtomBUid;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	TWeakObjectPtr<AAtomBase> AtomA;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	TWeakObjectPtr<AAtomBase> AtomB;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	int32 AtomASlotIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Registry")
	int32 AtomBSlotIndex = INDEX_NONE;
};

// 单局规则编排入口，负责局内系统的生命周期调度和后续业务系统接入。
// 同时维护场上原子和化学键 UID 注册表；玩法状态机和交互规则逐项接入。
UCLASS(Blueprintable)
class CHEMICALBOND_API AChemicalBondGameDirector : public AActor
{
	GENERATED_BODY()

public:
	AChemicalBondGameDirector();

	// 生命周期
	virtual void Tick(float DeltaSeconds) override;

	// Director API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Director")
	void InitializeDirector(AChemicalBondGameMode* InOwningGameMode);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Director")
	void StartDirector();

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Director")
	void StopDirector();

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Director")
	bool IsDirectorStarted() const;

	// 原子注册表 API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Registry")
	FGuid SpawnAtom(AAtomBase* Atom);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Registry")
	bool TerminateAtom(AAtomBase* Atom);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Registry")
	bool TerminateAtomByUid(FGuid AtomUid);

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	bool IsAtomRegistered(FGuid AtomUid) const;

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	AAtomBase* GetAtomByUid(FGuid AtomUid) const;

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	TArray<FGuid> GetAllAtomUids() const;

	// 化学键注册表 API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Registry")
	FGuid LinkAtoms(AAtomBase* AtomA, AAtomBase* AtomB, EBondType BondType, int32 AtomASlotIndex, int32 AtomBSlotIndex);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Registry")
	bool CutBond(FGuid BondUid);

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	bool IsBondRegistered(FGuid BondUid) const;

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	FChemicalBondRegistryRecord GetBondRecord(FGuid BondUid, bool& bFound) const;

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	TArray<FGuid> GetBondUidsByType(EBondType BondType) const;

	UFUNCTION(BlueprintPure, Category="ChemicalBond|Registry")
	TArray<FGuid> GetAllBondUids() const;

protected:
	// 生命周期
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 运行时状态
	UPROPERTY(Transient)
	TObjectPtr<AChemicalBondGameMode> OwningGameMode = nullptr;

	UPROPERTY(Transient)
	bool bDirectorStarted = false;

	UPROPERTY(Transient)
	TMap<FGuid, TWeakObjectPtr<AAtomBase>> AtomRegistry;

	UPROPERTY(Transient)
	TMap<FGuid, FChemicalBondRegistryRecord> BondRegistry;

	UPROPERTY(Transient)
	TArray<FGuid> SingleBondUids;

	UPROPERTY(Transient)
	TArray<FGuid> DoubleBondUids;

	UPROPERTY(Transient)
	TArray<FGuid> TripleBondUids;

	FGuid GenerateUniqueAtomUid() const;
	FGuid GenerateUniqueBondUid() const;
	void RegisterExistingAtomsInWorld();
	void AddBondUidToTypeList(FGuid BondUid, EBondType BondType);
	void RemoveBondUidFromTypeList(FGuid BondUid, EBondType BondType);
	TArray<FGuid> GetAtomBondUidsForTermination(FGuid AtomUid, AAtomBase* Atom) const;
};
