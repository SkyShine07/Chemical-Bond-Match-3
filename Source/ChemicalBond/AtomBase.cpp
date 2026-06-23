#include "AtomBase.h"

#include "Game/ChemicalBondGameDirector.h"
#include "Game/ChemicalBondGameMode.h"
#include "Movement/FluidMotionComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AAtomBase::AAtomBase()
{
    PrimaryActorTick.bCanEverTick = true;

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = Root;

    ProximitySphere = CreateDefaultSubobject<USphereComponent>(TEXT("ProximitySphere"));
    ProximitySphere->SetupAttachment(RootComponent);
    ProximitySphere->SetSphereRadius(200.f);
    ProximitySphere->SetCollisionProfileName(TEXT("OverlapAll"));
    ProximitySphere->SetGenerateOverlapEvents(true);

    ProximityVisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProximityVisual"));
    ProximityVisualMesh->SetupAttachment(RootComponent);
    ProximityVisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ProximityVisualMesh->SetGenerateOverlapEvents(false);
    ProximityVisualMesh->SetCastShadow(false);
    ProximityVisualMesh->bHiddenInGame = true;
    ProximityVisualMesh->SetVisibility(false);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> RangeDiscMesh(
        TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    if (RangeDiscMesh.Succeeded())
    {
        ProximityVisualMesh->SetStaticMesh(RangeDiscMesh.Object);
    }

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> RangeMaterial(
        TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
    if (RangeMaterial.Succeeded())
    {
        ProximityVisualMesh->SetMaterial(0, RangeMaterial.Object);
    }

    FluidMotionComponent = CreateDefaultSubobject<UFluidMotionComponent>(TEXT("FluidMotionComponent"));
}

void AAtomBase::BeginPlay()
{
    Super::BeginPlay();
    ConstrainToGameplayPlane();
    InitFromDataTable();

    ProximitySphere->SetSphereRadius(ProximityRadius);
    RefreshProximityVisual();
    ProximitySphere->OnComponentBeginOverlap.AddDynamic(
        this, &AAtomBase::HandleProximitySphereOverlap);
    ProximitySphere->OnComponentEndOverlap.AddDynamic(
        this, &AAtomBase::HandleProximitySphereEndOverlap);

    TryRegisterWithDirector();
}

void AAtomBase::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ConstrainToGameplayPlane();
    DrawProximityIndicator();
}

void AAtomBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UE_LOG(LogTemp, Log,
        TEXT("[Game:Atom] Atom EndPlay. Atom=%s AtomUid=%s Reason=%d"),
        *GetNameSafe(this),
        *AtomUid.ToString(),
        static_cast<int32>(EndPlayReason));
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
    ApplyTemporaryInteractionRadiusFromMass();

    if (FluidMotionComponent)
    {
        FluidMotionComponent->SetEffectiveMass(Mass);
    }
}

void AAtomBase::SetInitialAtomState(EAtomState NewState)
{
    AtomState = NewState;
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
    Record.MySlotIndices.Add(MySlot);
    Record.PartnerSlotIndices.Add(PartnerSlot);
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
        if (Bonds[i].MySlotIndex == MySlotIndex || Bonds[i].MySlotIndices.Contains(MySlotIndex))
        {
            for (const int32 OccupiedSlotIndex : Bonds[i].MySlotIndices)
            {
                if (OccupiedSlotIndex >= 0 && OccupiedSlotIndex < SlotOccupied.Num())
                {
                    SlotOccupied[OccupiedSlotIndex] = false;
                }
            }

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
            for (const int32 MySlotIndex : Bonds[i].MySlotIndices)
            {
                if (MySlotIndex >= 0 && MySlotIndex < SlotOccupied.Num())
                {
                    SlotOccupied[MySlotIndex] = false;
                }
            }

            Bonds.RemoveAt(i);
            BondUids.Remove(InBondUid);
            return true;
        }
    }

    BondUids.Remove(InBondUid);
    return false;
}

bool AAtomBase::SetBondTypeByUid(FGuid InBondUid, EBondType NewBondType)
{
    if (!InBondUid.IsValid()) return false;

    for (FBondRecord& BondRecord : Bonds)
    {
        if (BondRecord.BondUid == InBondUid)
        {
            BondRecord.BondType = NewBondType;
            return true;
        }
    }

    return false;
}

bool AAtomBase::AddBondSlotByUid(FGuid InBondUid, int32 MySlot, int32 PartnerSlot)
{
    if (!InBondUid.IsValid()) return false;
    if (MySlot < 0 || MySlot >= TotalSlots) return false;
    if (SlotOccupied[MySlot]) return false;

    for (FBondRecord& BondRecord : Bonds)
    {
        if (BondRecord.BondUid == InBondUid)
        {
            SlotOccupied[MySlot] = true;
            BondRecord.MySlotIndices.AddUnique(MySlot);
            BondRecord.PartnerSlotIndices.AddUnique(PartnerSlot);
            return true;
        }
    }

    return false;
}

bool AAtomBase::RemoveBondSlotByUid(FGuid InBondUid, int32 MySlot, int32 PartnerSlot)
{
    if (!InBondUid.IsValid()) return false;

    for (FBondRecord& BondRecord : Bonds)
    {
        if (BondRecord.BondUid == InBondUid)
        {
            if (!BondRecord.MySlotIndices.Contains(MySlot))
            {
                return false;
            }

            BondRecord.MySlotIndices.Remove(MySlot);
            BondRecord.PartnerSlotIndices.Remove(PartnerSlot);

            if (MySlot >= 0 && MySlot < SlotOccupied.Num())
            {
                SlotOccupied[MySlot] = false;
            }

            if (BondRecord.MySlotIndex == MySlot)
            {
                BondRecord.MySlotIndex = BondRecord.MySlotIndices.IsEmpty() ? INDEX_NONE : BondRecord.MySlotIndices[0];
            }
            if (BondRecord.PartnerSlotIndex == PartnerSlot)
            {
                BondRecord.PartnerSlotIndex = BondRecord.PartnerSlotIndices.IsEmpty() ? INDEX_NONE : BondRecord.PartnerSlotIndices[0];
            }

            return true;
        }
    }

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
    RefreshProximityVisual();
    OnAtomStateChanged(NewState);
}

void AAtomBase::BeginInteractionCooldown(float CooldownSeconds)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    InteractionCooldownEndTime = FMath::Max(
        InteractionCooldownEndTime,
        World->GetTimeSeconds() + FMath::Max(0.f, CooldownSeconds));
}

bool AAtomBase::IsInteractionCoolingDown() const
{
    const UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    return World->GetTimeSeconds() < InteractionCooldownEndTime;
}

bool AAtomBase::IsProximityOverlappingAtom(AAtomBase* OtherAtom) const
{
    if (!ProximitySphere || !OtherAtom)
    {
        return false;
    }

    return ProximitySphere->IsOverlappingActor(OtherAtom);
}

void AAtomBase::ConstrainToGameplayPlane()
{
    const FVector CurrentLocation = GetActorLocation();
    const FVector PlaneLocation = ChemicalBondGameplayPlane::ProjectLocation(CurrentLocation);
    if (!CurrentLocation.Equals(PlaneLocation, KINDA_SMALL_NUMBER))
    {
        SetActorLocation(PlaneLocation, false);
    }
}

void AAtomBase::ApplyTemporaryInteractionRadiusFromMass()
{
    const float SafeMass = FMath::Max(Mass, 1.f);
    ProximityRadius = 120.f + SafeMass * 8.f;

    if (ProximitySphere)
    {
        ProximitySphere->SetSphereRadius(ProximityRadius);
    }

    RefreshProximityVisual();
}

void AAtomBase::RefreshProximityVisual()
{
    if (!ProximityVisualMesh)
    {
        return;
    }

    // The old filled disc could occlude atoms through transparency sorting.
    // Keep the temporary range visual as a debug ring only.
    const float VisualRadiusScale = FMath::Max(ProximityRadius / 50.f, 0.01f);
    ProximityVisualMesh->SetRelativeScale3D(FVector(VisualRadiusScale, VisualRadiusScale, 0.015f));
    ProximityVisualMesh->SetRelativeLocation(FVector(0.f, 0.f, -55.f));
    ProximityVisualMesh->SetVisibility(false, true);
}

void AAtomBase::DrawProximityIndicator()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const FLinearColor RangeColor =
        AtomState == EAtomState::PlayerConnected
            ? FLinearColor(0.1f, 0.9f, 0.65f, 1.f)
            : AtomState == EAtomState::PendingDecision
                ? FLinearColor(1.f, 0.8f, 0.15f, 1.f)
                : FLinearColor(0.35f, 0.65f, 1.f, 1.f);
    const FVector Center = ChemicalBondGameplayPlane::ProjectLocation(GetActorLocation()) + FVector(0.f, 0.f, -55.f);
    DrawDebugCircle(
        World,
        Center,
        ProximityRadius,
        96,
        RangeColor.ToFColor(true),
        false,
        0.f,
        0,
        2.f,
        FVector::ForwardVector,
        FVector::RightVector,
        false);
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
        OnProximityEnter.Broadcast(this, OtherAtom);
    }
}

void AAtomBase::HandleProximitySphereEndOverlap(
    UPrimitiveComponent* OverlappedComponent,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex)
{
    if (AAtomBase* OtherAtom = Cast<AAtomBase>(OtherActor))
    {
        OnProximityExit.Broadcast(this, OtherAtom);
    }
}
