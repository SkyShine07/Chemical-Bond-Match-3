// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HLSLTypeAliases.h"
#include "GameFramework/Actor.h"
#include "../AtomTypes.h"
#include "ChemicalBondGameDirector.generated.h"

class APlaygroundAtom;
class UCameraComponent;
class AAtomBase;
class AChemicalBondGameMode;
class UNiagaraComponent;
class UNiagaraSystem;

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

USTRUCT(BlueprintType)
struct FRefreshRegionInfo
{
	GENERATED_BODY()

	// 区域范围
	UPROPERTY(BlueprintReadOnly)
	FVector MinRange;
	
	UPROPERTY(BlueprintReadOnly)
	FVector MaxRange;
	
	// 相关区域
	UPROPERTY(BlueprintReadOnly)
	TArray<uint8> SubGuideRegionIndex;
	
	UPROPERTY(BlueprintReadOnly)
	TArray<uint8> WeakGuideRegionIndex;
	
	UPROPERTY(BlueprintReadOnly)
	TArray<uint8> NonGuideRegionIndex;
	
	UPROPERTY(BlueprintReadOnly)
	uint8 CentrallySymmetricRegionIndex;
	
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRegionRefreshed,FRefreshRegionInfo,MainGuideRegion);

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
	
	// 计算8个刷新区域
	
	// 计算三个区域范围：生存，逻辑，可视范围
	UFUNCTION(BlueprintCallable,BlueprintPure, Category="BoxRange")
	 FVector GetViewBoxRange();

	UFUNCTION(BlueprintCallable, BlueprintPure,Category="BoxRange")
	FVector GetLogicRegionBoxRange();

	UFUNCTION(BlueprintCallable, BlueprintPure,Category="BoxRange")
	FVector GetAtomLifeRegionBoxRange();
	
	
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	TArray<FRefreshRegionInfo> GetAllGridRegionsInfoAtAtomLifeRegion(float Scale=1.f);
	
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	 int32 GetNextMainGuideRegionIndex(const TArray<FRefreshRegionInfo>& RefreshRegionInfos,uint8 LocalCurrentMainGuideIndex);
	
	UFUNCTION(BlueprintCallable,BlueprintPure, Category="BoxRange")
	FRefreshRegionInfo GetCurrentMainGuideRegionInfo()  ;
	
	// 刷新所有区域
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	void RefreshRegions(bool bIsRandom );
	
	// 生成8个区域信息
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	static TArray<FRefreshRegionInfo> Get_8_Regions(FVector Range);
	
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	static uint8 FindRegionIndexByLocation(FVector TargetLocation,  const TArray<FRefreshRegionInfo>& RefreshRegionInfos);
	
	
	// ******刷新区域的功能调用入口 *********
	UFUNCTION(BlueprintCallable, Category="BoxRange")
	void StartRefreshRegion();
	
	

	
	// 原子刷新逻辑
	
	UFUNCTION(BlueprintCallable, Category="AtomSpawn")
	int32 GetSpawnAtomNumInRegion(float RefreshFrequency);
	
	UFUNCTION(BlueprintCallable, Category="AtomSpawn")
	EAtomElementType GetSpawnAtomTypeInRegion();
	
	// TODO:检测随机位置是否合法
	UFUNCTION(Blueprintable, Category="AtomSpawn")
	bool  CanSpawnAtomAtPostion(FVector Location);
	
	UFUNCTION(Blueprintable, Category="AtomSpawn")
	FVector  FindSpawnAtomPostionOffset(uint8 regionIndex,uint8 FindNum=10);
	
	UFUNCTION(Blueprintable, Category="AtomSpawn")
	void SpawnAtoms(int32 SpawnNum,UClass* AtomClass,int32 RegionIndex);
	
	UFUNCTION(Blueprintable, Category="AtomSpawn")
	TArray<APlaygroundAtom*> GetAllAtomsOutLifeRange();
	
	
	 // * **  原子刷新逻辑入口 **
	UFUNCTION(Blueprintable, Category="AtomSpawn")
	void SpawnAtomInAllRegions();
	
	bool ValidateBondRegistryConsistency(FString& OutError) const;
	void AssertBondRegistryConsistency();

	// 当区域刷新时广播
	UPROPERTY(BlueprintAssignable)
	FOnRegionRefreshed OnRegionRefreshed;
	
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

	// 蓝图配置：Class=GameDirector 派生类，Range=NiagaraSystem 资源，
	// Effect=化学键成键时生成的线状特效，参数 start/end 使用槽位中心世界坐标。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Presentation", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UNiagaraSystem> BondVisualSystem = nullptr;

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


	// 刷新逻辑参数
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float LogicRegionBoxScale = 1.2f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float AtomLifeRegionBoxScale = 2.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float Tmin = 0.8f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float Tmax =1.5f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float MainGuideRefreshFrequency =1.2f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float SubGuideRefreshFrequency =0.9f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float  WeakGuideRefreshFrequency =0.7f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true", ClampMin="0.01"))
	float   NoneGuideRefreshFrequency =0.6f;
	
	// 划分8个刷新区域
	UPROPERTY(BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 CurrentMainGuideIndex=0;
	
	UPROPERTY(BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	TArray<FRefreshRegionInfo>  RefreshRegionInfos;

	
	//原子生成权重参数
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	float Cbase=3;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wnormal=100;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wring=10;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wc=50;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wh=100;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wp=10;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wn=20;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="BoxRange", meta=(AllowPrivateAccess="true"))
	int32 Wo=30;
	

	
	
	
	FGuid GenerateUniqueAtomUid() const;
	FGuid GenerateUniqueBondUid() const;
	void RegisterExistingAtomsInWorld();
	void AddBondUidToTypeList(FGuid BondUid, EBondType BondType);
	void RemoveBondUidFromTypeList(FGuid BondUid, EBondType BondType);
	TArray<FGuid> GetAtomBondUidsForTermination(FGuid AtomUid, AAtomBase* Atom) const;
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
};
