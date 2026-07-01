// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "../AtomTypes.h"
#include "ChemicalBondMoleculeTypes.h"
#include "ChemicalBondGameDirector.generated.h"

class AAtomBase;
class AChemicalBondGameMode;
class UNiagaraComponent;
class UNiagaraSystem;
class UChemicalBondLevelGoalAsset;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;

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

USTRUCT(BlueprintType)
struct FAtomInteractionPairKey
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Connection")
	FGuid FirstAtomUid;

	UPROPERTY(BlueprintReadOnly, Category="ChemicalBond|Connection")
	FGuid SecondAtomUid;

	bool IsValid() const
	{
		return FirstAtomUid.IsValid() && SecondAtomUid.IsValid() && FirstAtomUid != SecondAtomUid;
	}

	bool operator==(const FAtomInteractionPairKey& Other) const
	{
		return FirstAtomUid == Other.FirstAtomUid && SecondAtomUid == Other.SecondAtomUid;
	}
};

FORCEINLINE uint32 GetTypeHash(const FAtomInteractionPairKey& Key)
{
	return HashCombine(GetTypeHash(Key.FirstAtomUid), GetTypeHash(Key.SecondAtomUid));
}

USTRUCT()
struct FAtomConnectionCandidate
{
	GENERATED_BODY()

	FAtomInteractionPairKey PairKey;
	TWeakObjectPtr<AAtomBase> AtomA;
	TWeakObjectPtr<AAtomBase> AtomB;
	float RemainingConfirmationSeconds = 0.f;
};

USTRUCT()
struct FAtomDecisionRequest
{
	GENERATED_BODY()

	FAtomInteractionPairKey PairKey;
	TWeakObjectPtr<AAtomBase> ConnectedAtom;
	TWeakObjectPtr<AAtomBase> FreeAtom;
	float RemainingDecisionSeconds = 0.f;
	FGuid BondUid;
	bool bHasFormedBond = false;
};

USTRUCT()
struct FBondVisualComponentList
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> Components;
};

// 单局规则编排入口，负责局内系统生命周期、原子/化学键注册表、连接决策队列和临时基团物理连接。
// 后续分子图、危害、胜负和正式物理约束在设计确认后继续接入。
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

	// 连接决策 API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Connection")
	void HandleAtomProximityEnter(AAtomBase* AtomA, AAtomBase* AtomB);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Connection")
	void HandleAtomProximityExit(AAtomBase* AtomA, AAtomBase* AtomB);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Connection")
	bool HandleDecisionConfirmInput();

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Connection")
	bool HandleDecisionRejectInput();

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|Connection")
	void MarkAtomAsPlayerConnected(AAtomBase* Atom);

	bool ValidateBondRegistryConsistency(FString& OutError) const;
	void AssertBondRegistryConsistency();

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

	// 策划配置
	// 蓝图配置：Class=GameDirector 派生类，Range=0.0..10.0 秒，
	// Effect=交互范围交叉后等待多久再结算连接条件；0 表示立刻结算。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Connection", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float ContactConfirmationSeconds = 0.2f;

	// 蓝图配置：Class=GameDirector 派生类，Range=0.0..30.0 秒，
	// Effect=玩家处理待决策连接的基准窗口，后续温度系统可在此基础上修正。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Connection", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float DecisionWindowSeconds = 2.f;

	// 蓝图配置：Class=GameDirector 派生类，Range=0.0..100000.0，
	// Effect=轻轻推离的基础冲量，质量系数会在此基础上影响推离强度。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Connection", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float GentleRepulsionImpulse = 1800.f;

	// 蓝图配置：Class=GameDirector 派生类，Range=0.0..1000.0，
	// Effect=原子量对轻轻推离冲量的影响系数。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Connection", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float GentleRepulsionMassScale = 8.f;

	// 蓝图配置：Class=GameDirector 派生类，Range=0.0..10.0 秒，
	// Effect=轻轻推离后两个原子忽略交互结算的时间窗口。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Connection", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float InteractionCooldownSeconds = 0.35f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true"))
	TSubclassOf<AAtomBase> RefreshAtomClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float RefreshIntervalMin = 1.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float RefreshIntervalMax = 3.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float GuideIntervalMin = 8.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float GuideIntervalMax = 14.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="1"))
	int32 TargetFreeAtomCount = 40;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="1"))
	int32 SpawnPlacementAttempts = 10;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="1.0"))
	float RefreshBaseCount = 1.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="1.0"))
	float MainGuideRefreshCoefficient = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="1.0"))
	float SubGuideRefreshCoefficient = 0.7f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="1.0"))
	float WeakGuideRefreshCoefficient = 0.35f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="1.0"))
	float NoneGuideRefreshCoefficient = 0.1f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="1.0"))
	float DirectedSpawnRate = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float RandomSpawnImpulseMin = 7000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float RandomSpawnImpulseMax = 11000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float DirectedSpawnImpulseMin = 14000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Refresh", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float DirectedSpawnImpulseMax = 22000.f;

	UPROPERTY(Transient)
	float RefreshTimeRemaining = 0.f;

	UPROPERTY(Transient)
	float GuideTimeRemaining = 0.f;

	UPROPERTY(Transient)
	int32 CurrentMainGuideRegion = INDEX_NONE;

	UPROPERTY(Transient)
	TMap<FAtomInteractionPairKey, FAtomConnectionCandidate> ConnectionCandidates;

	UPROPERTY(Transient)
	TArray<FAtomDecisionRequest> DecisionQueue;

	UPROPERTY(Transient)
	FAtomDecisionRequest ActiveDecisionRequest;

	UPROPERTY(Transient)
	bool bHasActiveDecisionRequest = false;

	UPROPERTY(Transient)
	TSet<FAtomInteractionPairKey> QueuedDecisionPairKeys;

	UPROPERTY(Transient)
	TSet<FGuid> LockedAtomUids;

	TMap<FGuid, TMap<FGuid, FTransform>> RigidGroupLocalTransforms;

	UPROPERTY(Transient)
	TMap<FGuid, FBondVisualComponentList> BondVisualComponents;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UProceduralMeshComponent> InteractionRangeFillMesh = nullptr;

	// 蓝图配置：Class=GameDirector 派生类，Range=NiagaraSystem 资源，
	// Effect=化学键成键时生成的线状特效，参数 start/end 使用槽位中心世界坐标。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UNiagaraSystem> BondVisualSystem = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UMaterialInterface> InteractionRangeFillMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> InteractionRangeFillMaterialInstance = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	FLinearColor InteractionRangeFillColor = FLinearColor(0.18f, 0.75f, 0.95f, 0.45f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	FLinearColor FreeAtomInteractionRangeColor = FLinearColor(0.55f, 0.58f, 0.6f, 0.45f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	FLinearColor UnavailableInteractionRangeColor = FLinearColor(1.f, 0.18f, 0.1f, 0.45f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true", ClampMin="2.0"))
	float InteractionRangeFillBoundarySegmentLength = 8.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true", ClampMin="0.0"))
	float InteractionRangeBoundaryThickness = 8.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	float InteractionRangeFillZOffset = 2.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	bool bEnableInteractionRangeFillVisual = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	bool bEnableInteractionRangeBoundaryVisual = true;

	UPROPERTY(Transient)
	bool bLoggedInteractionRangeFillVisual = false;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> ActiveDecisionWarningComponent = nullptr;

	UPROPERTY(Transient)
	float LastDecisionWarningLogTime = -1000.f;

	UPROPERTY(Transient)
	bool bDecisionWarningVisualConfigured = false;

	UPROPERTY(Transient)
	bool bLoggedDecisionWarningParameters = false;

	// 蓝图配置：Class=GameDirector 派生类，Range=NiagaraSystem 资源，
	// Effect=玩家基团参与待决策时，在已连接原子中心生成的倒计时警示特效。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UNiagaraSystem> DecisionWarningVisualSystem = nullptr;

	// 蓝图配置：Class=GameDirector 派生类，Range=0.01..10.0，
	// Effect=把已连接原子的交互半径换算成写入 NS_Warning 的 User.Radiu 数值；默认 2.0 表示按直径传入。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float DecisionWarningRadiusParameterScale = 2.f;

	// 蓝图配置：Class=GameDirector 派生类，Range=本关 UChemicalBondLevelGoalAsset 资产，
	// Effect=本关通关目标（目标原子/键型/拓扑需求）；为空则不进行胜利判定。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Victory", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UChemicalBondLevelGoalAsset> LevelGoalAsset = nullptr;

	// 蓝图配置：Class=GameDirector 派生类，Range=10.0..1000.0，
	// Effect=成环后正多边形的边长（相邻环原子中心间距），默认与临时连接距离一致。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Ring", meta=(AllowPrivateAccess="true", ClampMin="10.0"))
	float RingPolygonEdgeLength = 90.f;

	// 成环候选队列（LIFO）：每个候选是一条可闭合的成环可能性。
	TArray<FChemicalBondRingCandidate> RingDecisionQueue;

	UPROPERTY(Transient)
	bool bHasActiveRingDecision = false;

	// 当前激活的成环候选与其闭合键。
	FChemicalBondRingCandidate ActiveRingCandidate;

	UPROPERTY(Transient)
	FGuid ActiveRingClosingBondUid;

	// 被玩家拒绝全部成环可能性而降级的成环原子 UID；后续检测不再为它们生成成环候选。
	UPROPERTY(Transient)
	TSet<FGuid> DemotedRingAtomUids;

	// 已完成至少一次成环闭合的成环原子 UID；后续检测不再为它们生成成环候选。
	UPROPERTY(Transient)
	TSet<FGuid> CompletedRingAtomUids;

	// 玩家本体分子结构脏标记，在获得/失去原子后置位，于 Tick 末尾合并结算一次检测。
	UPROPERTY(Transient)
	bool bPlayerMoleculeDirty = false;

	// 已经上报过通关，避免重复判定与重复日志。
	UPROPERTY(Transient)
	bool bVictoryReported = false;

	FGuid GenerateUniqueAtomUid() const;
	FGuid GenerateUniqueBondUid() const;
	void RegisterExistingAtomsInWorld();
	void AddBondUidToTypeList(FGuid BondUid, EBondType BondType);
	void RemoveBondUidFromTypeList(FGuid BondUid, EBondType BondType);
	TArray<FGuid> GetAtomBondUidsForTermination(FGuid AtomUid, AAtomBase* Atom) const;
	int32 PruneStaleAtomRegistryEntries();
	bool DoesBondRegistryReferenceAtom(FGuid AtomUid) const;
	void ClearTransientReferencesForAtom(FGuid AtomUid);
	bool DoesTypeListContainBond(FGuid BondUid, EBondType BondType) const;
	bool DoesOtherTypeListContainBond(FGuid BondUid, EBondType BondType) const;
	void RebuildRegistriesFromAtomData();
	void HandleBondRegistryMismatch(const FString& ErrorMessage);
	void BindAtomConnectionEvents(AAtomBase* Atom);
	FAtomInteractionPairKey MakePairKey(AAtomBase* AtomA, AAtomBase* AtomB);
	void ProcessConnectionCandidates(float DeltaSeconds);
	void ProcessActiveDecision(float DeltaSeconds);
	void ProcessPhysicalConnections(float DeltaSeconds);
	void ConstrainRegisteredAtomsToGameplayPlane() const;
	void ProcessPlayerRigidGroups(TSet<FAtomInteractionPairKey>& OutHandledPairKeys);
	void CollectBondedGroup(AAtomBase* RootAtom, TArray<AAtomBase*>& OutAtoms, TSet<FAtomInteractionPairKey>& OutPairKeys);
	void AddConnectedNeighbor(
		AAtomBase* SourceAtom,
		AAtomBase* NeighborAtom,
		TArray<AAtomBase*>& PendingAtoms,
		TSet<FGuid>& VisitedAtomUids,
		TSet<FAtomInteractionPairKey>& OutPairKeys);
	bool ApplyPairConstraintIfUnhandled(
		AAtomBase* AtomA,
		AAtomBase* AtomB,
		const TSet<FAtomInteractionPairKey>& HandledPairKeys,
		const TCHAR* Reason);
	void ApplyConnectionPullConstraint(AAtomBase* AnchorAtom, AAtomBase* PulledAtom, const TCHAR* Reason);
	bool FindClosestFreeSlotPair(AAtomBase* AtomA, AAtomBase* AtomB, int32& OutAtomASlot, int32& OutAtomBSlot) const;
	void AlignAtomsForSlotConnection(AAtomBase* AtomA, int32 AtomASlot, AAtomBase* AtomB, int32 AtomBSlot, const TCHAR* Reason);
	void RefreshAtomBondLayouts(AAtomBase* AtomA, AAtomBase* AtomB) const;
	void UpdateInteractionRangeFillVisual();
	void SpawnOrUpdateBondVisual(FGuid BondUid);
	void UpdateAllBondVisuals();
	void DestroyBondVisual(FGuid BondUid);
	void ConfigureDecisionWarningVisualSystem();
	void LogDecisionWarningVisualParametersOnce();
	void SetDecisionWarningVisualParameters(float WarningLifetime, float WarningRadius);
	void SpawnOrUpdateActiveDecisionWarningVisual();
	void DestroyActiveDecisionWarningVisual();
	void SettleConnectionCandidate(const FAtomConnectionCandidate& Candidate);
	void EnqueueDecisionRequest(AAtomBase* ConnectedAtom, AAtomBase* FreeAtom, const FAtomInteractionPairKey& PairKey);
	void StartNextDecisionRequest();
	void FinishActiveDecision(bool bRejected);
	bool TryAdvanceActiveDecisionBond();
	bool ChangeBondType(FGuid BondUid, EBondType NewBondType, int32 AtomAAdditionalSlot, int32 AtomBAdditionalSlot);
	bool FindExistingBondBetween(AAtomBase* AtomA, AAtomBase* AtomB, FGuid& OutBondUid, EBondType& OutBondType) const;
	void SetAtomGroupState(AAtomBase* RootAtom, EAtomState NewState);
	void ApplyFixedConnectionConstraint(AAtomBase* AtomA, AAtomBase* AtomB, const TCHAR* Reason);
	AAtomBase* ChooseConnectionAnchor(AAtomBase* AtomA, AAtomBase* AtomB) const;
	void StopConstrainedAtomMotion(AAtomBase* Atom) const;
	void ApplyGentleRepulsion(AAtomBase* AtomA, AAtomBase* AtomB, const TCHAR* Reason);
	bool CanAtomsStartConnection(AAtomBase* AtomA, AAtomBase* AtomB, FString& OutReason) const;
	bool IsAtomLocked(AAtomBase* Atom) const;
	void LockAtom(AAtomBase* Atom);
	void UnlockAtom(AAtomBase* Atom);
	void ReleaseDecisionPair(const FAtomInteractionPairKey& PairKey);
	void InitializeRefreshRuntime();
	void ProcessSceneRefresh(float DeltaSeconds);
	void ProcessRefreshGuide(float DeltaSeconds);
	void ProcessLifeSpanRecycling();
	void ExecuteGlobalRefresh();
	bool TrySpawnRefreshAtomInRegion(int32 RegionIndex);
	EAtomElementType PickRefreshElementType() const;
	void ApplySpawnInitialImpulse(AAtomBase* SpawnedAtom, const FVector& PlayerLocation);
	int32 CountFreeAtomsInLogicRange() const;
	int32 CountFreeAtomsInRegion(int32 RegionIndex) const;
	float GetRefreshCoefficientForRegion(int32 RegionIndex) const;
	int32 GetPreviousGuideRegion(int32 RegionIndex) const;
	int32 GetNextGuideRegion(int32 RegionIndex) const;
	int32 GetOppositeGuideRegion(int32 RegionIndex) const;
	bool GetRefreshRegionBounds(int32 RegionIndex, FVector2D& OutMin, FVector2D& OutMax) const;
	bool IsWorldLocationInsideRefreshHalfExtent(const FVector& WorldLocation, const FVector2D& HalfExtent) const;
	bool IsSpawnLocationLegal(const FVector& SpawnLocation, float SpawnProximityRadius) const;

	// 分子结构统一检测：成环 -> 胜利 -> 危害
	void MarkPlayerMoleculeDirty();
	void ProcessPlayerMoleculeDetection();
	void OnPlayerMoleculeChanged();
	AAtomBase* FindPlayerAnchorAtom() const;
	bool BuildPlayerMoleculeSnapshot(FChemicalBondMoleculeSnapshot& OutSnapshot) const;

	// 成环分析与成环决策
	void AnalyzeRings(
		const FChemicalBondMoleculeSnapshot& Snapshot,
		TArray<FChemicalBondRing>& OutClosedRings,
		TArray<FChemicalBondRingCandidate>& OutCandidates) const;
	void EnqueueRingCandidates(const TArray<FChemicalBondRingCandidate>& Candidates);
	bool IsRingCandidateQueuedOrActive(const FChemicalBondRingCandidate& Candidate) const;
	void StartNextRingDecision();
	bool HandleRingDecisionConfirmInput();
	bool HandleRingDecisionRejectInput();
	void FinishActiveRingDecision(bool bRejected);
	bool IsActiveRingCandidateStillValid() const;
	void ArrangeRingAsRegularPolygon(const TArray<AAtomBase*>& RingAtoms);
	void RecapturePlayerGroupLayoutFromCurrent(AAtomBase* AnchorAtom);

	// 胜利判定
	bool EvaluateVictory(
		const FChemicalBondMoleculeSnapshot& Snapshot,
		const TArray<FChemicalBondRing>& ClosedRings) const;
	void ReportVictory();

	// 危害扫描
	void DetectDebuffGroups(
		const FChemicalBondMoleculeSnapshot& Snapshot,
		TArray<FChemicalBondDebuffMatch>& OutMatches) const;
	void ExecuteDebuff(const FChemicalBondDebuffMatch& Match);
};
