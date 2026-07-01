#pragma once

#include "CoreMinimal.h"
#include "../AtomBase.h"
#include "PlaygroundAtom.generated.h"

class UStaticMeshComponent;
class UTextRenderComponent;

// Playground 专用可视化原子，继承 AAtomBase 以复用当前原子核心能力。
// 它使用 C++ 内置测试数据，不依赖正式 DataTable 资产。
UCLASS(Blueprintable)
class CHEMICALBOND_API APlaygroundAtom : public AAtomBase
{
	GENERATED_BODY()

public:
	APlaygroundAtom();

	// Playground API
	virtual void ConfigureElementType(EAtomElementType InElementType) override;
	void ConfigurePlaygroundAtom(EAtomElementType InElementType);

protected:
	// 生命周期
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	// 可视组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UStaticMeshComponent> VisualMesh = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ChemicalBond|Playground", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UTextRenderComponent> ElementLabel = nullptr;

	void RefreshVisuals();
	static void GetPlaygroundAtomDefaults(EAtomElementType InElementType, float& OutMass, int32& OutSlots, bool& bOutCanFormRing);
	static FText GetElementLabelText(EAtomElementType InElementType);
	static FLinearColor GetElementColor(EAtomElementType InElementType);
};
