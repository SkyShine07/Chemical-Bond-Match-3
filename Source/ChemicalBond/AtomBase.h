#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SphereComponent.h"
#include "AtomTypes.h"
#include "AtomBase.generated.h"

class AAtomBase;
class UFluidMotionComponent;

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
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAtomProximityEnter, AAtomBase*, OtherAtom);

UCLASS(Abstract)
class CHEMICALBOND_API AAtomBase : public AActor
{
    GENERATED_BODY()

public:
    AAtomBase();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void ApplyRuntimeAtomData(EAtomElementType InElementType, float InMass, int32 InTotalSlots, bool bInCanFormRing);

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

    // 状态变化时由 Blueprint 子类实现视觉反馈
    UFUNCTION(BlueprintImplementableEvent, Category = "原子|事件")
    void OnAtomStateChanged(EAtomState NewState);

    // -----------------------------------------------------------------------
    // Blueprint 可调用方法 — 查询
    // -----------------------------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 GetAvailableSlotCount() const;

    // 返回第一个空闲槽位的索引，无空槽时返回 INDEX_NONE
    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    int32 FindFreeSlotIndex() const;

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    float GetMass() const { return Mass; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    EAtomState GetAtomState() const { return AtomState; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    TArray<FBondRecord> GetBonds() const { return Bonds; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    EAtomElementType GetElementType() const { return ElementType; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    FGuid GetAtomUid() const { return AtomUid; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    bool HasAtomUid() const { return AtomUid.IsValid(); }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    TArray<FGuid> GetBondUids() const { return BondUids; }

    UFUNCTION(BlueprintCallable, Category = "原子|查询")
    UFluidMotionComponent* GetFluidMotionComponent() const { return FluidMotionComponent; }

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

    UFUNCTION(BlueprintCallable, Category = "原子|注册表")
    void AssignAtomUid(FGuid InAtomUid);

    UFUNCTION(BlueprintCallable, Category = "原子|注册表")
    void ClearAtomUid();

    // -----------------------------------------------------------------------
    // Blueprint 可调用方法 — 状态
    // -----------------------------------------------------------------------

    UFUNCTION(BlueprintCallable, Category = "原子|状态")
    void SetAtomState(EAtomState NewState);

private:
    UPROPERTY()
    USphereComponent* ProximitySphere = nullptr;

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

    UPROPERTY()
    EAtomState AtomState = EAtomState::Free;

    void InitFromDataTable();
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
};
