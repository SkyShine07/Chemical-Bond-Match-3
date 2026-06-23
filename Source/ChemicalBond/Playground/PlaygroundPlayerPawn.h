#pragma once

#include "CoreMinimal.h"
#include "../AtomBase.h"
#include "PlaygroundPlayerPawn.generated.h"

class UCameraComponent;
class USphereComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

// Playground 专用 C_Player 玩家粒子，用于 PIE 直接验证 FluidMotionComponent 手感。
// 输入在 Tick 中读取键盘状态，避免污染正式 Enhanced Input 配置。
UCLASS(Blueprintable)
class CHEMICALBOND_API APlaygroundPlayerPawn : public AAtomBase
{
	GENERATED_BODY()

public:
	APlaygroundPlayerPawn();

	// 生命周期
	virtual void PreInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	// 策划配置
	// 蓝图配置：Class=Playground 玩家 Pawn，Range=0.01..10000.0，
	// Effect=测试玩家 C_Player 的运动质量，默认使用 C 的质量 12。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(ClampMin="0.01"))
	float PlayerAtomMass = 12.f;

private:
	// 组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<USphereComponent> CollisionRoot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UStaticMeshComponent> VisualMesh = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UTextRenderComponent> ElementLabel = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<USpringArmComponent> CameraBoom = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UCameraComponent> Camera = nullptr;

	void PollPlaygroundInput();
	void PollConnectionDecisionInput(const APlayerController* PlayerController);

	bool bWasSpaceDown = false;
	bool bWasFDown = false;
};
