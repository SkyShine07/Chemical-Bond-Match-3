#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "AtomTypes.generated.h"

namespace ChemicalBondGameplayPlane
{
    constexpr float AtomPlaneZ = 0.f;

    FORCEINLINE FVector ProjectLocation(FVector Location)
    {
        Location.Z = AtomPlaneZ;
        return Location;
    }

    FORCEINLINE FVector ProjectVector(FVector Vector)
    {
        Vector.Z = 0.f;
        return Vector;
    }
}

UENUM(BlueprintType)
enum class EAtomElementType : uint8
{
    H           UMETA(DisplayName = "H"),
    C_Normal    UMETA(DisplayName = "C (普通)"),
    C_Player    UMETA(DisplayName = "C (玩家)"),
    C_Ring      UMETA(DisplayName = "C (成环)"),
    O_Normal    UMETA(DisplayName = "O (普通)"),
    O_Ring      UMETA(DisplayName = "O (成环)"),
    N_Normal    UMETA(DisplayName = "N (普通)"),
    N_Ring      UMETA(DisplayName = "N (成环)"),
    P_Normal    UMETA(DisplayName = "P (普通)"),
    P_Ring      UMETA(DisplayName = "P (成环)"),
};

UENUM(BlueprintType)
enum class EBondType : uint8
{
    Single  UMETA(DisplayName = "单键"),
    Double  UMETA(DisplayName = "双键"),
    Triple  UMETA(DisplayName = "三键"),
};

UENUM(BlueprintType)
enum class EAtomState : uint8
{
    Free            UMETA(DisplayName = "游离"),
    PendingDecision UMETA(DisplayName = "待决策"),
    PlayerConnected UMETA(DisplayName = "已连接到玩家基团"),
};

// DataTable 行结构，RowName 与 EElementType 变体名对应（如 "C_Normal"）
USTRUCT(BlueprintType)
struct FAtomDataRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "原子数据")
    float Mass = 1.f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "原子数据")
    int32 TotalSlots = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "原子数据")
    bool bCanFormRing = false;
};
