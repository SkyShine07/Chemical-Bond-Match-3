#include "AtomBase.h"

#include "Game/ChemicalBondGameDirector.h"
#include "Game/ChemicalBondGameMode.h"
#include "Movement/FluidMotionComponent.h"
#include "Components/StaticMeshComponent.h"
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

    static ConstructorHelpers::FObjectFinder<UStaticMesh> SlotMesh(
        TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (SlotMesh.Succeeded())
    {
        SlotSphereMesh = SlotMesh.Object;
    }

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> SlotMaterial(
        TEXT("/Engine/EngineDebugMaterials/M_SimpleOpaque.M_SimpleOpaque"));
    if (SlotMaterial.Succeeded())
    {
        SlotVisualMaterial = SlotMaterial.Object;
    }

    FluidMotionComponent = CreateDefaultSubobject<UFluidMotionComponent>(TEXT("FluidMotionComponent"));
}

void AAtomBase::BeginPlay()
{
    Super::BeginPlay();
    ConstrainToGameplayPlane();
    InitFromDataTable();
    RebuildSlotVisualMeshes();

    ProximitySphere->SetSphereRadius(ProximityRadius);
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
    RefreshSlotVisualLayout();
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
    RebuildSlotVisualMeshes();
    ApplyTemporaryInteractionRadiusFromMass();

    if (FluidMotionComponent)
    {
        FluidMotionComponent->SetEffectiveMass(Mass);
    }
}

void AAtomBase::ConfigureElementType(EAtomElementType InElementType)
{
    float RuntimeMass = 12.f;
    int32 RuntimeSlots = 4;
    bool bRuntimeCanFormRing = false;

    switch (InElementType)
    {
    case EAtomElementType::H:
    case EAtomElementType::H_Normal:
        RuntimeMass = 1.f;
        RuntimeSlots = 1;
        break;
    case EAtomElementType::O_Normal:
        RuntimeMass = 16.f;
        RuntimeSlots = 2;
        break;
    case EAtomElementType::O_Ring:
        RuntimeMass = 16.f;
        RuntimeSlots = 2;
        bRuntimeCanFormRing = true;
        break;
    case EAtomElementType::N_Normal:
        RuntimeMass = 14.f;
        RuntimeSlots = 3;
        break;
    case EAtomElementType::N_Ring:
        RuntimeMass = 14.f;
        RuntimeSlots = 3;
        bRuntimeCanFormRing = true;
        break;
    case EAtomElementType::P_Normal:
        RuntimeMass = 31.f;
        RuntimeSlots = 5;
        break;
    case EAtomElementType::P_Ring:
        RuntimeMass = 31.f;
        RuntimeSlots = 5;
        bRuntimeCanFormRing = true;
        break;
    case EAtomElementType::C_Ring:
        RuntimeMass = 12.f;
        RuntimeSlots = 4;
        bRuntimeCanFormRing = true;
        break;
    case EAtomElementType::C_Player:
    case EAtomElementType::C_Normal:
    default:
        RuntimeMass = 12.f;
        RuntimeSlots = 4;
        break;
    }

    ApplyRuntimeAtomData(InElementType, RuntimeMass, RuntimeSlots, bRuntimeCanFormRing);
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

bool AAtomBase::IsSlotOccupied(int32 SlotIndex) const
{
    return SlotOccupied.IsValidIndex(SlotIndex) && SlotOccupied[SlotIndex];
}

int32 AAtomBase::FindFreeSlotIndex() const
{
    for (int32 i = 0; i < SlotOccupied.Num(); ++i)
    {
        if (!SlotOccupied[i]) return i;
    }
    return INDEX_NONE;
}

int32 AAtomBase::FindNearestFreeSlotIndexToWorldLocation(FVector WorldLocation) const
{
    int32 BestSlotIndex = INDEX_NONE;
    float BestDistanceSquared = TNumericLimits<float>::Max();

    for (int32 SlotIndex = 0; SlotIndex < SlotOccupied.Num(); ++SlotIndex)
    {
        if (SlotOccupied[SlotIndex])
        {
            continue;
        }

        const float DistanceSquared = FVector::DistSquared(
            GetSlotWorldLocation(SlotIndex),
            ChemicalBondGameplayPlane::ProjectLocation(WorldLocation));
        if (DistanceSquared < BestDistanceSquared)
        {
            BestDistanceSquared = DistanceSquared;
            BestSlotIndex = SlotIndex;
        }
    }

    return BestSlotIndex;
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
                RingSlotAngleOverridesDegrees.Remove(OccupiedSlotIndex);
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
                // 该键释放的槽位若带有成环角度覆盖，一并清除，避免环断开后留下错误角度。
                RingSlotAngleOverridesDegrees.Remove(MySlotIndex);
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

void AAtomBase::SetInteractionRangeVisualApplicable(bool bInApplicable)
{
    bInteractionRangeVisualApplicable = bInApplicable;
}

void AAtomBase::RefreshSlotVisualLayout()
{
    if (SlotVisualMeshes.Num() != TotalSlots)
    {
        RebuildSlotVisualMeshes();
    }

    const float SlotDiameter = FMath::Max(AtomBodyDiameter * SlotSphereDiameterRatio, 1.f);
    const float SlotScale = SlotDiameter / 100.f;
    for (int32 SlotIndex = 0; SlotIndex < SlotVisualMeshes.Num(); ++SlotIndex)
    {
        UStaticMeshComponent* SlotMeshComponent = SlotVisualMeshes[SlotIndex].Get();
        if (!SlotMeshComponent)
        {
            continue;
        }

        SlotMeshComponent->SetRelativeLocation(GetSlotRelativeLocation(SlotIndex));
        SlotMeshComponent->SetRelativeScale3D(FVector(SlotScale));
        SlotMeshComponent->SetVisibility(true, true);
    }
}

void AAtomBase::NotifyBondLayoutChanged()
{
    RefreshSlotVisualLayout();
}

void AAtomBase::SetRingSlotAngleOverrideDegrees(int32 SlotIndex, float LocalAngleDegrees)
{
    if (SlotIndex < 0 || SlotIndex >= TotalSlots)
    {
        return;
    }

    RingSlotAngleOverridesDegrees.Add(SlotIndex, LocalAngleDegrees);
    RefreshSlotVisualLayout();
}

void AAtomBase::ClearRingSlotAngleOverride(int32 SlotIndex)
{
    if (RingSlotAngleOverridesDegrees.Remove(SlotIndex) > 0)
    {
        RefreshSlotVisualLayout();
    }
}

void AAtomBase::ClearAllRingSlotAngleOverrides()
{
    if (RingSlotAngleOverridesDegrees.Num() > 0)
    {
        RingSlotAngleOverridesDegrees.Reset();
        RefreshSlotVisualLayout();
    }
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

EAtomInteractionRangeVisualState AAtomBase::GetInteractionRangeVisualState() const
{
    if (!bInteractionRangeVisualApplicable)
    {
        return EAtomInteractionRangeVisualState::NotApplicable;
    }

    if (GetAvailableSlotCount() <= 0)
    {
        return EAtomInteractionRangeVisualState::Unavailable;
    }

    return AtomState == EAtomState::PlayerConnected
        ? EAtomInteractionRangeVisualState::PlayerGroupAvailable
        : EAtomInteractionRangeVisualState::FreeAvailable;
}

FVector AAtomBase::GetSlotWorldLocation(int32 SlotIndex) const
{
    const FVector SlotLocation = GetActorLocation() + GetSlotWorldOffset(SlotIndex);
    return ChemicalBondGameplayPlane::ProjectLocation(SlotLocation);
}

FVector AAtomBase::GetSlotWorldOffset(int32 SlotIndex) const
{
    const FVector RelativeLocation = GetSlotRelativeLocation(SlotIndex);
    return ChemicalBondGameplayPlane::ProjectVector(
        GetActorTransform().TransformVectorNoScale(RelativeLocation));
}

float AAtomBase::GetSlotBaseAngleDegrees(int32 SlotIndex) const
{
    if (TotalSlots <= 0 || SlotIndex < 0 || SlotIndex >= TotalSlots)
    {
        return 0.f;
    }

    return static_cast<float>(SlotIndex) * 360.f / static_cast<float>(TotalSlots);
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

}

void AAtomBase::RebuildSlotVisualMeshes()
{
    if (HasAnyFlags(RF_ClassDefaultObject) || !GetWorld())
    {
        return;
    }

    for (TObjectPtr<UStaticMeshComponent>& SlotMeshComponent : SlotVisualMeshes)
    {
        if (SlotMeshComponent)
        {
            SlotMeshComponent->DestroyComponent();
        }
    }
    SlotVisualMeshes.Reset();

    if (TotalSlots <= 0 || !RootComponent)
    {
        return;
    }

    for (int32 SlotIndex = 0; SlotIndex < TotalSlots; ++SlotIndex)
    {
        const FName ComponentName = *FString::Printf(TEXT("SlotVisual_%d"), SlotIndex);
        UStaticMeshComponent* SlotMeshComponent = NewObject<UStaticMeshComponent>(this, ComponentName);
        if (!SlotMeshComponent)
        {
            continue;
        }

        SlotMeshComponent->SetupAttachment(RootComponent);
        SlotMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        SlotMeshComponent->SetGenerateOverlapEvents(false);
        SlotMeshComponent->SetCastShadow(false);
        if (SlotSphereMesh)
        {
            SlotMeshComponent->SetStaticMesh(SlotSphereMesh);
        }
        if (SlotVisualMaterial)
        {
            SlotMeshComponent->SetMaterial(0, SlotVisualMaterial);
        }
        SlotMeshComponent->RegisterComponent();
        SlotVisualMeshes.Add(SlotMeshComponent);
    }

    RefreshSlotVisualLayout();
}

FVector AAtomBase::GetSlotRelativeLocation(int32 SlotIndex) const
{
    float AngleDegrees = GetSlotBaseAngleDegrees(SlotIndex);
    // 成环槽位覆盖优先级最高；其次是多键展开；最后是默认轨道角度。
    if (const float* RingAngleOverride = RingSlotAngleOverridesDegrees.Find(SlotIndex))
    {
        AngleDegrees = *RingAngleOverride;
    }
    else
    {
        TryGetMultiBondSlotAngleDegrees(SlotIndex, AngleDegrees);
    }

    const float AngleRadians = FMath::DegreesToRadians(AngleDegrees);
    const float SafeOrbitDistance = FMath::Max(SlotOrbitDistance, 0.f);
    return FVector(
        FMath::Cos(AngleRadians) * SafeOrbitDistance,
        FMath::Sin(AngleRadians) * SafeOrbitDistance,
        0.f);
}

bool AAtomBase::TryGetMultiBondSlotAngleDegrees(int32 SlotIndex, float& OutAngleDegrees) const
{
    for (const FBondRecord& BondRecord : Bonds)
    {
        if (BondRecord.BondType == EBondType::Single || !BondRecord.MySlotIndices.Contains(SlotIndex))
        {
            continue;
        }

        const AAtomBase* PartnerAtom = BondRecord.PartnerAtom.Get();
        if (!PartnerAtom)
        {
            continue;
        }

        FVector DirectionToPartner =
            ChemicalBondGameplayPlane::ProjectLocation(PartnerAtom->GetActorLocation()) -
            ChemicalBondGameplayPlane::ProjectLocation(GetActorLocation());
        DirectionToPartner = ChemicalBondGameplayPlane::ProjectVector(DirectionToPartner);
        if (DirectionToPartner.IsNearlyZero())
        {
            DirectionToPartner = FVector::ForwardVector;
        }

        const FVector LocalDirection = GetActorTransform().InverseTransformVectorNoScale(
            DirectionToPartner.GetSafeNormal());
        const float FacingAngleDegrees = FMath::RadiansToDegrees(FMath::Atan2(LocalDirection.Y, LocalDirection.X));

        TArray<int32> SortedSlots = BondRecord.MySlotIndices;
        SortedSlots.Sort();
        const int32 SlotOrder = SortedSlots.IndexOfByKey(SlotIndex);
        const int32 SlotCount = SortedSlots.Num();
        if (SlotOrder == INDEX_NONE || SlotCount <= 1)
        {
            OutAngleDegrees = FacingAngleDegrees;
            return true;
        }

        const float CenteredOrder = static_cast<float>(SlotOrder) - (static_cast<float>(SlotCount - 1) * 0.5f);
        OutAngleDegrees = FacingAngleDegrees + CenteredOrder * MultiBondSlotSpreadDegrees;
        return true;
    }

    return false;
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
