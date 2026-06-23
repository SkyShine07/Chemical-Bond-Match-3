#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidMotionComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogChemicalBondFluidMotion, Log, All);

// 负责液体感运动的受力积分、阻力计算和 Blueprint 调参入口。
// 本组件只处理运动语义，不拥有连接、规则、镜头或表现逻辑。
UCLASS(ClassGroup=(ChemicalBond), meta=(BlueprintSpawnableComponent))
class CHEMICALBOND_API UFluidMotionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidMotionComponent();

	// 生命周期
	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// 受力输入 API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void SetMoveInput(FVector WorldDirection, float Scale);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void ClearMoveInput();

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void SetYawInput(float Scale);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void ClearYawInput();

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void SetSprintActive(bool bActive);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void AddLinearForce(FVector Force);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void AddLinearImpulse(FVector Impulse);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void AddYawTorque(float Torque);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void AddYawImpulse(float Impulse);

	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void StopFluidMotion();

	// 质量 API
	UFUNCTION(BlueprintCallable, Category="ChemicalBond|FluidMotion")
	void SetEffectiveMass(float NewMass);

	UFUNCTION(BlueprintPure, Category="ChemicalBond|FluidMotion")
	float GetEffectiveMass() const { return EffectiveMass; }

	// 状态查询
	UFUNCTION(BlueprintPure, Category="ChemicalBond|FluidMotion")
	FVector GetLinearVelocity() const { return LinearVelocity; }

	UFUNCTION(BlueprintPure, Category="ChemicalBond|FluidMotion")
	float GetYawVelocity() const { return YawVelocity; }

	UFUNCTION(BlueprintPure, Category="ChemicalBond|FluidMotion")
	bool IsSprintActive() const { return bSprintActive; }

protected:
	// 策划配置
	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.01..10000.0，
	// Effect=对象未从原子数据或外部系统获得质量时使用的默认质量；数值越大，加速和旋转响应越慢。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Mass", meta=(ClampMin="0.01"))
	float DefaultMass = 12.f;

	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.01..10000.0，
	// Effect=限制运行时有效质量的最小值，避免除零和极端加速度。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Mass", meta=(ClampMin="0.01"))
	float MinimumMass = 0.1f;

	// 蓝图配置：Class=玩家基团、游离原子、待决策基团，Range=0.0..100000.0，
	// Effect=持续移动输入产生的基础推进力。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Force", meta=(ClampMin="0.0"))
	float MoveForce = 7600.f;

	// 蓝图配置：Class=玩家基团，Range=0.0..100000.0，
	// Effect=冲刺开启时额外叠加在移动方向上的推进力。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Force", meta=(ClampMin="0.0"))
	float SprintForce = 7200.f;

	// 蓝图配置：Class=可旋转基团，Range=0.0..100000.0，
	// Effect=Q/E 或其他旋转输入产生的偏航力矩。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Force", meta=(ClampMin="0.0"))
	float YawTorque = 7200.f;

	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.0..10000.0，
	// Effect=速度越大时固定比例抵消线速度，提供基础液体阻力。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Drag", meta=(ClampMin="0.0"))
	float LinearDrag = 3.5f;

	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.0..10000.0，
	// Effect=速度越快时阻力增长越明显，用于制造液体中的速度上限感。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Drag", meta=(ClampMin="0.0"))
	float QuadraticDrag = 0.015f;

	// 蓝图配置：Class=所有可旋转对象，Range=0.0..10000.0，
	// Effect=抵消偏航角速度，提供旋转阻力。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Drag", meta=(ClampMin="0.0"))
	float AngularDrag = 4.f;

	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.0..100000.0，
	// Effect=限制最大线速度，防止连续受力后速度失控。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Limits", meta=(ClampMin="0.0"))
	float MaxLinearSpeed = 900.f;

	// 蓝图配置：Class=所有可旋转对象，Range=0.0..3600.0，
	// Effect=限制最大偏航角速度，单位为度/秒。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Limits", meta=(ClampMin="0.0"))
	float MaxYawSpeed = 240.f;

	// 蓝图配置：Class=所有挂接 FluidMotionComponent 的可移动对象，Range=0.0..100.0，
	// Effect=速度低于该值且没有输入时归零，避免对象无限微小滑动。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Limits", meta=(ClampMin="0.0"))
	float StopLinearSpeedThreshold = 2.f;

	// 蓝图配置：Class=所有可旋转对象，Range=0.0..100.0，
	// Effect=偏航角速度低于该值且没有输入时归零。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Limits", meta=(ClampMin="0.0"))
	float StopYawSpeedThreshold = 1.f;

	// 蓝图配置：Class=需要轻微漂浮感的对象，Range=0.0..10000.0，
	// Effect=持续加入低频扰动力；0 表示关闭漂浮扰动。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Float", meta=(ClampMin="0.0"))
	float FloatingDisturbanceForce = 0.f;

	// 蓝图配置：Class=需要轻微漂浮感的对象，Range=0.01..10.0，
	// Effect=控制漂浮扰动变化速度；数值越大，扰动变化越快。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|FluidMotion|Float", meta=(ClampMin="0.01"))
	float FloatingDisturbanceFrequency = 0.35f;

private:
	// 运行时状态
	UPROPERTY(Transient)
	FVector LinearVelocity = FVector::ZeroVector;

	UPROPERTY(Transient)
	FVector PendingForce = FVector::ZeroVector;

	UPROPERTY(Transient)
	FVector MoveInput = FVector::ZeroVector;

	UPROPERTY(Transient)
	float YawVelocity = 0.f;

	UPROPERTY(Transient)
	float PendingYawTorque = 0.f;

	UPROPERTY(Transient)
	float YawInput = 0.f;

	UPROPERTY(Transient)
	float EffectiveMass = 12.f;

	UPROPERTY(Transient)
	bool bSprintActive = false;

	UPROPERTY(Transient)
	float FloatingSeed = 0.f;

	void ApplyConfiguredForces();
	void IntegrateLinearVelocity(float DeltaTime);
	void IntegrateYawVelocity(float DeltaTime);
	void MoveOwner(float DeltaTime);
	void RotateOwner(float DeltaTime);
	void ConstrainOwnerToGameplayPlane() const;
	float GetSafeMass() const;
	FVector GetFloatingForce(float TimeSeconds) const;
};
