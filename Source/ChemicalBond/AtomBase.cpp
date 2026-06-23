#include "AtomBase.h"

#include "Game/ChemicalBondGameDirector.h"
#include "Game/ChemicalBondGameMode.h"
#include "Movement/FluidMotionComponent.h"

AAtomBase::AAtomBase()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = Root;

    ProximitySphere = CreateDefaultSubobject<USphereComponent>(TEXT("ProximitySphere"));
    ProximitySphere->SetupAttachment(RootComponent);
    ProximitySphere->SetSphereRadius(200.f);
    ProximitySphere->SetCollisionProfileName(TEXT("OverlapAll"));
    ProximitySphere->SetGenerateOverlapEvents(true);

    FluidMotionComponent = CreateDefaultSubobject<UFluidMotionComponent>(TEXT("FluidMotionComponent"));
}

void AAtomBase::BeginPlay()
{
    Super::BeginPlay();
    InitFromDataTable();

    ProximitySphere->SetSphereRadius(ProximityRadius);
    ProximitySphere->OnComponentBeginOverlap.AddDynamic(
        this, &AAtomBase::HandleProximitySphereOverlap);

    TryRegisterWithDirector();
}

void AAtomBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    TryUnregisterFromDirector();

    Super::EndPlay(EndPlayReason);
}

void AAtomBase::InitFromDataTable()
{
    if (!AtomDataTable)
    {
        if (TotalSlots > 0)
        {
            return;
        }

        UE_LOG(LogTemp, Warning, TEXT("AAtomBase [%s]: AtomDataTable 未设置"), *GetName());
        return;
    }

    // EAtomElementType 枚举值名称（如 "C_Normal"）直接用作 DataTable RowName
    FString FullName = UEnum::GetValueAsString(ElementType);
    FString RowNameStr;
    if (!FullName.Split(TEXT("::"), nullptr, &RowNameStr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        RowNameStr = FullName;
    }

    const FAtomDataRow* Row = AtomDataTable->FindRow<FAtomDataRow>(
        FName(*RowNameStr), TEXT("AAtomBase::InitFromDataTable"));

    if (!Row)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("AAtomBase [%s]: DataTable 中找不到行 [%s]"), *GetName(), *RowNameStr);
        return;
    }

    ApplyRuntimeAtomData(ElementType, Row->Mass, Row->TotalSlots, Row->bCanFormRing);
}

void AAtomBase::ApplyRuntimeAtomData(EAtomElementType InElementType, float InMass, int32 InTotalSlots, bool bInCanFormRing)
{
    ElementType = InElementType;
    Mass = InMass;
    TotalSlots = FMath::Max(0, InTotalSlots);
    bCanFormRing = bInCanFormRing;

    SlotOccupied.Init(false, TotalSlots);

    if (FluidMotionComponent)
    {
        FluidMotionComponent->SetEffectiveMass(Mass);
    }
}

int32 AAtomBase::GetAvailableSlotCount() const
{
    int32 Count = 0;
    for (bool bOccupied : SlotOccupied)
    {
        if (!bOccupied) ++Count;
    }
    return Count;
}

int32 AAtomBase::FindFreeSlotIndex() const
{
    for (int32 i = 0; i < SlotOccupied.Num(); ++i)
    {
        if (!SlotOccupied[i]) return i;
    }
    return INDEX_NONE;
}

bool AAtomBase::AddBond(AAtomBase* Partner, EBondType InBondType, int32 MySlot, int32 PartnerSlot)
{
    return AddBondWithUid(FGuid(), Partner, InBondType, MySlot, PartnerSlot);
}

bool AAtomBase::AddBondWithUid(FGuid InBondUid, AAtomBase* Partner, EBondType InBondType, int32 MySlot, int32 PartnerSlot)
{
    if (!Partner) return false;
    if (MySlot < 0 || MySlot >= TotalSlots) return false;
    if (SlotOccupied[MySlot]) return false;
    if (InBondUid.IsValid() && BondUids.Contains(InBondUid)) return false;

    SlotOccupied[MySlot] = true;

    FBondRecord Record;
    Record.BondUid         = InBondUid;
    Record.PartnerAtom     = Partner;
    Record.BondType        = InBondType;
    Record.MySlotIndex     = MySlot;
    Record.PartnerSlotIndex = PartnerSlot;
    Bonds.Add(Record);

    if (InBondUid.IsValid())
    {
        BondUids.Add(InBondUid);
    }

    return true;
}

bool AAtomBase::RemoveBond(int32 MySlotIndex)
{
    if (MySlotIndex < 0 || MySlotIndex >= TotalSlots) return false;
    if (!SlotOccupied[MySlotIndex]) return false;

    SlotOccupied[MySlotIndex] = false;

    for (int32 i = Bonds.Num() - 1; i >= 0; --i)
    {
        if (Bonds[i].MySlotIndex == MySlotIndex)
        {
            if (Bonds[i].BondUid.IsValid())
            {
                BondUids.Remove(Bonds[i].BondUid);
            }
            Bonds.RemoveAt(i);
            break;
        }
    }
    return true;
}

bool AAtomBase::RemoveBondByUid(FGuid InBondUid)
{
    if (!InBondUid.IsValid()) return false;

    for (int32 i = Bonds.Num() - 1; i >= 0; --i)
    {
        if (Bonds[i].BondUid == InBondUid)
        {
            const int32 MySlotIndex = Bonds[i].MySlotIndex;
            if (MySlotIndex >= 0 && MySlotIndex < SlotOccupied.Num())
            {
                SlotOccupied[MySlotIndex] = false;
            }

            Bonds.RemoveAt(i);
            BondUids.Remove(InBondUid);
            return true;
        }
    }

    BondUids.Remove(InBondUid);
    return false;
}

void AAtomBase::AssignAtomUid(FGuid InAtomUid)
{
    AtomUid = InAtomUid;
}

void AAtomBase::ClearAtomUid()
{
    AtomUid.Invalidate();
}

void AAtomBase::SetAtomState(EAtomState NewState)
{
    AtomState = NewState;
    OnAtomStateChanged(NewState);
}

void AAtomBase::TryRegisterWithDirector()
{
    UWorld* World = GetWorld();
    if (!World) return;

    AChemicalBondGameMode* ChemicalBondGameMode = World->GetAuthGameMode<AChemicalBondGameMode>();
    if (!ChemicalBondGameMode) return;

    AChemicalBondGameDirector* GameDirector = ChemicalBondGameMode->GetGameDirector();
    if (!GameDirector) return;

    GameDirector->SpawnAtom(this);
}

void AAtomBase::TryUnregisterFromDirector()
{
    UWorld* World = GetWorld();
    if (!World) return;

    AChemicalBondGameMode* ChemicalBondGameMode = World->GetAuthGameMode<AChemicalBondGameMode>();
    if (!ChemicalBondGameMode) return;

    AChemicalBondGameDirector* GameDirector = ChemicalBondGameMode->GetGameDirector();
    if (!GameDirector) return;

    GameDirector->TerminateAtom(this);
}

void AAtomBase::HandleProximitySphereOverlap(
    UPrimitiveComponent* OverlappedComponent,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    if (AAtomBase* OtherAtom = Cast<AAtomBase>(OtherActor))
    {
        OnProximityEnter.Broadcast(OtherAtom);
    }
}
