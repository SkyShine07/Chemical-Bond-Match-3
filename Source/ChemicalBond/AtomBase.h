#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Components/SphereComponent.h"
#include "AtomTypes.h"
#include "AtomBase.generated.h"

class AAtomBase;
class UFluidMotionComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

// 存在原子上的单条键记录
USTRUCT(BlueprintType)
struct FBondRecord
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "键")
    FGuid BondUid;

    UPROPERTY(BlueprintReadOnly, Category = "键")
    TWeakObjectPtr<AAtomBase> PartnerAtom;

    UPROPERTY(BlueprintReadOnly, Category = "键")
    EBondType BondType = EBondType::Single;

    // 本原子占用的槽位索引
    UPROPERTY(BlueprintReadOnly, Category = "键")
    int32 MySlotIndex = INDEX_NONE;

    // 对方原子占用的槽位索引
    UPROPERTY(BlueprintReadOnly, Category = "键")
    int32 PartnerSlotIndex = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category = "键")
    TArray<int32> MySlotIndices;

    UPROPERTY(BlueprintReadOnly, Category = "键")
    TArray<int32> PartnerSlotIndices;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAtomProximityEnter, AAtomBase*, SourceAtom, AAtomBase*, OtherAtom);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAtomProximityExit, AAtomBase*, SourceAtom, AAtomBase*, OtherAtom);

// 原子 Pawn 基类，维护槽位、键记录、状态机、交互检测和底部交互范围指示盘。
UCLASS(Abstract)
class CHEMICALBOND_API AAtomBase : public APawn
{
    GENERATED_BODY()

public:
    AAtomBase();
    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void ApplyRuntimeAtomData(EAtomElementType InElementType, float InMass, int32 InTotalSlots, bool bInCanFormRing);
    void SetInitialAtomState(EAtomState NewState);

    // -----------------------------------------------------------------------
    // 配置属性（在 Blueprint 子类的 Default 面板中设置）
    // -----------------------------------------------------------------------

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|配置")
    EAtomElementType ElementType = EAtomElementType::C_Normal;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|配置")
    UDataTable* AtomDataTable = nullptr;

    // 接近检测半径，TODO-0005 公式确认前使用固定值
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|配置")
    float ProximityRadius = 200.f;

    // -----------------------------------------------------------------------
    // 运行时只读属性（BeginPlay 从 DataTable 填入）
    // -----------------------------------------------------------------------

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "原子|运行时")
    float Mass = 0.f;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "原子|运行时")
    int32 TotalSlots = 0;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "原子|运行时")
    bool bCanFormRing = false;

public:
    // -----------------------------------------------------------------------
    // 事件
    // -----------------------------------------------------------------------

    // 接近范围内出现其他原子时广播，由上层决策系统监听
    UPROPERTY(BlueprintAssignable, Category = "原子|事件")
    FOnAtomProximityEnter OnProximityEnter;

    UPROPERTY(BlueprintAssignable, Category = "原子|事件")
    FOnAtomProximityExit OnProximityExit;

    // 状态变化时由 Blueprint 子类实现视觉反馈
    UFUNCTION(BlueprintImplementableEvent, Category = "原子|事件")
    void OnAtomStateChanged(EAtomState NewState);

    // -----------------------------------------------------------------------
    // Blueprint 可调用方法 — 查询
    // -----------------------------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 GetAvailableSlotCount() const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 GetTotalSlotCount() const { return TotalSlots; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool IsSlotOccupied(int32 SlotIndex) const;

    // 返回第一个空闲槽位的索引，无空槽时返回 INDEX_NONE
    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 FindFreeSlotIndex() const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 FindNearestFreeSlotIndexToWorldLocation(FVector WorldLocation) const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    float GetMass() const { return Mass; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    EAtomState GetAtomState() const { return AtomState; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    TArray<FBondRecord> GetBonds() const { return Bonds; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    EAtomElementType GetElementType() const { return ElementType; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool CanFormRing() const { return bCanFormRing; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    FGuid GetAtomUid() const { return AtomUid; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool HasAtomUid() const { return AtomUid.IsValid(); }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    TArray<FGuid> GetBondUids() const { return BondUids; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    UFluidMotionComponent* GetFluidMotionComponent() const { return FluidMotionComponent; }

    virtual void ConfigureElementType(EAtomElementType InElementType);

    UFUNCTION(BlueprintCallable, Category = "ChemicalBond|Plane")
    void ConstrainToGameplayPlane();

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    float GetProximityRadius() const { return ProximityRadius; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    FVector GetSlotWorldLocation(int32 SlotIndex) const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    FVector GetSlotWorldOffset(int32 SlotIndex) const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    float GetSlotConnectionDistance() const { return AtomBodyDiameter; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    float GetSlotBaseAngleDegrees(int32 SlotIndex) const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool IsInteractionCoolingDown() const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool IsProximityOverlappingAtom(AAtomBase* OtherAtom) const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    EAtomInteractionRangeVisualState GetInteractionRangeVisualState() const;

    // -----------------------------------------------------------------------
    // Blueprint 可调用方法 — 槽位与键操作
    // -----------------------------------------------------------------------

    // 双方都需要调用各自的 AddBond，由上层连接系统负责协调
    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool AddBond(AAtomBase* Partner, EBondType InBondType, int32 MySlot, int32 PartnerSlot);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool AddBondWithUid(FGuid InBondUid, AAtomBase* Partner, EBondType InBondType, int32 MySlot, int32 PartnerSlot);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool RemoveBond(int32 MySlotIndex);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool RemoveBondByUid(FGuid InBondUid);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool SetBondTypeByUid(FGuid InBondUid, EBondType NewBondType);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool AddBondSlotByUid(FGuid InBondUid, int32 MySlot, int32 PartnerSlot);

    UFUNCTION(BlueprintCallable, Category = "原子|连接")
    bool RemoveBondSlotByUid(FGuid InBondUid, int32 MySlot, int32 PartnerSlot);

    UFUNCTION(BlueprintCallable, Category = "原子|注册表")
    void AssignAtomUid(FGuid InAtomUid);

    UFUNCTION(BlueprintCallable, Category = "原子|注册表")
    void ClearAtomUid();

    // -----------------------------------------------------------------------
    // Blueprint 可调用方法 — 状态
    // -----------------------------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "原子|状态")
    void SetAtomState(EAtomState NewState);

    UFUNCTION(BlueprintCallable, Category = "原子|状态")
    void BeginInteractionCooldown(float CooldownSeconds);

    UFUNCTION(BlueprintCallable, Category = "原子|状态")
    void SetInteractionRangeVisualApplicable(bool bInApplicable);

    UFUNCTION(BlueprintCallable, Category = "原子|表现")
    void RefreshSlotVisualLayout();

    UFUNCTION(BlueprintCallable, Category = "原子|表现")
    void NotifyBondLayoutChanged();

    // 成环槽位角度覆盖：成环时由 Director 设置指定槽位指向相邻环顶点的本地角度，
    // 使参与环的原子键槽滑动并形成均匀正多边形。优先级高于多键展开与默认轨道角度。
    void SetRingSlotAngleOverrideDegrees(int32 SlotIndex, float LocalAngleDegrees);
    void ClearRingSlotAngleOverride(int32 SlotIndex);
    void ClearAllRingSlotAngleOverrides();

private:
    UPROPERTY()
    USphereComponent* ProximitySphere = nullptr;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UStaticMeshComponent>> SlotVisualMeshes;

    UPROPERTY()
    TObjectPtr<UStaticMesh> SlotSphereMesh = nullptr;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> SlotVisualMaterial = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "原子|运动", meta = (AllowPrivateAccess = "true"))
    UFluidMotionComponent* FluidMotionComponent = nullptr;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "原子|注册表", meta = (AllowPrivateAccess = "true"))
    FGuid AtomUid;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "原子|注册表", meta = (AllowPrivateAccess = "true"))
    TArray<FGuid> BondUids;

    // 槽位占用状态，长度 = TotalSlots，由 InitFromDataTable 初始化
    UPROPERTY()
    TArray<bool> SlotOccupied;

    UPROPERTY()
    TArray<FBondRecord> Bonds;

    // 成环时各槽位的本地角度覆盖（槽位索引 -> 角度，单位度）。空表示无覆盖。
    TMap<int32, float> RingSlotAngleOverridesDegrees;

    UPROPERTY()
    EAtomState AtomState = EAtomState::Free;

    UPROPERTY(Transient)
    float InteractionCooldownEndTime = 0.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|交互范围表现", meta = (AllowPrivateAccess = "true"))
    bool bInteractionRangeVisualApplicable = true;

    // 蓝图配置：Class=原子 Blueprint 派生类，Range=1.0..1000.0，
    // Effect=定义原子本体直径，并作为槽位球尺寸和槽位对齐距离的基准。
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|槽位表现", meta = (AllowPrivateAccess = "true", ClampMin = "1.0"))
    float AtomBodyDiameter = 65.f;

    // 蓝图配置：Class=原子 Blueprint 派生类，Range=0.0..1000.0，
    // Effect=槽位球中心到原子中心的固定轨道距离 d，运行时不得被滑动逻辑改变。
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|槽位表现", meta = (AllowPrivateAccess = "true", ClampMin = "0.0"))
    float SlotOrbitDistance = 55.f;

    // 蓝图配置：Class=原子 Blueprint 派生类，Range=0.01..1.0，
    // Effect=槽位球直径相对原子本体直径的比例，正式规则默认为 0.3。
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|槽位表现", meta = (AllowPrivateAccess = "true", ClampMin = "0.01", ClampMax = "1.0"))
    float SlotSphereDiameterRatio = 0.3f;

    // 蓝图配置：Class=原子 Blueprint 派生类，Range=0.0..90.0 度，
    // Effect=双键/三键槽位滑向成键侧后围绕朝向的展开角。
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "原子|槽位表现", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", ClampMax = "90.0"))
    float MultiBondSlotSpreadDegrees = 20.f;

    void InitFromDataTable();
    void ApplyTemporaryInteractionRadiusFromMass();
    void RebuildSlotVisualMeshes();
    FVector GetSlotRelativeLocation(int32 SlotIndex) const;
    bool TryGetMultiBondSlotAngleDegrees(int32 SlotIndex, float& OutAngleDegrees) const;
    void TryRegisterWithDirector();
    void TryUnregisterFromDirector();

    UFUNCTION()
    void HandleProximitySphereOverlap(
        UPrimitiveComponent* OverlappedComponent,
        AActor* OtherActor,
        UPrimitiveComponent* OtherComp,
        int32 OtherBodyIndex,
        bool bFromSweep,
        const FHitResult& SweepResult);

    UFUNCTION()
    void HandleProximitySphereEndOverlap(
        UPrimitiveComponent* OverlappedComponent,
        AActor* OtherActor,
        UPrimitiveComponent* OtherComp,
        int32 OtherBodyIndex);
};
