// Fill out your copyright notice in the Description page of Project Settings.

#include "ChemicalBondGameDirector.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "../AtomBase.h"
#include "ChemicalBondGameMode.h"
#include "ChemicalBondLevelGoal.h"
#include "../Playground/PlaygroundAtom.h"
#include "../Playground/PlaygroundPlayerPawn.h"
#include "../Movement/FluidMotionComponent.h"
#include "AI/NavigationSystemBase.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY(LogChemicalBondDirector);

namespace
{
// TODO: 该临时连接距离需要在正式原子模型尺寸和策划数值表确认后改为配置项。
constexpr float TemporaryConnectedAtomDistance = 90.f;
constexpr float ConnectionConstraintTolerance = 1.f;
constexpr float PendingConnectionPullAlpha = 0.18f;
constexpr float PendingConnectionMaxStep = 30.f;

float GetPlanarYawDegrees(FVector Direction)
{
	Direction = ChemicalBondGameplayPlane::ProjectVector(Direction);
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}

	return FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
}

FVector MakePlanarDirectionFromYaw(float YawDegrees)
{
	const float YawRadians = FMath::DegreesToRadians(YawDegrees);
	return FVector(FMath::Cos(YawRadians), FMath::Sin(YawRadians), 0.f).GetSafeNormal();
}

float FindShortestAngleDegrees(float FromDegrees, float ToDegrees)
{
	return FMath::FindDeltaAngleDegrees(FromDegrees, ToDegrees);
}

FVector FindWeightedFacingDirection(AAtomBase* AtomA, int32 AtomASlot, AAtomBase* AtomB, int32 AtomBSlot)
{
	if (!AtomA || !AtomB)
	{
		return FVector::ForwardVector;
	}

	const float AtomAAngle = GetPlanarYawDegrees(AtomA->GetSlotWorldOffset(AtomASlot));
	const float AtomBAsAtomAAngle = GetPlanarYawDegrees(-AtomB->GetSlotWorldOffset(AtomBSlot));
	const float TotalMass = FMath::Max(AtomA->GetMass(), 1.f) + FMath::Max(AtomB->GetMass(), 1.f);
	const float AtomBInfluence = FMath::Max(AtomB->GetMass(), 1.f) / TotalMass;
	const float BlendedAngle = AtomAAngle + FindShortestAngleDegrees(AtomAAngle, AtomBAsAtomAAngle) * AtomBInfluence;
	return MakePlanarDirectionFromYaw(BlendedAngle);
}

bool TryGetAtomBondRecord(const AAtomBase* Atom, FGuid BondUid, FBondRecord& OutRecord)
{
	if (!Atom || !BondUid.IsValid())
	{
		return false;
	}

	const TArray<FBondRecord> AtomBonds = Atom->GetBonds();
	for (const FBondRecord& BondRecord : AtomBonds)
	{
		if (BondRecord.BondUid == BondUid)
		{
			OutRecord = BondRecord;
			return true;
		}
	}

	return false;
}

bool PairKeyContainsAtomUid(const FAtomInteractionPairKey& PairKey, FGuid AtomUid)
{
	return PairKey.FirstAtomUid == AtomUid || PairKey.SecondAtomUid == AtomUid;
}

bool RingCandidateContainsAtomUid(const FChemicalBondRingCandidate& Candidate, FGuid AtomUid)
{
	return Candidate.RingAtomUid == AtomUid
		|| Candidate.TargetAtomUid == AtomUid
		|| Candidate.PathAtomUids.Contains(AtomUid);
}

struct FRefreshRangeFrame
{
	FVector Center = FVector::ZeroVector;
	FVector AxisX = FVector::ForwardVector;
	FVector AxisY = FVector::RightVector;
	float YawDegrees = 0.f;
	FVector2D ViewPortHalfExtent = FVector2D::ZeroVector;
	FVector2D LogicHalfExtent = FVector2D::ZeroVector;
	FVector2D LifeSpanHalfExtent = FVector2D::ZeroVector;

	bool IsValid() const
	{
		return LifeSpanHalfExtent.X > 0.f && LifeSpanHalfExtent.Y > 0.f
			&& LogicHalfExtent.X > 0.f && LogicHalfExtent.Y > 0.f
			&& ViewPortHalfExtent.X > 0.f && ViewPortHalfExtent.Y > 0.f;
	}

	FVector2D ToLocal2D(const FVector& WorldLocation) const
	{
		const FVector Delta = ChemicalBondGameplayPlane::ProjectVector(WorldLocation - Center);
		return FVector2D(FVector::DotProduct(Delta, AxisX), FVector::DotProduct(Delta, AxisY));
	}

	FVector ToWorld(const FVector2D& LocalLocation) const
	{
		return ChemicalBondGameplayPlane::ProjectLocation(Center + AxisX * LocalLocation.X + AxisY * LocalLocation.Y);
	}
};

bool TryBuildRefreshRangeFrame(const AChemicalBondGameDirector* Director, FRefreshRangeFrame& OutFrame)
{
	if (!Director)
	{
		return false;
	}

	const APlaygroundPlayerPawn* PlayerPawn =
		Cast<APlaygroundPlayerPawn>(UGameplayStatics::GetPlayerPawn(Director, 0));
	if (!PlayerPawn)
	{
		return false;
	}

	FVector Center = FVector::ZeroVector;
	float YawDegrees = 0.f;
	FVector2D ViewPortHalfExtent = FVector2D::ZeroVector;
	FVector2D LogicHalfExtent = FVector2D::ZeroVector;
	FVector2D LifeSpanHalfExtent = FVector2D::ZeroVector;
	if (!PlayerPawn->GetRefreshRangeSnapshot(
		Center,
		YawDegrees,
		ViewPortHalfExtent,
		LogicHalfExtent,
		LifeSpanHalfExtent))
	{
		return false;
	}

	OutFrame.Center = Center;
	OutFrame.YawDegrees = YawDegrees;
	OutFrame.AxisX = MakePlanarDirectionFromYaw(YawDegrees);
	OutFrame.AxisY = MakePlanarDirectionFromYaw(YawDegrees + 90.f);
	OutFrame.ViewPortHalfExtent = ViewPortHalfExtent;
	OutFrame.LogicHalfExtent = LogicHalfExtent;
	OutFrame.LifeSpanHalfExtent = LifeSpanHalfExtent;
	return OutFrame.IsValid();
}

float GetRefreshElementMass(EAtomElementType ElementType)
{
	switch (ElementType)
	{
	case EAtomElementType::H:
	case EAtomElementType::H_Normal:
		return 1.f;
	case EAtomElementType::O_Normal:
	case EAtomElementType::O_Ring:
		return 16.f;
	case EAtomElementType::N_Normal:
	case EAtomElementType::N_Ring:
		return 14.f;
	case EAtomElementType::P_Normal:
	case EAtomElementType::P_Ring:
		return 31.f;
	case EAtomElementType::C_Normal:
	case EAtomElementType::C_Ring:
	default:
		return 12.f;
	}
}

float GetRefreshElementProximityRadius(EAtomElementType ElementType)
{
	return 120.f + FMath::Max(GetRefreshElementMass(ElementType), 1.f) * 8.f;
}

float DistanceSquaredPointToSegment2D(const FVector& Point, const FVector& SegmentStart, const FVector& SegmentEnd)
{
	const FVector Point2D = ChemicalBondGameplayPlane::ProjectLocation(Point);
	const FVector Start2D = ChemicalBondGameplayPlane::ProjectLocation(SegmentStart);
	const FVector End2D = ChemicalBondGameplayPlane::ProjectLocation(SegmentEnd);
	const FVector Segment = End2D - Start2D;
	const float SegmentLengthSquared = Segment.SizeSquared();
	if (SegmentLengthSquared <= KINDA_SMALL_NUMBER)
	{
		return FVector::DistSquared(Point2D, Start2D);
	}

	const float T = FMath::Clamp(FVector::DotProduct(Point2D - Start2D, Segment) / SegmentLengthSquared, 0.f, 1.f);
	const FVector ClosestPoint = Start2D + Segment * T;
	return FVector::DistSquared(Point2D, ClosestPoint);
}
}

AChemicalBondGameDirector::AChemicalBondGameDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RefreshAtomClass = APlaygroundAtom::StaticClass();

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	InteractionRangeFillMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("InteractionRangeFillMesh"));
	InteractionRangeFillMesh->SetupAttachment(RootComponent);
	InteractionRangeFillMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InteractionRangeFillMesh->SetGenerateOverlapEvents(false);
	InteractionRangeFillMesh->SetCastShadow(false);
	InteractionRangeFillMesh->bUseAsyncCooking = true;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> InteractionRangeFillMaterialAsset(
		TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	if (InteractionRangeFillMaterialAsset.Succeeded())
	{
		InteractionRangeFillMaterial = InteractionRangeFillMaterialAsset.Object;
	}

	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> BondVisualAsset(
		TEXT("/Game/VFX/huaxuejian/NS_LianJie.NS_LianJie"));
	if (BondVisualAsset.Succeeded())
	{
		BondVisualSystem = BondVisualAsset.Object;
	}

	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> DecisionWarningVisualAsset(
		TEXT("/Game/VFX/warning/NS_Warning.NS_Warning"));
	if (DecisionWarningVisualAsset.Succeeded())
	{
		DecisionWarningVisualSystem = DecisionWarningVisualAsset.Object;
	}
}

void AChemicalBondGameDirector::BeginPlay()
{
	Super::BeginPlay();

	if (InteractionRangeFillMesh && InteractionRangeFillMaterial)
	{
		InteractionRangeFillMaterialInstance = UMaterialInstanceDynamic::Create(InteractionRangeFillMaterial, this);
		if (InteractionRangeFillMaterialInstance)
		{
			InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("Color")), InteractionRangeFillColor);
			InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("BaseColor")), InteractionRangeFillColor);
			InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("TintColor")), InteractionRangeFillColor);
			InteractionRangeFillMaterialInstance->SetScalarParameterValue(FName(TEXT("Opacity")), InteractionRangeFillColor.A);
			InteractionRangeFillMesh->SetMaterial(0, InteractionRangeFillMaterialInstance);
		}
		else
		{
			InteractionRangeFillMesh->SetMaterial(0, InteractionRangeFillMaterial);
		}
	}

}

void AChemicalBondGameDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopDirector();

	Super::EndPlay(EndPlayReason);
}

void AChemicalBondGameDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ConstrainRegisteredAtomsToGameplayPlane();
	ProcessConnectionCandidates(DeltaSeconds);
	ProcessActiveDecision(DeltaSeconds);
	ProcessPhysicalConnections(DeltaSeconds);
	ProcessPlayerMoleculeDetection();
	ProcessSceneRefresh(DeltaSeconds);
	UpdateInteractionRangeFillVisual();
	UpdateAllBondVisuals();
	SpawnOrUpdateActiveDecisionWarningVisual();
	ConstrainRegisteredAtomsToGameplayPlane();
}

void AChemicalBondGameDirector::InitializeDirector(AChemicalBondGameMode* InOwningGameMode)
{
	if (!InOwningGameMode)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] InitializeDirector failed because OwningGameMode is null. Director=%s"),
			*GetNameSafe(this));
		return;
	}

	OwningGameMode = InOwningGameMode;
}

void AChemicalBondGameDirector::StartDirector()
{
	if (bDirectorStarted)
	{
		return;
	}

	if (!OwningGameMode)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] StartDirector failed because OwningGameMode is null. Director=%s"),
			*GetNameSafe(this));
		return;
	}

	bDirectorStarted = true;
	SetActorTickEnabled(true);
	RegisterExistingAtomsInWorld();
	InitializeRefreshRuntime();
	AssertBondRegistryConsistency();
}

void AChemicalBondGameDirector::StopDirector()
{
	if (!bDirectorStarted)
	{
		return;
	}

	bDirectorStarted = false;
	SetActorTickEnabled(false);
	ConnectionCandidates.Reset();
	DecisionQueue.Reset();
	QueuedDecisionPairKeys.Reset();
	LockedAtomUids.Reset();
	RigidGroupLocalTransforms.Reset();
	for (TPair<FGuid, FBondVisualComponentList>& VisualPair : BondVisualComponents)
	{
		for (TObjectPtr<UNiagaraComponent>& BondVisualComponent : VisualPair.Value.Components)
		{
			if (BondVisualComponent)
			{
				BondVisualComponent->Deactivate();
				BondVisualComponent->DestroyComponent();
			}
		}
	}
	BondVisualComponents.Reset();
	if (InteractionRangeFillMesh)
	{
		InteractionRangeFillMesh->ClearAllMeshSections();
	}
	DestroyActiveDecisionWarningVisual();
	bHasActiveDecisionRequest = false;
	RingDecisionQueue.Reset();
	bHasActiveRingDecision = false;
	ActiveRingCandidate = FChemicalBondRingCandidate();
	ActiveRingClosingBondUid.Invalidate();
	DemotedRingAtomUids.Reset();
	CompletedRingAtomUids.Reset();
	bPlayerMoleculeDirty = false;
	bVictoryReported = false;
	RefreshTimeRemaining = 0.f;
	GuideTimeRemaining = 0.f;
	CurrentMainGuideRegion = INDEX_NONE;
}

bool AChemicalBondGameDirector::IsDirectorStarted() const
{
	return bDirectorStarted;
}

FGuid AChemicalBondGameDirector::SpawnAtom(AAtomBase* Atom)
{
	if (!Atom)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] Cannot register atom because Atom is null. Director=%s"),
			*GetNameSafe(this));
		return FGuid();
	}

	const FGuid ExistingUid = Atom->GetAtomUid();
	if (ExistingUid.IsValid())
	{
		Atom->ConstrainToGameplayPlane();
		if (!AtomRegistry.Contains(ExistingUid))
		{
			AtomRegistry.Add(ExistingUid, Atom);
		}
		BindAtomConnectionEvents(Atom);
		return ExistingUid;
	}

	const FGuid NewAtomUid = GenerateUniqueAtomUid();
	Atom->ConstrainToGameplayPlane();
	Atom->AssignAtomUid(NewAtomUid);
	AtomRegistry.Add(NewAtomUid, Atom);
	BindAtomConnectionEvents(Atom);
	return NewAtomUid;
}

bool AChemicalBondGameDirector::TerminateAtom(AAtomBase* Atom)
{
	if (!Atom)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] TerminateAtom ignored because Atom is null. Director=%s"),
			*GetNameSafe(this));
		return false;
	}

	const FGuid AtomUid = Atom->GetAtomUid();
	if (!AtomUid.IsValid())
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] TerminateAtom ignored because AtomUid is invalid. Atom=%s"),
			*GetNameSafe(Atom));
		return false;
	}

	return TerminateAtomByUid(AtomUid);
}

bool AChemicalBondGameDirector::TerminateAtomByUid(FGuid AtomUid)
{
	if (!AtomUid.IsValid())
	{
		return false;
	}

	PruneStaleAtomRegistryEntries();
	AssertBondRegistryConsistency();

	TWeakObjectPtr<AAtomBase>* AtomPtr = AtomRegistry.Find(AtomUid);
	AAtomBase* Atom = AtomPtr ? AtomPtr->Get() : nullptr;
	UE_LOG(LogChemicalBondDirector, Warning,
		TEXT("[Game:Director] TerminateAtomByUid executing. Atom=%s AtomUid=%s BondCount=%d"),
		*GetNameSafe(Atom),
		*AtomUid.ToString(),
		Atom ? Atom->GetBondUids().Num() : 0);

	for (auto CandidateIt = ConnectionCandidates.CreateIterator(); CandidateIt; ++CandidateIt)
	{
		if (CandidateIt.Key().FirstAtomUid == AtomUid || CandidateIt.Key().SecondAtomUid == AtomUid)
		{
			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Connection] Remove candidate because atom is terminating. AtomUid=%s Pair=(%s,%s)"),
				*AtomUid.ToString(),
				*CandidateIt.Key().FirstAtomUid.ToString(),
				*CandidateIt.Key().SecondAtomUid.ToString());
			CandidateIt.RemoveCurrent();
		}
	}

	const TArray<FGuid> BondUidsToCut = GetAtomBondUidsForTermination(AtomUid, Atom);
	for (const FGuid& BondUid : BondUidsToCut)
	{
		CutBond(BondUid);
	}

	AtomRegistry.Remove(AtomUid);
	LockedAtomUids.Remove(AtomUid);
	DemotedRingAtomUids.Remove(AtomUid);
	CompletedRingAtomUids.Remove(AtomUid);
	if (Atom)
	{
		Atom->ClearAtomUid();
	}

	AssertBondRegistryConsistency();
	MarkPlayerMoleculeDirty();
	return true;
}

bool AChemicalBondGameDirector::IsAtomRegistered(FGuid AtomUid) const
{
	const TWeakObjectPtr<AAtomBase>* AtomPtr = AtomRegistry.Find(AtomUid);
	return AtomPtr && AtomPtr->IsValid();
}

AAtomBase* AChemicalBondGameDirector::GetAtomByUid(FGuid AtomUid) const
{
	const TWeakObjectPtr<AAtomBase>* AtomPtr = AtomRegistry.Find(AtomUid);
	return AtomPtr ? AtomPtr->Get() : nullptr;
}

TArray<FGuid> AChemicalBondGameDirector::GetAllAtomUids() const
{
	TArray<FGuid> AtomUids;
	AtomRegistry.GetKeys(AtomUids);
	return AtomUids;
}

int32 AChemicalBondGameDirector::PruneStaleAtomRegistryEntries()
{
	TArray<FGuid> StaleAtomUids;
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		if (!AtomPair.Key.IsValid() || !AtomPair.Value.IsValid())
		{
			StaleAtomUids.Add(AtomPair.Key);
		}
	}

	int32 PrunedCount = 0;
	for (const FGuid& AtomUid : StaleAtomUids)
	{
		if (DoesBondRegistryReferenceAtom(AtomUid))
		{
			UE_LOG(LogChemicalBondDirector, Error,
				TEXT("[Game:Director] Stale atom registry entry still has registered bonds and cannot be pruned. AtomUid=%s"),
				*AtomUid.ToString());
			continue;
		}

		AtomRegistry.Remove(AtomUid);
		ClearTransientReferencesForAtom(AtomUid);
		++PrunedCount;

		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] Pruned stale atom registry entry without bonds. AtomUid=%s"),
			*AtomUid.ToString());
	}

	return PrunedCount;
}

bool AChemicalBondGameDirector::DoesBondRegistryReferenceAtom(FGuid AtomUid) const
{
	if (!AtomUid.IsValid())
	{
		return false;
	}

	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FChemicalBondRegistryRecord& Record = BondPair.Value;
		if (Record.AtomAUid == AtomUid || Record.AtomBUid == AtomUid)
		{
			return true;
		}
	}

	return false;
}

void AChemicalBondGameDirector::ClearTransientReferencesForAtom(FGuid AtomUid)
{
	for (auto CandidateIt = ConnectionCandidates.CreateIterator(); CandidateIt; ++CandidateIt)
	{
		if (PairKeyContainsAtomUid(CandidateIt.Key(), AtomUid))
		{
			CandidateIt.RemoveCurrent();
		}
	}

	for (int32 DecisionIndex = DecisionQueue.Num() - 1; DecisionIndex >= 0; --DecisionIndex)
	{
		FAtomDecisionRequest& Request = DecisionQueue[DecisionIndex];
		if (!PairKeyContainsAtomUid(Request.PairKey, AtomUid))
		{
			continue;
		}

		if (AAtomBase* FreeAtom = Request.FreeAtom.Get())
		{
			if (FreeAtom->GetAtomState() == EAtomState::PendingDecision)
			{
				FreeAtom->SetAtomState(EAtomState::Free);
			}
			UnlockAtom(FreeAtom);
		}
		if (AAtomBase* ConnectedAtom = Request.ConnectedAtom.Get())
		{
			UnlockAtom(ConnectedAtom);
		}
		ReleaseDecisionPair(Request.PairKey);
		DecisionQueue.RemoveAt(DecisionIndex, 1, EAllowShrinking::No);
	}

	bool bShouldStartNextDecision = false;
	if (bHasActiveDecisionRequest && PairKeyContainsAtomUid(ActiveDecisionRequest.PairKey, AtomUid))
	{
		AAtomBase* ConnectedAtom = ActiveDecisionRequest.ConnectedAtom.Get();
		AAtomBase* FreeAtom = ActiveDecisionRequest.FreeAtom.Get();
		DestroyActiveDecisionWarningVisual();
		if (FreeAtom && FreeAtom->GetAtomState() == EAtomState::PendingDecision)
		{
			FreeAtom->SetAtomState(EAtomState::Free);
		}
		UnlockAtom(ConnectedAtom);
		UnlockAtom(FreeAtom);
		ReleaseDecisionPair(ActiveDecisionRequest.PairKey);
		bHasActiveDecisionRequest = false;
		ActiveDecisionRequest = FAtomDecisionRequest();
		bShouldStartNextDecision = true;
	}

	for (auto QueuedIt = QueuedDecisionPairKeys.CreateIterator(); QueuedIt; ++QueuedIt)
	{
		if (PairKeyContainsAtomUid(*QueuedIt, AtomUid))
		{
			QueuedIt.RemoveCurrent();
		}
	}

	LockedAtomUids.Remove(AtomUid);
	DemotedRingAtomUids.Remove(AtomUid);
	CompletedRingAtomUids.Remove(AtomUid);
	RigidGroupLocalTransforms.Remove(AtomUid);
	for (TPair<FGuid, TMap<FGuid, FTransform>>& GroupPair : RigidGroupLocalTransforms)
	{
		GroupPair.Value.Remove(AtomUid);
	}

	for (int32 CandidateIndex = RingDecisionQueue.Num() - 1; CandidateIndex >= 0; --CandidateIndex)
	{
		if (RingCandidateContainsAtomUid(RingDecisionQueue[CandidateIndex], AtomUid))
		{
			RingDecisionQueue.RemoveAt(CandidateIndex, 1, EAllowShrinking::No);
		}
	}

	bool bShouldStartNextRingDecision = false;
	if (bHasActiveRingDecision && RingCandidateContainsAtomUid(ActiveRingCandidate, AtomUid))
	{
		bHasActiveRingDecision = false;
		ActiveRingCandidate = FChemicalBondRingCandidate();
		ActiveRingClosingBondUid.Invalidate();
		bShouldStartNextRingDecision = true;
	}

	if (bShouldStartNextDecision)
	{
		StartNextDecisionRequest();
	}
	if (bShouldStartNextRingDecision)
	{
		StartNextRingDecision();
	}
}

FGuid AChemicalBondGameDirector::LinkAtoms(
	AAtomBase* AtomA,
	AAtomBase* AtomB,
	EBondType BondType,
	int32 AtomASlotIndex,
	int32 AtomBSlotIndex)
{
	if (!AtomA)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] Cannot link atoms because AtomA is null. Director=%s"),
			*GetNameSafe(this));
		return FGuid();
	}

	if (!AtomB)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] Cannot link atoms because AtomB is null. AtomA=%s"),
			*GetNameSafe(AtomA));
		return FGuid();
	}

	if (AtomA == AtomB)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Director] Cannot link atom to itself. Atom=%s"),
			*GetNameSafe(AtomA));
		return FGuid();
	}

	const FGuid AtomAUid = SpawnAtom(AtomA);
	const FGuid AtomBUid = SpawnAtom(AtomB);
	if (!AtomAUid.IsValid() || !AtomBUid.IsValid())
	{
		return FGuid();
	}

	PruneStaleAtomRegistryEntries();
	AssertBondRegistryConsistency();

	const FGuid BondUid = GenerateUniqueBondUid();
	if (!AtomA->AddBondWithUid(BondUid, AtomB, BondType, AtomASlotIndex, AtomBSlotIndex))
	{
		return FGuid();
	}

	if (!AtomB->AddBondWithUid(BondUid, AtomA, BondType, AtomBSlotIndex, AtomASlotIndex))
	{
		AtomA->RemoveBondByUid(BondUid);
		return FGuid();
	}

	FChemicalBondRegistryRecord Record;
	Record.BondUid = BondUid;
	Record.BondType = BondType;
	Record.AtomAUid = AtomAUid;
	Record.AtomBUid = AtomBUid;
	Record.AtomA = AtomA;
	Record.AtomB = AtomB;
	Record.AtomASlotIndex = AtomASlotIndex;
	Record.AtomBSlotIndex = AtomBSlotIndex;

	BondRegistry.Add(BondUid, Record);
	AddBondUidToTypeList(BondUid, BondType);
	RefreshAtomBondLayouts(AtomA, AtomB);
	SpawnOrUpdateBondVisual(BondUid);
	AssertBondRegistryConsistency();
	MarkPlayerMoleculeDirty();
	return BondUid;
}

bool AChemicalBondGameDirector::CutBond(FGuid BondUid)
{
	if (!BondUid.IsValid())
	{
		return false;
	}

	PruneStaleAtomRegistryEntries();
	AssertBondRegistryConsistency();

	FChemicalBondRegistryRecord Record;
	if (!BondRegistry.RemoveAndCopyValue(BondUid, Record))
	{
		return false;
	}

	RemoveBondUidFromTypeList(BondUid, Record.BondType);
	DestroyBondVisual(BondUid);
	AAtomBase* AtomA = Record.AtomA.Get();
	AAtomBase* AtomB = Record.AtomB.Get();

	if (AtomA)
	{
		AtomA->RemoveBondByUid(BondUid);
	}

	if (AtomB)
	{
		AtomB->RemoveBondByUid(BondUid);
	}

	RefreshAtomBondLayouts(AtomA, AtomB);
	ApplyGentleRepulsion(AtomA, AtomB, TEXT("CutBond"));
	AssertBondRegistryConsistency();
	MarkPlayerMoleculeDirty();
	return true;
}

bool AChemicalBondGameDirector::IsBondRegistered(FGuid BondUid) const
{
	return BondRegistry.Contains(BondUid);
}

FChemicalBondRegistryRecord AChemicalBondGameDirector::GetBondRecord(FGuid BondUid, bool& bFound) const
{
	if (const FChemicalBondRegistryRecord* Record = BondRegistry.Find(BondUid))
	{
		bFound = true;
		return *Record;
	}

	bFound = false;
	return FChemicalBondRegistryRecord();
}

TArray<FGuid> AChemicalBondGameDirector::GetBondUidsByType(EBondType BondType) const
{
	switch (BondType)
	{
	case EBondType::Single:
		return SingleBondUids;
	case EBondType::Double:
		return DoubleBondUids;
	case EBondType::Triple:
		return TripleBondUids;
	default:
		return TArray<FGuid>();
	}
}

TArray<FGuid> AChemicalBondGameDirector::GetAllBondUids() const
{
	TArray<FGuid> BondUids;
	BondRegistry.GetKeys(BondUids);
	return BondUids;
}

bool AChemicalBondGameDirector::ValidateBondRegistryConsistency(FString& OutError) const
{
	OutError.Reset();

	auto Fail = [&OutError](const FString& ErrorMessage)
	{
		OutError = ErrorMessage;
		return false;
	};

	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		if (!AtomPair.Key.IsValid())
		{
			return Fail(TEXT("AtomRegistry contains an invalid AtomUid."));
		}

		const AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom)
		{
			return Fail(FString::Printf(
				TEXT("AtomRegistry contains a stale atom pointer. AtomUid=%s"),
				*AtomPair.Key.ToString()));
		}

		if (Atom->GetAtomUid() != AtomPair.Key)
		{
			return Fail(FString::Printf(
				TEXT("AtomRegistry key does not match atom-local AtomUid. RegistryUid=%s Atom=%s AtomUid=%s"),
				*AtomPair.Key.ToString(),
				*GetNameSafe(Atom),
				*Atom->GetAtomUid().ToString()));
		}

		const TArray<FGuid> AtomBondUids = Atom->GetBondUids();
		const TArray<FBondRecord> AtomBonds = Atom->GetBonds();

		for (const FGuid& AtomBondUid : AtomBondUids)
		{
			if (!AtomBondUid.IsValid())
			{
				return Fail(FString::Printf(
					TEXT("Atom-local BondUids contains an invalid BondUid. Atom=%s AtomUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString()));
			}

			const FChemicalBondRegistryRecord* DirectorRecord = BondRegistry.Find(AtomBondUid);
			if (!DirectorRecord)
			{
				return Fail(FString::Printf(
					TEXT("Atom-local BondUid is missing from Director BondRegistry. Atom=%s AtomUid=%s BondUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString(),
					*AtomBondUid.ToString()));
			}

			if (DirectorRecord->AtomAUid != AtomPair.Key && DirectorRecord->AtomBUid != AtomPair.Key)
			{
				return Fail(FString::Printf(
					TEXT("Director BondRegistry record does not include atom that owns the local BondUid. Atom=%s AtomUid=%s BondUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString(),
					*AtomBondUid.ToString()));
			}

			FBondRecord LocalRecord;
			if (!TryGetAtomBondRecord(Atom, AtomBondUid, LocalRecord))
			{
				return Fail(FString::Printf(
					TEXT("Atom-local BondUids contains a BondUid without matching FBondRecord. Atom=%s AtomUid=%s BondUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString(),
					*AtomBondUid.ToString()));
			}
		}

		TSet<FGuid> SeenLocalBondUids;
		for (const FBondRecord& LocalRecord : AtomBonds)
		{
			if (!LocalRecord.BondUid.IsValid())
			{
				return Fail(FString::Printf(
					TEXT("Atom-local FBondRecord is missing a valid BondUid. Atom=%s AtomUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString()));
			}

			if (SeenLocalBondUids.Contains(LocalRecord.BondUid))
			{
				return Fail(FString::Printf(
					TEXT("Atom-local FBondRecord contains duplicate BondUid. Atom=%s AtomUid=%s BondUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString(),
					*LocalRecord.BondUid.ToString()));
			}

			SeenLocalBondUids.Add(LocalRecord.BondUid);
			if (!AtomBondUids.Contains(LocalRecord.BondUid))
			{
				return Fail(FString::Printf(
					TEXT("Atom-local FBondRecord is missing from atom BondUids. Atom=%s AtomUid=%s BondUid=%s"),
					*GetNameSafe(Atom),
					*AtomPair.Key.ToString(),
					*LocalRecord.BondUid.ToString()));
			}
		}
	}

	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FGuid BondUid = BondPair.Key;
		const FChemicalBondRegistryRecord& DirectorRecord = BondPair.Value;

		if (!BondUid.IsValid() || !DirectorRecord.BondUid.IsValid() || BondUid != DirectorRecord.BondUid)
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry key does not match record BondUid. RegistryUid=%s RecordUid=%s"),
				*BondUid.ToString(),
				*DirectorRecord.BondUid.ToString()));
		}

		if (!DoesTypeListContainBond(BondUid, DirectorRecord.BondType))
		{
			return Fail(FString::Printf(
				TEXT("Director bond type list is missing registered BondUid. BondUid=%s BondType=%d"),
				*BondUid.ToString(),
				static_cast<int32>(DirectorRecord.BondType)));
		}

		if (DoesOtherTypeListContainBond(BondUid, DirectorRecord.BondType))
		{
			return Fail(FString::Printf(
				TEXT("Director bond type list contains BondUid under the wrong type. BondUid=%s BondType=%d"),
				*BondUid.ToString(),
				static_cast<int32>(DirectorRecord.BondType)));
		}

		const AAtomBase* AtomA = DirectorRecord.AtomA.Get();
		if (!AtomA)
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry has a stale AtomA pointer. BondUid=%s"),
				*BondUid.ToString()));
		}

		const AAtomBase* AtomB = DirectorRecord.AtomB.Get();
		if (!AtomB)
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry has a stale AtomB pointer. BondUid=%s"),
				*BondUid.ToString()));
		}

		if (AtomA->GetAtomUid() != DirectorRecord.AtomAUid || AtomB->GetAtomUid() != DirectorRecord.AtomBUid)
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry atom UID does not match atom-local UID. BondUid=%s AtomA=%s AtomAUid=%s RecordAtomAUid=%s AtomB=%s AtomBUid=%s RecordAtomBUid=%s"),
				*BondUid.ToString(),
				*GetNameSafe(AtomA),
				*AtomA->GetAtomUid().ToString(),
				*DirectorRecord.AtomAUid.ToString(),
				*GetNameSafe(AtomB),
				*AtomB->GetAtomUid().ToString(),
				*DirectorRecord.AtomBUid.ToString()));
		}

		FBondRecord AtomARecord;
		if (!TryGetAtomBondRecord(AtomA, BondUid, AtomARecord) || !AtomA->GetBondUids().Contains(BondUid))
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry record is missing from AtomA local data. AtomA=%s BondUid=%s"),
				*GetNameSafe(AtomA),
				*BondUid.ToString()));
		}

		FBondRecord AtomBRecord;
		if (!TryGetAtomBondRecord(AtomB, BondUid, AtomBRecord) || !AtomB->GetBondUids().Contains(BondUid))
		{
			return Fail(FString::Printf(
				TEXT("Director BondRegistry record is missing from AtomB local data. AtomB=%s BondUid=%s"),
				*GetNameSafe(AtomB),
				*BondUid.ToString()));
		}

		if (AtomARecord.BondType != DirectorRecord.BondType
			|| AtomARecord.MySlotIndex != DirectorRecord.AtomASlotIndex
			|| AtomARecord.PartnerSlotIndex != DirectorRecord.AtomBSlotIndex
			|| AtomARecord.PartnerAtom.Get() != AtomB)
		{
			return Fail(FString::Printf(
				TEXT("AtomA local FBondRecord does not match Director BondRegistry. AtomA=%s BondUid=%s"),
				*GetNameSafe(AtomA),
				*BondUid.ToString()));
		}

		if (AtomBRecord.BondType != DirectorRecord.BondType
			|| AtomBRecord.MySlotIndex != DirectorRecord.AtomBSlotIndex
			|| AtomBRecord.PartnerSlotIndex != DirectorRecord.AtomASlotIndex
			|| AtomBRecord.PartnerAtom.Get() != AtomA)
		{
			return Fail(FString::Printf(
				TEXT("AtomB local FBondRecord does not match Director BondRegistry. AtomB=%s BondUid=%s"),
				*GetNameSafe(AtomB),
				*BondUid.ToString()));
		}
	}

	auto ValidateTypeList = [this, &Fail](const TArray<FGuid>& BondUids, EBondType ExpectedType)
	{
		TSet<FGuid> SeenBondUids;
		for (const FGuid& BondUid : BondUids)
		{
			if (!BondUid.IsValid())
			{
				return Fail(FString::Printf(
					TEXT("Director bond type list contains an invalid BondUid. BondType=%d"),
					static_cast<int32>(ExpectedType)));
			}

			if (SeenBondUids.Contains(BondUid))
			{
				return Fail(FString::Printf(
					TEXT("Director bond type list contains duplicate BondUid. BondUid=%s BondType=%d"),
					*BondUid.ToString(),
					static_cast<int32>(ExpectedType)));
			}

			SeenBondUids.Add(BondUid);

			const FChemicalBondRegistryRecord* DirectorRecord = BondRegistry.Find(BondUid);
			if (!DirectorRecord)
			{
				return Fail(FString::Printf(
					TEXT("Director bond type list references a BondUid missing from BondRegistry. BondUid=%s BondType=%d"),
					*BondUid.ToString(),
					static_cast<int32>(ExpectedType)));
			}

			if (DirectorRecord->BondType != ExpectedType)
			{
				return Fail(FString::Printf(
					TEXT("Director bond type list references a BondUid with mismatched type. BondUid=%s ExpectedType=%d ActualType=%d"),
					*BondUid.ToString(),
					static_cast<int32>(ExpectedType),
					static_cast<int32>(DirectorRecord->BondType)));
			}
		}

		return true;
	};

	if (!ValidateTypeList(SingleBondUids, EBondType::Single))
	{
		return false;
	}

	if (!ValidateTypeList(DoubleBondUids, EBondType::Double))
	{
		return false;
	}

	if (!ValidateTypeList(TripleBondUids, EBondType::Triple))
	{
		return false;
	}

	return true;
}

void AChemicalBondGameDirector::AssertBondRegistryConsistency()
{
	if (!bDirectorStarted)
	{
		return;
	}

	FString ErrorMessage;
	if (ValidateBondRegistryConsistency(ErrorMessage))
	{
		return;
	}

	HandleBondRegistryMismatch(ErrorMessage);
}

void AChemicalBondGameDirector::HandleAtomProximityEnter(AAtomBase* AtomA, AAtomBase* AtomB)
{
	FString Reason;
	if (!CanAtomsStartConnection(AtomA, AtomB, Reason))
	{
		UE_LOG(LogChemicalBondDirector, Verbose,
			TEXT("[Game:Connection] Ignore proximity enter. AtomA=%s AtomB=%s Reason=%s"),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB),
			*Reason);
		if (AtomA && AtomB
			&& !AtomA->IsInteractionCoolingDown()
			&& !AtomB->IsInteractionCoolingDown()
			&& Reason != TEXT("Same atom")
			&& Reason != TEXT("Atom UID is invalid")
			&& Reason != TEXT("Atom locked")
			&& Reason != TEXT("Pending decision")
			&& Reason != TEXT("Both atoms are already in player group"))
		{
			ApplyGentleRepulsion(AtomA, AtomB, TEXT("ImmediateUnconnectableOverlap"));
		}
		return;
	}

	const FAtomInteractionPairKey PairKey = MakePairKey(AtomA, AtomB);
	if (!PairKey.IsValid() || ConnectionCandidates.Contains(PairKey) || QueuedDecisionPairKeys.Contains(PairKey))
	{
		return;
	}

	FAtomConnectionCandidate Candidate;
	Candidate.PairKey = PairKey;
	Candidate.AtomA = AtomA;
	Candidate.AtomB = AtomB;
	Candidate.RemainingConfirmationSeconds = ContactConfirmationSeconds;
	ConnectionCandidates.Add(PairKey, Candidate);

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Connection] Candidate created. AtomA=%s AtomB=%s Pair=(%s,%s) ConfirmSeconds=%.2f"),
		*GetNameSafe(AtomA),
		*GetNameSafe(AtomB),
		*PairKey.FirstAtomUid.ToString(),
		*PairKey.SecondAtomUid.ToString(),
		ContactConfirmationSeconds);
}

void AChemicalBondGameDirector::HandleAtomProximityExit(AAtomBase* AtomA, AAtomBase* AtomB)
{
	const FAtomInteractionPairKey PairKey = MakePairKey(AtomA, AtomB);
	if (!PairKey.IsValid())
	{
		return;
	}

	if (ConnectionCandidates.Remove(PairKey) > 0)
	{
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Candidate removed by end overlap. AtomA=%s AtomB=%s Pair=(%s,%s)"),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB),
			*PairKey.FirstAtomUid.ToString(),
			*PairKey.SecondAtomUid.ToString());
	}
}

bool AChemicalBondGameDirector::HandleDecisionConfirmInput()
{
	// 成环决策优先于普通连接决策处理空格输入。
	if (bHasActiveRingDecision)
	{
		return HandleRingDecisionConfirmInput();
	}

	if (!bHasActiveDecisionRequest)
	{
		StartNextDecisionRequest();
	}

	if (!bHasActiveDecisionRequest)
	{
		UE_LOG(LogChemicalBondDirector, Verbose,
			TEXT("[Game:Connection] Confirm input ignored because no active decision exists."));
		return false;
	}

	return TryAdvanceActiveDecisionBond();
}

bool AChemicalBondGameDirector::HandleDecisionRejectInput()
{
	// 成环决策优先于普通连接决策处理 F 输入。
	if (bHasActiveRingDecision)
	{
		return HandleRingDecisionRejectInput();
	}

	if (!bHasActiveDecisionRequest)
	{
		UE_LOG(LogChemicalBondDirector, Verbose,
			TEXT("[Game:Connection] Reject input ignored because no active decision exists."));
		return false;
	}

	FinishActiveDecision(true);
	return true;
}

void AChemicalBondGameDirector::MarkAtomAsPlayerConnected(AAtomBase* Atom)
{
	if (!Atom)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Connection] Cannot mark player group because Atom is null."));
		return;
	}

	SpawnAtom(Atom);
	SetAtomGroupState(Atom, EAtomState::PlayerConnected);
	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Connection] Mark atom group as PlayerConnected. RootAtom=%s AtomUid=%s"),
		*GetNameSafe(Atom),
		*Atom->GetAtomUid().ToString());
	MarkPlayerMoleculeDirty();
}

void AChemicalBondGameDirector::BindAtomConnectionEvents(AAtomBase* Atom)
{
	if (!Atom)
	{
		return;
	}

	Atom->OnProximityEnter.AddUniqueDynamic(this, &AChemicalBondGameDirector::HandleAtomProximityEnter);
	Atom->OnProximityExit.AddUniqueDynamic(this, &AChemicalBondGameDirector::HandleAtomProximityExit);
}

FAtomInteractionPairKey AChemicalBondGameDirector::MakePairKey(AAtomBase* AtomA, AAtomBase* AtomB)
{
	FAtomInteractionPairKey PairKey;
	if (!AtomA || !AtomB || AtomA == AtomB)
	{
		return PairKey;
	}

	const FGuid AtomAUid = SpawnAtom(AtomA);
	const FGuid AtomBUid = SpawnAtom(AtomB);
	if (!AtomAUid.IsValid() || !AtomBUid.IsValid() || AtomAUid == AtomBUid)
	{
		return PairKey;
	}

	const FString AtomAKey = AtomAUid.ToString(EGuidFormats::Digits);
	const FString AtomBKey = AtomBUid.ToString(EGuidFormats::Digits);
	if (AtomAKey.Compare(AtomBKey) <= 0)
	{
		PairKey.FirstAtomUid = AtomAUid;
		PairKey.SecondAtomUid = AtomBUid;
	}
	else
	{
		PairKey.FirstAtomUid = AtomBUid;
		PairKey.SecondAtomUid = AtomAUid;
	}

	return PairKey;
}

void AChemicalBondGameDirector::ProcessConnectionCandidates(float DeltaSeconds)
{
	TArray<FAtomConnectionCandidate> CandidatesToSettle;
	for (auto CandidateIt = ConnectionCandidates.CreateIterator(); CandidateIt; ++CandidateIt)
	{
		FAtomConnectionCandidate& Candidate = CandidateIt.Value();
		Candidate.RemainingConfirmationSeconds -= DeltaSeconds;
		if (Candidate.RemainingConfirmationSeconds <= 0.f)
		{
			CandidatesToSettle.Add(Candidate);
			CandidateIt.RemoveCurrent();
		}
	}

	for (const FAtomConnectionCandidate& Candidate : CandidatesToSettle)
	{
		SettleConnectionCandidate(Candidate);
	}
}

void AChemicalBondGameDirector::ProcessActiveDecision(float DeltaSeconds)
{
	if (!bHasActiveDecisionRequest)
	{
		StartNextDecisionRequest();
		return;
	}

	ActiveDecisionRequest.RemainingDecisionSeconds -= DeltaSeconds;
	if (ActiveDecisionRequest.RemainingDecisionSeconds <= 0.f)
	{
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Decision timed out. Pair=(%s,%s) HasBond=%d"),
			*ActiveDecisionRequest.PairKey.FirstAtomUid.ToString(),
			*ActiveDecisionRequest.PairKey.SecondAtomUid.ToString(),
			ActiveDecisionRequest.bHasFormedBond ? 1 : 0);
		FinishActiveDecision(false);
	}
}

void AChemicalBondGameDirector::ProcessPhysicalConnections(float DeltaSeconds)
{
	if (DeltaSeconds <= 0.f)
	{
		return;
	}

	TSet<FAtomInteractionPairKey> HandledPairKeys;
	ProcessPlayerRigidGroups(HandledPairKeys);

	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FChemicalBondRegistryRecord& Record = BondPair.Value;
		ApplyPairConstraintIfUnhandled(Record.AtomA.Get(), Record.AtomB.Get(), HandledPairKeys, TEXT("Bond"));
	}

	if (bHasActiveDecisionRequest)
	{
		const FAtomInteractionPairKey PairKey = MakePairKey(
			ActiveDecisionRequest.ConnectedAtom.Get(),
			ActiveDecisionRequest.FreeAtom.Get());
		if (!HandledPairKeys.Contains(PairKey))
		{
			ApplyConnectionPullConstraint(
				ActiveDecisionRequest.ConnectedAtom.Get(),
				ActiveDecisionRequest.FreeAtom.Get(),
				TEXT("ActiveDecision"));
		}
	}

	for (const FAtomDecisionRequest& QueuedRequest : DecisionQueue)
	{
		const FAtomInteractionPairKey PairKey = MakePairKey(
			QueuedRequest.ConnectedAtom.Get(),
			QueuedRequest.FreeAtom.Get());
		if (!HandledPairKeys.Contains(PairKey))
		{
			ApplyConnectionPullConstraint(
				QueuedRequest.ConnectedAtom.Get(),
				QueuedRequest.FreeAtom.Get(),
				TEXT("QueuedDecision"));
		}
	}
}

void AChemicalBondGameDirector::ConstrainRegisteredAtomsToGameplayPlane() const
{
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		if (AAtomBase* Atom = AtomPair.Value.Get())
		{
			Atom->ConstrainToGameplayPlane();
		}
	}
}

void AChemicalBondGameDirector::ProcessPlayerRigidGroups(TSet<FAtomInteractionPairKey>& OutHandledPairKeys)
{
	TSet<FGuid> CurrentAnchorUids;

	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		AAtomBase* AnchorAtom = AtomPair.Value.Get();
		if (!AnchorAtom || !AnchorAtom->IsPlayerControlled())
		{
			continue;
		}
		AnchorAtom->ConstrainToGameplayPlane();

		TArray<AAtomBase*> GroupAtoms;
		TSet<FAtomInteractionPairKey> GroupPairKeys;
		CollectBondedGroup(AnchorAtom, GroupAtoms, GroupPairKeys);
		if (GroupAtoms.Num() <= 1)
		{
			RigidGroupLocalTransforms.Remove(AtomPair.Key);
			continue;
		}

		CurrentAnchorUids.Add(AtomPair.Key);
		OutHandledPairKeys.Append(GroupPairKeys);

		TMap<FGuid, FTransform>& LocalTransforms = RigidGroupLocalTransforms.FindOrAdd(AtomPair.Key);
		TSet<FGuid> CurrentGroupUids;
		bool bNeedsLayoutCapture = LocalTransforms.IsEmpty();

		for (AAtomBase* GroupAtom : GroupAtoms)
		{
			if (!GroupAtom || GroupAtom == AnchorAtom)
			{
				continue;
			}

			const FGuid GroupAtomUid = GroupAtom->GetAtomUid();
			CurrentGroupUids.Add(GroupAtomUid);
			if (!LocalTransforms.Contains(GroupAtomUid))
			{
				bNeedsLayoutCapture = true;
			}
		}

		for (auto LocalIt = LocalTransforms.CreateIterator(); LocalIt; ++LocalIt)
		{
			if (!CurrentGroupUids.Contains(LocalIt.Key()))
			{
				LocalIt.RemoveCurrent();
				bNeedsLayoutCapture = true;
			}
		}

		if (bNeedsLayoutCapture)
		{
			for (const FAtomInteractionPairKey& PairKey : GroupPairKeys)
			{
				ApplyFixedConnectionConstraint(
					GetAtomByUid(PairKey.FirstAtomUid),
					GetAtomByUid(PairKey.SecondAtomUid),
					TEXT("RigidGroupInitialize"));
			}

			const FTransform AnchorTransform = AnchorAtom->GetActorTransform();
			LocalTransforms.Reset();
			for (AAtomBase* GroupAtom : GroupAtoms)
			{
				if (!GroupAtom || GroupAtom == AnchorAtom)
				{
					continue;
				}

				GroupAtom->ConstrainToGameplayPlane();
				LocalTransforms.Add(
					GroupAtom->GetAtomUid(),
					GroupAtom->GetActorTransform().GetRelativeTransform(AnchorTransform));
				StopConstrainedAtomMotion(GroupAtom);
			}

			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Connection] Rigid player group layout captured. Anchor=%s AtomCount=%d PairCount=%d"),
				*GetNameSafe(AnchorAtom),
				GroupAtoms.Num(),
				GroupPairKeys.Num());
			continue;
		}

		const FTransform AnchorTransform = AnchorAtom->GetActorTransform();
		for (AAtomBase* GroupAtom : GroupAtoms)
		{
			if (!GroupAtom || GroupAtom == AnchorAtom)
			{
				continue;
			}

			const FTransform* LocalTransform = LocalTransforms.Find(GroupAtom->GetAtomUid());
			if (!LocalTransform)
			{
				continue;
			}

			const FTransform TargetTransform = (*LocalTransform) * AnchorTransform;
			const FVector TargetLocation = ChemicalBondGameplayPlane::ProjectLocation(TargetTransform.GetLocation());
			GroupAtom->SetActorLocationAndRotation(
				TargetLocation,
				TargetTransform.GetRotation(),
				false);
			StopConstrainedAtomMotion(GroupAtom);
		}
	}

	for (auto AnchorIt = RigidGroupLocalTransforms.CreateIterator(); AnchorIt; ++AnchorIt)
	{
		if (!CurrentAnchorUids.Contains(AnchorIt.Key()))
		{
			AnchorIt.RemoveCurrent();
		}
	}
}

void AChemicalBondGameDirector::CollectBondedGroup(
	AAtomBase* RootAtom,
	TArray<AAtomBase*>& OutAtoms,
	TSet<FAtomInteractionPairKey>& OutPairKeys)
{
	OutAtoms.Reset();
	OutPairKeys.Reset();
	if (!RootAtom)
	{
		return;
	}

	TArray<AAtomBase*> PendingAtoms;
	TSet<FGuid> VisitedAtomUids;
	PendingAtoms.Add(RootAtom);

	while (!PendingAtoms.IsEmpty())
	{
		AAtomBase* Atom = PendingAtoms.Pop();
		if (!Atom)
		{
			continue;
		}

		const FGuid AtomUid = Atom->GetAtomUid();
		if (!AtomUid.IsValid() || VisitedAtomUids.Contains(AtomUid))
		{
			continue;
		}

		VisitedAtomUids.Add(AtomUid);
		OutAtoms.Add(Atom);

		for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
		{
			const FChemicalBondRegistryRecord& Record = BondPair.Value;
			if (Record.AtomAUid == AtomUid)
			{
				AddConnectedNeighbor(Atom, Record.AtomB.Get(), PendingAtoms, VisitedAtomUids, OutPairKeys);
			}
			else if (Record.AtomBUid == AtomUid)
			{
				AddConnectedNeighbor(Atom, Record.AtomA.Get(), PendingAtoms, VisitedAtomUids, OutPairKeys);
			}
		}

	}
}

void AChemicalBondGameDirector::AddConnectedNeighbor(
	AAtomBase* SourceAtom,
	AAtomBase* NeighborAtom,
	TArray<AAtomBase*>& PendingAtoms,
	TSet<FGuid>& VisitedAtomUids,
	TSet<FAtomInteractionPairKey>& OutPairKeys)
{
	const FAtomInteractionPairKey PairKey = MakePairKey(SourceAtom, NeighborAtom);
	if (PairKey.IsValid())
	{
		OutPairKeys.Add(PairKey);
	}

	if (!NeighborAtom)
	{
		return;
	}

	const FGuid NeighborUid = NeighborAtom->GetAtomUid();
	if (NeighborUid.IsValid() && !VisitedAtomUids.Contains(NeighborUid))
	{
		PendingAtoms.Add(NeighborAtom);
	}
}

bool AChemicalBondGameDirector::ApplyPairConstraintIfUnhandled(
	AAtomBase* AtomA,
	AAtomBase* AtomB,
	const TSet<FAtomInteractionPairKey>& HandledPairKeys,
	const TCHAR* Reason)
{
	const FAtomInteractionPairKey PairKey = MakePairKey(AtomA, AtomB);
	if (!PairKey.IsValid() || HandledPairKeys.Contains(PairKey))
	{
		return false;
	}

	ApplyFixedConnectionConstraint(AtomA, AtomB, Reason);
	return true;
}

void AChemicalBondGameDirector::ApplyConnectionPullConstraint(
	AAtomBase* AnchorAtom,
	AAtomBase* PulledAtom,
	const TCHAR* Reason)
{
	if (!AnchorAtom)
	{
		return;
	}

	if (!PulledAtom)
	{
		return;
	}

	if (AnchorAtom == PulledAtom)
	{
		return;
	}

	int32 AnchorSlot = INDEX_NONE;
	int32 PulledSlot = INDEX_NONE;
	if (FindClosestFreeSlotPair(AnchorAtom, PulledAtom, AnchorSlot, PulledSlot))
	{
		AlignAtomsForSlotConnection(AnchorAtom, AnchorSlot, PulledAtom, PulledSlot, Reason);
		return;
	}

	AnchorAtom->ConstrainToGameplayPlane();
	PulledAtom->ConstrainToGameplayPlane();

	const FVector AnchorLocation = ChemicalBondGameplayPlane::ProjectLocation(AnchorAtom->GetActorLocation());
	const FVector PulledLocation = ChemicalBondGameplayPlane::ProjectLocation(PulledAtom->GetActorLocation());
	FVector Direction = PulledLocation - AnchorLocation;
	Direction = ChemicalBondGameplayPlane::ProjectVector(Direction);
	if (Direction.IsNearlyZero())
	{
		Direction = AnchorAtom->GetActorForwardVector();
		Direction = ChemicalBondGameplayPlane::ProjectVector(Direction);
		if (Direction.IsNearlyZero())
		{
			Direction = FVector::ForwardVector;
		}
	}
	Direction = Direction.GetSafeNormal();

	FVector TargetLocation = AnchorLocation + Direction * TemporaryConnectedAtomDistance;
	TargetLocation = ChemicalBondGameplayPlane::ProjectLocation(TargetLocation);

	FVector PullDelta = TargetLocation - PulledLocation;
	PullDelta = ChemicalBondGameplayPlane::ProjectVector(PullDelta);
	if (PullDelta.SizeSquared() <= FMath::Square(ConnectionConstraintTolerance))
	{
		return;
	}

	const FVector Step = PullDelta.GetClampedToMaxSize(PendingConnectionMaxStep) * PendingConnectionPullAlpha;
	PulledAtom->SetActorLocation(ChemicalBondGameplayPlane::ProjectLocation(PulledLocation + Step), false);
	StopConstrainedAtomMotion(PulledAtom);

	UE_LOG(LogChemicalBondDirector, VeryVerbose,
		TEXT("[Game:Connection] Connection pull applied. Anchor=%s Pulled=%s Reason=%s Remaining=%.2f Step=%.2f"),
		*GetNameSafe(AnchorAtom),
		*GetNameSafe(PulledAtom),
		Reason ? Reason : TEXT("Unknown"),
		PullDelta.Size(),
		Step.Size());
}

bool AChemicalBondGameDirector::FindClosestFreeSlotPair(
	AAtomBase* AtomA,
	AAtomBase* AtomB,
	int32& OutAtomASlot,
	int32& OutAtomBSlot) const
{
	OutAtomASlot = INDEX_NONE;
	OutAtomBSlot = INDEX_NONE;
	if (!AtomA)
	{
		return false;
	}
	if (!AtomB)
	{
		return false;
	}

	float BestDistanceSquared = TNumericLimits<float>::Max();
	for (int32 AtomASlot = 0; AtomASlot < AtomA->GetTotalSlotCount(); ++AtomASlot)
	{
		if (AtomA->IsSlotOccupied(AtomASlot))
		{
			continue;
		}

		const FVector AtomASlotLocation = AtomA->GetSlotWorldLocation(AtomASlot);
		for (int32 AtomBSlot = 0; AtomBSlot < AtomB->GetTotalSlotCount(); ++AtomBSlot)
		{
			if (AtomB->IsSlotOccupied(AtomBSlot))
			{
				continue;
			}

			const float DistanceSquared = FVector::DistSquared(
				AtomASlotLocation,
				AtomB->GetSlotWorldLocation(AtomBSlot));
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				OutAtomASlot = AtomASlot;
				OutAtomBSlot = AtomBSlot;
			}
		}
	}

	return OutAtomASlot != INDEX_NONE && OutAtomBSlot != INDEX_NONE;
}

void AChemicalBondGameDirector::AlignAtomsForSlotConnection(
	AAtomBase* AtomA,
	int32 AtomASlot,
	AAtomBase* AtomB,
	int32 AtomBSlot,
	const TCHAR* Reason)
{
	if (!AtomA)
	{
		return;
	}
	if (!AtomB)
	{
		return;
	}
	if (AtomA == AtomB)
	{
		return;
	}

	AtomA->ConstrainToGameplayPlane();
	AtomB->ConstrainToGameplayPlane();

	const bool bAtomAAnchored =
		AtomA->IsPlayerControlled()
		|| (AtomA->GetAtomState() == EAtomState::PlayerConnected && AtomB->GetAtomState() != EAtomState::PlayerConnected);
	const bool bAtomBAnchored =
		AtomB->IsPlayerControlled()
		|| (AtomB->GetAtomState() == EAtomState::PlayerConnected && AtomA->GetAtomState() != EAtomState::PlayerConnected);

	FVector DirectionFromAToB = FVector::ForwardVector;
	if (bAtomAAnchored && !bAtomBAnchored)
	{
		DirectionFromAToB = AtomA->GetSlotWorldOffset(AtomASlot);
	}
	else if (bAtomBAnchored && !bAtomAAnchored)
	{
		DirectionFromAToB = -AtomB->GetSlotWorldOffset(AtomBSlot);
	}
	else
	{
		DirectionFromAToB = FindWeightedFacingDirection(AtomA, AtomASlot, AtomB, AtomBSlot);
	}

	DirectionFromAToB = ChemicalBondGameplayPlane::ProjectVector(DirectionFromAToB);
	if (DirectionFromAToB.IsNearlyZero())
	{
		DirectionFromAToB =
			ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation()) -
			ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation());
	}
	if (DirectionFromAToB.IsNearlyZero())
	{
		DirectionFromAToB = FVector::ForwardVector;
	}
	DirectionFromAToB = DirectionFromAToB.GetSafeNormal();

	auto SetAtomYawForSlotDirection = [](AAtomBase* Atom, int32 SlotIndex, FVector DesiredSlotDirection)
	{
		if (!Atom)
		{
			return;
		}

		const float DesiredSlotYaw = GetPlanarYawDegrees(DesiredSlotDirection);
		FRotator NewRotation = Atom->GetActorRotation();
		NewRotation.Pitch = 0.f;
		NewRotation.Roll = 0.f;
		NewRotation.Yaw = DesiredSlotYaw - Atom->GetSlotBaseAngleDegrees(SlotIndex);
		Atom->SetActorRotation(NewRotation);
		Atom->ConstrainToGameplayPlane();
	};

	if (bAtomAAnchored && !bAtomBAnchored)
	{
		SetAtomYawForSlotDirection(AtomB, AtomBSlot, -DirectionFromAToB);
	}
	else if (bAtomBAnchored && !bAtomAAnchored)
	{
		SetAtomYawForSlotDirection(AtomA, AtomASlot, DirectionFromAToB);
	}
	else
	{
		SetAtomYawForSlotDirection(AtomA, AtomASlot, DirectionFromAToB);
		SetAtomYawForSlotDirection(AtomB, AtomBSlot, -DirectionFromAToB);
	}

	const float TargetSlotDistance =
		(FMath::Max(AtomA->GetSlotConnectionDistance(), 1.f) + FMath::Max(AtomB->GetSlotConnectionDistance(), 1.f)) * 0.5f;
	const FVector TargetDelta = DirectionFromAToB * TargetSlotDistance;
	const FVector AtomASlotLocation = AtomA->GetSlotWorldLocation(AtomASlot);
	const FVector AtomBSlotLocation = AtomB->GetSlotWorldLocation(AtomBSlot);

	if (bAtomAAnchored && !bAtomBAnchored)
	{
		const FVector DesiredAtomBSlotLocation = AtomASlotLocation + TargetDelta;
		AtomB->SetActorLocation(
			ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation() + DesiredAtomBSlotLocation - AtomBSlotLocation),
			false);
		StopConstrainedAtomMotion(AtomB);
	}
	else if (bAtomBAnchored && !bAtomAAnchored)
	{
		const FVector DesiredAtomASlotLocation = AtomBSlotLocation - TargetDelta;
		AtomA->SetActorLocation(
			ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation() + DesiredAtomASlotLocation - AtomASlotLocation),
			false);
		StopConstrainedAtomMotion(AtomA);
	}
	else
	{
		const FVector CurrentDelta = AtomBSlotLocation - AtomASlotLocation;
		const FVector Correction = ChemicalBondGameplayPlane::ProjectVector(CurrentDelta - TargetDelta);
		const float AtomAMass = FMath::Max(AtomA->GetMass(), 1.f);
		const float AtomBMass = FMath::Max(AtomB->GetMass(), 1.f);
		const float TotalMass = AtomAMass + AtomBMass;
		const float AtomAMoveWeight = AtomBMass / TotalMass;
		const float AtomBMoveWeight = AtomAMass / TotalMass;
		AtomA->SetActorLocation(
			ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation() + Correction * AtomAMoveWeight),
			false);
		AtomB->SetActorLocation(
			ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation() - Correction * AtomBMoveWeight),
			false);
		StopConstrainedAtomMotion(AtomA);
		StopConstrainedAtomMotion(AtomB);
	}

	RefreshAtomBondLayouts(AtomA, AtomB);
	UE_LOG(LogChemicalBondDirector, VeryVerbose,
		TEXT("[Game:Connection] Slot alignment applied. AtomA=%s SlotA=%d AtomB=%s SlotB=%d Reason=%s"),
		*GetNameSafe(AtomA),
		AtomASlot,
		*GetNameSafe(AtomB),
		AtomBSlot,
		Reason ? Reason : TEXT("Unknown"));
}

void AChemicalBondGameDirector::RefreshAtomBondLayouts(AAtomBase* AtomA, AAtomBase* AtomB) const
{
	if (AtomA)
	{
		AtomA->NotifyBondLayoutChanged();
	}
	if (AtomB)
	{
		AtomB->NotifyBondLayoutChanged();
	}
}

void AChemicalBondGameDirector::UpdateInteractionRangeFillVisual()
{
	if (!InteractionRangeFillMesh)
	{
		return;
	}

	if (!bEnableInteractionRangeFillVisual)
	{
		InteractionRangeFillMesh->ClearAllMeshSections();
		InteractionRangeFillMesh->SetVisibility(false, true);
		return;
	}

	struct FRangeFillCircle
	{
		FVector2D Center = FVector2D::ZeroVector;
		float Radius = 0.f;
		EAtomInteractionRangeVisualState VisualState = EAtomInteractionRangeVisualState::FreeAvailable;
	};

	struct FRangeFillAngleInterval
	{
		float Start = 0.f;
		float End = 0.f;
	};

	TArray<FRangeFillCircle> FillCircles;
	FBox2D FillBounds(ForceInit);
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom)
		{
			continue;
		}

		const EAtomInteractionRangeVisualState VisualState = Atom->GetInteractionRangeVisualState();
		if (VisualState == EAtomInteractionRangeVisualState::NotApplicable)
		{
			continue;
		}

		const float Radius = FMath::Max(Atom->GetProximityRadius(), 0.f);
		if (Radius <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const FVector Location = ChemicalBondGameplayPlane::ProjectLocation(Atom->GetActorLocation());
		FRangeFillCircle Circle;
		Circle.Center = FVector2D(Location.X, Location.Y);
		Circle.Radius = Radius;
		Circle.VisualState = VisualState;
		FillCircles.Add(Circle);

		FillBounds += Circle.Center - FVector2D(Radius, Radius);
		FillBounds += Circle.Center + FVector2D(Radius, Radius);
	}

	if (FillCircles.IsEmpty() || !FillBounds.bIsValid)
	{
		InteractionRangeFillMesh->ClearAllMeshSections();
		InteractionRangeFillMesh->SetVisibility(false, true);
		return;
	}

	const float SegmentLength = FMath::Max(InteractionRangeFillBoundarySegmentLength, 2.f);
	const float BoundaryZ = InteractionRangeFillZOffset + 1.f;
	const float BoundaryThickness = FMath::Max(InteractionRangeBoundaryThickness, 0.f);
	UWorld* World = GetWorld();
	constexpr float TwoPi = 2.f * UE_PI;
	auto GetInteractionRangeVisualColor = [this](EAtomInteractionRangeVisualState VisualState)
	{
		switch (VisualState)
		{
		case EAtomInteractionRangeVisualState::Unavailable:
			return UnavailableInteractionRangeColor;
		case EAtomInteractionRangeVisualState::PlayerGroupAvailable:
			return InteractionRangeFillColor;
		case EAtomInteractionRangeVisualState::FreeAvailable:
		default:
			return FreeAtomInteractionRangeColor;
		}
	};

	auto GetComponentVisualColor = [&FillCircles, &GetInteractionRangeVisualColor](const TArray<int32>& Component)
	{
		EAtomInteractionRangeVisualState SelectedState = EAtomInteractionRangeVisualState::FreeAvailable;
		for (const int32 CircleIndex : Component)
		{
			const EAtomInteractionRangeVisualState VisualState = FillCircles[CircleIndex].VisualState;
			if (VisualState == EAtomInteractionRangeVisualState::PlayerGroupAvailable)
			{
				SelectedState = VisualState;
				break;
			}
			if (VisualState == EAtomInteractionRangeVisualState::Unavailable)
			{
				SelectedState = VisualState;
			}
		}
		return GetInteractionRangeVisualColor(SelectedState);
	};

	auto NormalizeAngle = [](float Angle)
	{
		float Result = FMath::Fmod(Angle, TwoPi);
		if (Result < 0.f)
		{
			Result += TwoPi;
		}
		return Result;
	};

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;

	Vertices.Reserve(FillCircles.Num() * 128);
	Triangles.Reserve(FillCircles.Num() * 384);
	Normals.Reserve(FillCircles.Num() * 128);
	UVs.Reserve(FillCircles.Num() * 128);
	VertexColors.Reserve(FillCircles.Num() * 128);
	Tangents.Reserve(FillCircles.Num() * 128);

	TArray<int32> ComponentIds;
	ComponentIds.Init(INDEX_NONE, FillCircles.Num());
	TArray<TArray<int32>> Components;
	for (int32 CircleIndex = 0; CircleIndex < FillCircles.Num(); ++CircleIndex)
	{
		if (ComponentIds[CircleIndex] != INDEX_NONE)
		{
			continue;
		}

		TArray<int32> Component;
		TArray<int32> Queue;
		Queue.Add(CircleIndex);
		ComponentIds[CircleIndex] = Components.Num();
		for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
		{
			const int32 CurrentIndex = Queue[QueueIndex];
			Component.Add(CurrentIndex);
			const FRangeFillCircle& CurrentCircle = FillCircles[CurrentIndex];
			for (int32 OtherIndex = 0; OtherIndex < FillCircles.Num(); ++OtherIndex)
			{
				if (ComponentIds[OtherIndex] != INDEX_NONE)
				{
					continue;
				}

				const FRangeFillCircle& OtherCircle = FillCircles[OtherIndex];
				const float Distance = FVector2D::Distance(CurrentCircle.Center, OtherCircle.Center);
				if (Distance <= CurrentCircle.Radius + OtherCircle.Radius)
				{
					ComponentIds[OtherIndex] = Components.Num();
					Queue.Add(OtherIndex);
				}
			}
		}
		Components.Add(Component);
	}

	const FTransform ComponentTransform = InteractionRangeFillMesh->GetComponentTransform();
	for (const TArray<int32>& Component : Components)
	{
		if (Component.IsEmpty())
		{
			continue;
		}

		FVector2D ComponentCenter = FVector2D::ZeroVector;
		float ComponentRadius = 0.f;
		const FLinearColor ComponentVisualColor = GetComponentVisualColor(Component);
		for (const int32 CircleIndex : Component)
		{
			const FRangeFillCircle& Circle = FillCircles[CircleIndex];
			ComponentCenter += Circle.Center;
			ComponentRadius = FMath::Max(ComponentRadius, Circle.Radius);
		}
		ComponentCenter /= static_cast<float>(Component.Num());

		TArray<FVector2D> BoundaryPoints;
		for (const int32 CircleIndex : Component)
		{
			const FRangeFillCircle& Circle = FillCircles[CircleIndex];
			TArray<FRangeFillAngleInterval> CoveredIntervals;
			bool bFullyCovered = false;

			for (const int32 OtherIndex : Component)
			{
				if (OtherIndex == CircleIndex)
				{
					continue;
				}

				const FRangeFillCircle& OtherCircle = FillCircles[OtherIndex];
				const FVector2D ToOther = OtherCircle.Center - Circle.Center;
				const float Distance = ToOther.Size();
				if (Distance <= KINDA_SMALL_NUMBER)
				{
					if (OtherCircle.Radius >= Circle.Radius)
					{
						bFullyCovered = true;
						break;
					}
					continue;
				}

				if (Distance + Circle.Radius <= OtherCircle.Radius + KINDA_SMALL_NUMBER)
				{
					bFullyCovered = true;
					break;
				}

				if (Distance >= Circle.Radius + OtherCircle.Radius || Distance + OtherCircle.Radius <= Circle.Radius)
				{
					continue;
				}

				const float BaseAngle = FMath::Atan2(ToOther.Y, ToOther.X);
				const float CoverageCosine = FMath::Clamp(
					(FMath::Square(Circle.Radius) + FMath::Square(Distance) - FMath::Square(OtherCircle.Radius)) /
						(2.f * Circle.Radius * Distance),
					-1.f,
					1.f);
				const float CoverageAngle = FMath::Acos(CoverageCosine);
				float Start = BaseAngle - CoverageAngle;
				float End = BaseAngle + CoverageAngle;
				while (Start < 0.f)
				{
					Start += TwoPi;
					End += TwoPi;
				}
				while (Start >= TwoPi)
				{
					Start -= TwoPi;
					End -= TwoPi;
				}

				if (End > TwoPi)
				{
					CoveredIntervals.Add({Start, TwoPi});
					CoveredIntervals.Add({0.f, End - TwoPi});
				}
				else
				{
					CoveredIntervals.Add({Start, End});
				}
			}

			if (bFullyCovered)
			{
				continue;
			}

			CoveredIntervals.Sort([](const FRangeFillAngleInterval& Left, const FRangeFillAngleInterval& Right)
			{
				return Left.Start < Right.Start;
			});

			TArray<FRangeFillAngleInterval> MergedIntervals;
			for (const FRangeFillAngleInterval& Interval : CoveredIntervals)
			{
				if (Interval.End <= Interval.Start)
				{
					continue;
				}

				if (MergedIntervals.IsEmpty() || Interval.Start > MergedIntervals.Last().End)
				{
					MergedIntervals.Add(Interval);
				}
				else
				{
					MergedIntervals.Last().End = FMath::Max(MergedIntervals.Last().End, Interval.End);
				}
			}

			TArray<FRangeFillAngleInterval> ExposedIntervals;
			if (MergedIntervals.IsEmpty())
			{
				ExposedIntervals.Add({0.f, TwoPi});
			}
			else
			{
				float Cursor = 0.f;
				for (const FRangeFillAngleInterval& Interval : MergedIntervals)
				{
					if (Interval.Start > Cursor)
					{
						ExposedIntervals.Add({Cursor, Interval.Start});
					}
					Cursor = FMath::Max(Cursor, Interval.End);
				}
				if (Cursor < TwoPi)
				{
					ExposedIntervals.Add({Cursor, TwoPi});
				}
			}

			for (const FRangeFillAngleInterval& ExposedInterval : ExposedIntervals)
			{
				const float ArcLength = (ExposedInterval.End - ExposedInterval.Start) * Circle.Radius;
				const int32 SegmentCount = FMath::Clamp(FMath::CeilToInt(ArcLength / SegmentLength), 8, 192);
				FVector2D PreviousBoundaryPoint = FVector2D::ZeroVector;
				bool bHasPreviousBoundaryPoint = false;
				for (int32 SegmentIndex = 0; SegmentIndex <= SegmentCount; ++SegmentIndex)
				{
					const float Alpha = static_cast<float>(SegmentIndex) / static_cast<float>(SegmentCount);
					const float Angle = FMath::Lerp(ExposedInterval.Start, ExposedInterval.End, Alpha);
					const FVector2D BoundaryPoint =
						Circle.Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Circle.Radius;
					BoundaryPoints.Add(BoundaryPoint);
					if (bEnableInteractionRangeBoundaryVisual && World && bHasPreviousBoundaryPoint)
					{
						const FLinearColor BoundaryLinearColor = GetInteractionRangeVisualColor(Circle.VisualState);
						DrawDebugLine(
							World,
							FVector(PreviousBoundaryPoint.X, PreviousBoundaryPoint.Y, BoundaryZ),
							FVector(BoundaryPoint.X, BoundaryPoint.Y, BoundaryZ),
							BoundaryLinearColor.ToFColor(true),
							false,
							0.f,
							0,
							BoundaryThickness);
					}
					PreviousBoundaryPoint = BoundaryPoint;
					bHasPreviousBoundaryPoint = true;
				}
			}
		}

		if (BoundaryPoints.Num() < 3)
		{
			continue;
		}

		BoundaryPoints.Sort([&ComponentCenter, NormalizeAngle](const FVector2D& Left, const FVector2D& Right)
		{
			const float LeftAngle = NormalizeAngle(FMath::Atan2(Left.Y - ComponentCenter.Y, Left.X - ComponentCenter.X));
			const float RightAngle = NormalizeAngle(FMath::Atan2(Right.Y - ComponentCenter.Y, Right.X - ComponentCenter.X));
			return LeftAngle < RightAngle;
		});

		TArray<FVector2D> UniqueBoundaryPoints;
		UniqueBoundaryPoints.Reserve(BoundaryPoints.Num());
		for (const FVector2D& BoundaryPoint : BoundaryPoints)
		{
			if (UniqueBoundaryPoints.IsEmpty() ||
				FVector2D::DistSquared(UniqueBoundaryPoints.Last(), BoundaryPoint) > FMath::Square(1.f))
			{
				UniqueBoundaryPoints.Add(BoundaryPoint);
			}
		}
		if (UniqueBoundaryPoints.Num() >= 2 &&
			FVector2D::DistSquared(UniqueBoundaryPoints[0], UniqueBoundaryPoints.Last()) <= FMath::Square(1.f))
		{
			UniqueBoundaryPoints.Pop();
		}
		if (UniqueBoundaryPoints.Num() < 3)
		{
			continue;
		}

		const int32 CenterIndex = Vertices.Num();
		const FVector WorldCenter(ComponentCenter.X, ComponentCenter.Y, InteractionRangeFillZOffset);
		Vertices.Add(ComponentTransform.InverseTransformPosition(WorldCenter));
		Normals.Add(FVector::UpVector);
		UVs.Add(FVector2D(0.5f, 0.5f));
		VertexColors.Add(ComponentVisualColor);
		Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));

		const int32 BoundaryStartIndex = Vertices.Num();
		for (const FVector2D& BoundaryPoint : UniqueBoundaryPoints)
		{
			const FVector WorldPoint(BoundaryPoint.X, BoundaryPoint.Y, InteractionRangeFillZOffset);
			Vertices.Add(ComponentTransform.InverseTransformPosition(WorldPoint));
			Normals.Add(FVector::UpVector);
			UVs.Add((BoundaryPoint - ComponentCenter) / FMath::Max(ComponentRadius * 2.f, 1.f) + FVector2D(0.5f, 0.5f));
			VertexColors.Add(ComponentVisualColor);
			Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
		}

		for (int32 BoundaryIndex = 0; BoundaryIndex < UniqueBoundaryPoints.Num(); ++BoundaryIndex)
		{
			const int32 FirstIndex = BoundaryStartIndex + BoundaryIndex;
			const int32 SecondIndex = BoundaryStartIndex + ((BoundaryIndex + 1) % UniqueBoundaryPoints.Num());
			Triangles.Add(CenterIndex);
			Triangles.Add(FirstIndex);
			Triangles.Add(SecondIndex);
			Triangles.Add(SecondIndex);
			Triangles.Add(FirstIndex);
			Triangles.Add(CenterIndex);
		}
	}

	if (Vertices.IsEmpty())
	{
		InteractionRangeFillMesh->ClearAllMeshSections();
		InteractionRangeFillMesh->SetVisibility(false, true);
		return;
	}

	if (InteractionRangeFillMaterialInstance)
	{
		InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("Color")), InteractionRangeFillColor);
		InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("BaseColor")), InteractionRangeFillColor);
		InteractionRangeFillMaterialInstance->SetVectorParameterValue(FName(TEXT("TintColor")), InteractionRangeFillColor);
		InteractionRangeFillMaterialInstance->SetScalarParameterValue(FName(TEXT("Opacity")), InteractionRangeFillColor.A);
	}

	InteractionRangeFillMesh->CreateMeshSection_LinearColor(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		VertexColors,
		Tangents,
		false);
	InteractionRangeFillMesh->SetVisibility(true, true);

	if (!bLoggedInteractionRangeFillVisual)
	{
		bLoggedInteractionRangeFillVisual = true;
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Presentation] Interaction range fill generated. CircleCount=%d ComponentCount=%d VertexCount=%d TriangleCount=%d SegmentLength=%.2f Material=%s"),
			FillCircles.Num(),
			Components.Num(),
			Vertices.Num(),
			Triangles.Num() / 3,
			SegmentLength,
			*GetNameSafe(InteractionRangeFillMesh->GetMaterial(0)));
	}
}

void AChemicalBondGameDirector::SpawnOrUpdateBondVisual(FGuid BondUid)
{
	if (!BondUid.IsValid())
	{
		return;
	}
	if (!BondVisualSystem)
	{
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Connection] Bond visual skipped because BondVisualSystem is null. BondUid=%s"),
			*BondUid.ToString());
		return;
	}

	FChemicalBondRegistryRecord* Record = BondRegistry.Find(BondUid);
	if (!Record)
	{
		return;
	}

	AAtomBase* AtomA = Record->AtomA.Get();
	AAtomBase* AtomB = Record->AtomB.Get();
	if (!AtomA)
	{
		return;
	}
	if (!AtomB)
	{
		return;
	}

	TArray<int32> AtomASlotIndices;
	TArray<int32> AtomBSlotIndices;
	for (const FBondRecord& AtomABondRecord : AtomA->GetBonds())
	{
		if (AtomABondRecord.BondUid == BondUid)
		{
			AtomASlotIndices = AtomABondRecord.MySlotIndices;
			AtomBSlotIndices = AtomABondRecord.PartnerSlotIndices;
			break;
		}
	}

	if (AtomASlotIndices.Num() <= 0 || AtomBSlotIndices.Num() <= 0)
	{
		AtomASlotIndices.Reset();
		AtomBSlotIndices.Reset();
		AtomASlotIndices.Add(Record->AtomASlotIndex);
		AtomBSlotIndices.Add(Record->AtomBSlotIndex);
	}

	FVector BondDirection =
		ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation()) -
		ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation());
	BondDirection = ChemicalBondGameplayPlane::ProjectVector(BondDirection);
	if (BondDirection.IsNearlyZero())
	{
		BondDirection = FVector::ForwardVector;
	}
	BondDirection = BondDirection.GetSafeNormal();

	const FVector BondSideAxis(-BondDirection.Y, BondDirection.X, 0.f);
	AtomASlotIndices.Sort([AtomA, BondSideAxis](const int32 LeftSlotIndex, const int32 RightSlotIndex)
	{
		return FVector::DotProduct(AtomA->GetSlotWorldLocation(LeftSlotIndex), BondSideAxis)
			< FVector::DotProduct(AtomA->GetSlotWorldLocation(RightSlotIndex), BondSideAxis);
	});
	AtomBSlotIndices.Sort([AtomB, BondSideAxis](const int32 LeftSlotIndex, const int32 RightSlotIndex)
	{
		return FVector::DotProduct(AtomB->GetSlotWorldLocation(LeftSlotIndex), BondSideAxis)
			< FVector::DotProduct(AtomB->GetSlotWorldLocation(RightSlotIndex), BondSideAxis);
	});

	const int32 RequiredVisualCount = FMath::Min(AtomASlotIndices.Num(), AtomBSlotIndices.Num());
	FBondVisualComponentList& VisualComponentList = BondVisualComponents.FindOrAdd(BondUid);
	while (VisualComponentList.Components.Num() > RequiredVisualCount)
	{
		TObjectPtr<UNiagaraComponent> ExtraComponent = VisualComponentList.Components.Pop();
		if (ExtraComponent)
		{
			ExtraComponent->Deactivate();
			ExtraComponent->DestroyComponent();
		}
	}
	if (VisualComponentList.Components.Num() < RequiredVisualCount)
	{
		VisualComponentList.Components.SetNum(RequiredVisualCount);
	}

	for (int32 VisualIndex = 0; VisualIndex < RequiredVisualCount; ++VisualIndex)
	{
		const FVector StartLocation = AtomA->GetSlotWorldLocation(AtomASlotIndices[VisualIndex]);
		const FVector EndLocation = AtomB->GetSlotWorldLocation(AtomBSlotIndices[VisualIndex]);

		UNiagaraComponent* BondVisualComponent = nullptr;
		if (VisualComponentList.Components.IsValidIndex(VisualIndex))
		{
			BondVisualComponent = VisualComponentList.Components[VisualIndex].Get();
		}

		if (!BondVisualComponent)
		{
			BondVisualComponent = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
				GetWorld(),
				BondVisualSystem,
				(StartLocation + EndLocation) * 0.5f,
				FRotator::ZeroRotator,
				FVector::OneVector,
				false,
				true,
				ENCPoolMethod::None,
				true);
			if (!BondVisualComponent)
			{
				continue;
			}

			VisualComponentList.Components[VisualIndex] = BondVisualComponent;
		}

		BondVisualComponent->SetWorldLocation((StartLocation + EndLocation) * 0.5f);
		BondVisualComponent->SetVariableVec3(FName(TEXT("start")), StartLocation);
		BondVisualComponent->SetVariableVec3(FName(TEXT("end")), EndLocation);
	}
}

void AChemicalBondGameDirector::UpdateAllBondVisuals()
{
	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		SpawnOrUpdateBondVisual(BondPair.Key);
	}
}

void AChemicalBondGameDirector::DestroyBondVisual(FGuid BondUid)
{
	FBondVisualComponentList ExistingComponents;
	if (!BondVisualComponents.RemoveAndCopyValue(BondUid, ExistingComponents))
	{
		return;
	}

	for (TObjectPtr<UNiagaraComponent>& BondVisualComponent : ExistingComponents.Components)
	{
		if (BondVisualComponent)
		{
			BondVisualComponent->Deactivate();
			BondVisualComponent->DestroyComponent();
		}
	}
}

void AChemicalBondGameDirector::ConfigureDecisionWarningVisualSystem()
{
	if (bDecisionWarningVisualConfigured)
	{
		return;
	}
	if (!DecisionWarningVisualSystem)
	{
		return;
	}

	bDecisionWarningVisualConfigured = true;
	for (FNiagaraEmitterHandle& EmitterHandle : DecisionWarningVisualSystem->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		if (!EmitterData->bLocalSpace)
		{
			EmitterData->bLocalSpace = true;
			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Connection] Decision warning emitter forced to local space. System=%s Emitter=%s"),
				*GetNameSafe(DecisionWarningVisualSystem),
				*EmitterHandle.GetName().ToString());
		}
	}
}

void AChemicalBondGameDirector::LogDecisionWarningVisualParametersOnce()
{
	if (bLoggedDecisionWarningParameters)
	{
		return;
	}
	if (!DecisionWarningVisualSystem)
	{
		return;
	}

	bLoggedDecisionWarningParameters = true;
	TArray<FNiagaraVariable> UserParameters;
	DecisionWarningVisualSystem->GetExposedParameters().GetUserParameters(UserParameters);
	for (const FNiagaraVariable& UserParameter : UserParameters)
	{
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Decision warning Niagara user parameter. Name=%s Type=%s"),
			*UserParameter.GetName().ToString(),
			*UserParameter.GetType().GetNameText().ToString());
	}
}

void AChemicalBondGameDirector::SetDecisionWarningVisualParameters(float WarningLifetime, float WarningRadius)
{
	if (!ActiveDecisionWarningComponent)
	{
		return;
	}

	const float WarningVisualRadius = FMath::Max(WarningRadius * DecisionWarningRadiusParameterScale, 0.f);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("lifetime")), WarningLifetime);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("radiu")), WarningVisualRadius);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("Lifetime")), WarningLifetime);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("Radiu")), WarningVisualRadius);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("User.lifetime")), WarningLifetime);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("User.radiu")), WarningVisualRadius);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("User.Lifetime")), WarningLifetime);
	ActiveDecisionWarningComponent->SetVariableFloat(FName(TEXT("User.Radiu")), WarningVisualRadius);
}

void AChemicalBondGameDirector::SpawnOrUpdateActiveDecisionWarningVisual()
{
	if (!bHasActiveDecisionRequest)
	{
		DestroyActiveDecisionWarningVisual();
		return;
	}

	AAtomBase* ConnectedAtom = ActiveDecisionRequest.ConnectedAtom.Get();
	AAtomBase* FreeAtom = ActiveDecisionRequest.FreeAtom.Get();
	if (!ConnectedAtom)
	{
		DestroyActiveDecisionWarningVisual();
		return;
	}
	if (!FreeAtom)
	{
		DestroyActiveDecisionWarningVisual();
		return;
	}
	if (ConnectedAtom->GetAtomState() != EAtomState::PlayerConnected || FreeAtom->GetAtomState() != EAtomState::PendingDecision)
	{
		DestroyActiveDecisionWarningVisual();
		return;
	}
	if (!DecisionWarningVisualSystem)
	{
		return;
	}

	ConfigureDecisionWarningVisualSystem();
	LogDecisionWarningVisualParametersOnce();

	const FVector WarningLocation = ChemicalBondGameplayPlane::ProjectLocation(ConnectedAtom->GetActorLocation());
	const float WarningLifetime = FMath::Max(ActiveDecisionRequest.RemainingDecisionSeconds, 0.f);
	const float WarningRadius = ConnectedAtom->GetProximityRadius();
	bool bCreatedWarningComponent = false;
	if (!ActiveDecisionWarningComponent)
	{
		ActiveDecisionWarningComponent = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			GetWorld(),
			DecisionWarningVisualSystem,
			WarningLocation,
			FRotator::ZeroRotator,
			FVector::OneVector,
			false,
			false,
			ENCPoolMethod::None,
			true);
		if (!ActiveDecisionWarningComponent)
		{
			return;
		}

		bCreatedWarningComponent = true;
	}

	if (ActiveDecisionWarningComponent->GetAttachParent())
	{
		ActiveDecisionWarningComponent->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	}

	ActiveDecisionWarningComponent->SetWorldLocation(WarningLocation);
	ActiveDecisionWarningComponent->SetWorldRotation(FRotator::ZeroRotator);
	ActiveDecisionWarningComponent->SetWorldScale3D(FVector::OneVector);
	SetDecisionWarningVisualParameters(WarningLifetime, WarningRadius);
	if (bCreatedWarningComponent)
	{
		ActiveDecisionWarningComponent->Activate(true);
	}
	else if (!ActiveDecisionWarningComponent->IsActive())
	{
		ActiveDecisionWarningComponent->Activate(true);
	}

	if (UWorld* World = GetWorld())
	{
		const float CurrentTime = World->GetTimeSeconds();
		if (CurrentTime - LastDecisionWarningLogTime >= 1.f)
		{
			LastDecisionWarningLogTime = CurrentTime;
			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Connection] Decision warning visual update. ConnectedAtom=%s Location=%s Radius=%.2f VisualRadius=%.2f Lifetime=%.2f ComponentLocation=%s"),
				*GetNameSafe(ConnectedAtom),
				*WarningLocation.ToString(),
				WarningRadius,
				FMath::Max(WarningRadius * DecisionWarningRadiusParameterScale, 0.f),
				WarningLifetime,
				*ActiveDecisionWarningComponent->GetComponentLocation().ToString());
		}
	}
}

void AChemicalBondGameDirector::DestroyActiveDecisionWarningVisual()
{
	if (!ActiveDecisionWarningComponent)
	{
		return;
	}

	ActiveDecisionWarningComponent->Deactivate();
	ActiveDecisionWarningComponent->DestroyComponent();
	ActiveDecisionWarningComponent = nullptr;
	LastDecisionWarningLogTime = -1000.f;
}

void AChemicalBondGameDirector::SettleConnectionCandidate(const FAtomConnectionCandidate& Candidate)
{
	AAtomBase* AtomA = Candidate.AtomA.Get();
	AAtomBase* AtomB = Candidate.AtomB.Get();
	FString Reason;
	if (!CanAtomsStartConnection(AtomA, AtomB, Reason))
	{
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Candidate expired as invalid. AtomA=%s AtomB=%s Reason=%s"),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB),
			*Reason);
		return;
	}

	if (!AtomA->IsProximityOverlappingAtom(AtomB) && !AtomB->IsProximityOverlappingAtom(AtomA))
	{
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Candidate expired because atoms are no longer overlapping. AtomA=%s AtomB=%s"),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB));
		return;
	}

	const bool bAtomAFree = AtomA->GetAtomState() == EAtomState::Free;
	const bool bAtomBFree = AtomB->GetAtomState() == EAtomState::Free;
	const bool bAtomAPlayer = AtomA->GetAtomState() == EAtomState::PlayerConnected;
	const bool bAtomBPlayer = AtomB->GetAtomState() == EAtomState::PlayerConnected;

	if (bAtomAFree && bAtomBFree)
	{
		FGuid ExistingBondUid;
		EBondType ExistingBondType = EBondType::Single;
		if (FindExistingBondBetween(AtomA, AtomB, ExistingBondUid, ExistingBondType))
		{
			UE_LOG(LogChemicalBondDirector, Verbose,
				TEXT("[Game:Connection] Free atom candidate ignored because bond already exists. AtomA=%s AtomB=%s BondUid=%s"),
				*GetNameSafe(AtomA),
				*GetNameSafe(AtomB),
				*ExistingBondUid.ToString());
			return;
		}

		int32 AtomASlot = INDEX_NONE;
		int32 AtomBSlot = INDEX_NONE;
		if (!FindClosestFreeSlotPair(AtomA, AtomB, AtomASlot, AtomBSlot))
		{
			ApplyGentleRepulsion(AtomA, AtomB, TEXT("NoFreeSlot"));
			return;
		}

		AlignAtomsForSlotConnection(AtomA, AtomASlot, AtomB, AtomBSlot, TEXT("FreeAutoLink"));
		const FGuid BondUid = LinkAtoms(AtomA, AtomB, EBondType::Single, AtomASlot, AtomBSlot);
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Free atoms auto-linked with single bond. AtomA=%s AtomB=%s BondUid=%s"),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB),
			*BondUid.ToString());
		return;
	}

	if (bAtomAPlayer && bAtomBFree)
	{
		EnqueueDecisionRequest(AtomA, AtomB, Candidate.PairKey);
		return;
	}

	if (bAtomBPlayer && bAtomAFree)
	{
		EnqueueDecisionRequest(AtomB, AtomA, Candidate.PairKey);
		return;
	}

	ApplyGentleRepulsion(AtomA, AtomB, TEXT("StateNotConnectable"));
}

void AChemicalBondGameDirector::EnqueueDecisionRequest(
	AAtomBase* ConnectedAtom,
	AAtomBase* FreeAtom,
	const FAtomInteractionPairKey& PairKey)
{
	if (!ConnectedAtom || !FreeAtom || !PairKey.IsValid() || QueuedDecisionPairKeys.Contains(PairKey))
	{
		return;
	}

	FAtomDecisionRequest Request;
	Request.PairKey = PairKey;
	Request.ConnectedAtom = ConnectedAtom;
	Request.FreeAtom = FreeAtom;
	Request.RemainingDecisionSeconds = DecisionWindowSeconds;
	DecisionQueue.Add(Request);
	QueuedDecisionPairKeys.Add(PairKey);

	FreeAtom->SetAtomState(EAtomState::PendingDecision);
	LockAtom(FreeAtom);

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Connection] Decision enqueued. ConnectedAtom=%s FreeAtom=%s Pair=(%s,%s) QueueLength=%d"),
		*GetNameSafe(ConnectedAtom),
		*GetNameSafe(FreeAtom),
		*PairKey.FirstAtomUid.ToString(),
		*PairKey.SecondAtomUid.ToString(),
		DecisionQueue.Num());

	StartNextDecisionRequest();
}

void AChemicalBondGameDirector::StartNextDecisionRequest()
{
	if (bHasActiveDecisionRequest)
	{
		return;
	}

	while (!DecisionQueue.IsEmpty())
	{
		ActiveDecisionRequest = DecisionQueue[0];
		DecisionQueue.RemoveAt(0);

		AAtomBase* ConnectedAtom = ActiveDecisionRequest.ConnectedAtom.Get();
		AAtomBase* FreeAtom = ActiveDecisionRequest.FreeAtom.Get();
		FString Reason;
		if (!ConnectedAtom)
		{
			Reason = TEXT("Connected atom is null");
		}
		else if (!FreeAtom)
		{
			Reason = TEXT("Free atom is null");
		}
		else if (ConnectedAtom->GetAtomState() != EAtomState::PlayerConnected)
		{
			Reason = TEXT("Connected atom is not PlayerConnected");
		}
		else if (FreeAtom->GetAtomState() != EAtomState::PendingDecision)
		{
			Reason = TEXT("Free atom is not PendingDecision");
		}
		else if (ConnectedAtom->IsInteractionCoolingDown() || FreeAtom->IsInteractionCoolingDown())
		{
			Reason = TEXT("Interaction cooldown");
		}
		else if (ConnectedAtom->GetAvailableSlotCount() <= 0 || FreeAtom->GetAvailableSlotCount() <= 0)
		{
			Reason = TEXT("No free slot");
		}

		if (!Reason.IsEmpty())
		{
			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Connection] Decision request discarded before activation. ConnectedAtom=%s FreeAtom=%s Reason=%s"),
				*GetNameSafe(ConnectedAtom),
				*GetNameSafe(FreeAtom),
				*Reason);
			if (FreeAtom && FreeAtom->GetAtomState() == EAtomState::PendingDecision)
			{
				FreeAtom->SetAtomState(EAtomState::Free);
			}
			UnlockAtom(FreeAtom);
			ReleaseDecisionPair(ActiveDecisionRequest.PairKey);
			continue;
		}

		LockAtom(ConnectedAtom);
		int32 ConnectedSlot = INDEX_NONE;
		int32 FreeSlot = INDEX_NONE;
		if (FindClosestFreeSlotPair(ConnectedAtom, FreeAtom, ConnectedSlot, FreeSlot))
		{
			AlignAtomsForSlotConnection(ConnectedAtom, ConnectedSlot, FreeAtom, FreeSlot, TEXT("DecisionActivated"));
		}
		bHasActiveDecisionRequest = true;
		SpawnOrUpdateActiveDecisionWarningVisual();
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Decision activated. ConnectedAtom=%s FreeAtom=%s Pair=(%s,%s) Window=%.2f"),
			*GetNameSafe(ConnectedAtom),
			*GetNameSafe(FreeAtom),
			*ActiveDecisionRequest.PairKey.FirstAtomUid.ToString(),
			*ActiveDecisionRequest.PairKey.SecondAtomUid.ToString(),
			ActiveDecisionRequest.RemainingDecisionSeconds);
		return;
	}
}

void AChemicalBondGameDirector::FinishActiveDecision(bool bRejected)
{
	if (!bHasActiveDecisionRequest)
	{
		return;
	}

	AAtomBase* ConnectedAtom = ActiveDecisionRequest.ConnectedAtom.Get();
	AAtomBase* FreeAtom = ActiveDecisionRequest.FreeAtom.Get();
	const bool bHasFormedBond = ActiveDecisionRequest.bHasFormedBond;
	DestroyActiveDecisionWarningVisual();

	if (FreeAtom)
	{
		if (bHasFormedBond)
		{
			SetAtomGroupState(FreeAtom, EAtomState::PlayerConnected);
		}
		else
		{
			FreeAtom->SetAtomState(EAtomState::Free);
			ApplyGentleRepulsion(ConnectedAtom, FreeAtom, bRejected ? TEXT("DecisionRejected") : TEXT("DecisionNoBond"));
		}
	}

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Connection] Decision finished. ConnectedAtom=%s FreeAtom=%s HasBond=%d Rejected=%d RemainingQueue=%d"),
		*GetNameSafe(ConnectedAtom),
		*GetNameSafe(FreeAtom),
		bHasFormedBond ? 1 : 0,
		bRejected ? 1 : 0,
		DecisionQueue.Num());

	UnlockAtom(ConnectedAtom);
	UnlockAtom(FreeAtom);
	ReleaseDecisionPair(ActiveDecisionRequest.PairKey);
	bHasActiveDecisionRequest = false;
	ActiveDecisionRequest = FAtomDecisionRequest();
	StartNextDecisionRequest();
}

bool AChemicalBondGameDirector::TryAdvanceActiveDecisionBond()
{
	AAtomBase* ConnectedAtom = ActiveDecisionRequest.ConnectedAtom.Get();
	AAtomBase* FreeAtom = ActiveDecisionRequest.FreeAtom.Get();
	if (!ConnectedAtom || !FreeAtom)
	{
		FinishActiveDecision(false);
		return false;
	}

	FGuid BondUid = ActiveDecisionRequest.BondUid;
	EBondType CurrentBondType = EBondType::Single;
	if (!BondUid.IsValid())
	{
		FindExistingBondBetween(ConnectedAtom, FreeAtom, BondUid, CurrentBondType);
	}
	else if (const FChemicalBondRegistryRecord* ExistingRecord = BondRegistry.Find(BondUid))
	{
		CurrentBondType = ExistingRecord->BondType;
	}

	if (!BondUid.IsValid())
	{
		int32 ConnectedSlot = INDEX_NONE;
		int32 FreeSlot = INDEX_NONE;
		if (!FindClosestFreeSlotPair(ConnectedAtom, FreeAtom, ConnectedSlot, FreeSlot))
		{
			FinishActiveDecision(false);
			return false;
		}

		AlignAtomsForSlotConnection(ConnectedAtom, ConnectedSlot, FreeAtom, FreeSlot, TEXT("DecisionSingleBond"));
		ActiveDecisionRequest.BondUid = LinkAtoms(
			ConnectedAtom,
			FreeAtom,
			EBondType::Single,
			ConnectedSlot,
			FreeSlot);
		ActiveDecisionRequest.bHasFormedBond = ActiveDecisionRequest.BondUid.IsValid();
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Decision formed single bond. ConnectedAtom=%s FreeAtom=%s BondUid=%s"),
			*GetNameSafe(ConnectedAtom),
			*GetNameSafe(FreeAtom),
			*ActiveDecisionRequest.BondUid.ToString());
	}
	else if (CurrentBondType == EBondType::Single || CurrentBondType == EBondType::Double)
	{
		int32 ConnectedSlot = INDEX_NONE;
		int32 FreeSlot = INDEX_NONE;
		if (!FindClosestFreeSlotPair(ConnectedAtom, FreeAtom, ConnectedSlot, FreeSlot))
		{
			FinishActiveDecision(false);
			return false;
		}

		const EBondType NewBondType = CurrentBondType == EBondType::Single ? EBondType::Double : EBondType::Triple;
		AlignAtomsForSlotConnection(ConnectedAtom, ConnectedSlot, FreeAtom, FreeSlot, TEXT("DecisionBondUpgrade"));
		if (!ChangeBondType(BondUid, NewBondType, ConnectedSlot, FreeSlot))
		{
			UE_LOG(LogChemicalBondDirector, Warning,
				TEXT("[Game:Connection] Failed to change bond type. BondUid=%s NewType=%d"),
				*BondUid.ToString(),
				static_cast<int32>(NewBondType));
			return false;
		}

		ActiveDecisionRequest.BondUid = BondUid;
		ActiveDecisionRequest.bHasFormedBond = true;
		CurrentBondType = NewBondType;
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Connection] Decision changed bond type. BondUid=%s NewType=%d"),
			*BondUid.ToString(),
			static_cast<int32>(NewBondType));
	}

	bool bFoundRecord = false;
	const FChemicalBondRegistryRecord Record = GetBondRecord(ActiveDecisionRequest.BondUid, bFoundRecord);
	if (bFoundRecord && (Record.BondType == EBondType::Triple
		|| ConnectedAtom->GetAvailableSlotCount() <= 0
		|| FreeAtom->GetAvailableSlotCount() <= 0))
	{
		FinishActiveDecision(false);
	}

	return true;
}

bool AChemicalBondGameDirector::ChangeBondType(
	FGuid BondUid,
	EBondType NewBondType,
	int32 AtomAAdditionalSlot,
	int32 AtomBAdditionalSlot)
{
	FChemicalBondRegistryRecord* Record = BondRegistry.Find(BondUid);
	if (!Record)
	{
		return false;
	}

	AAtomBase* AtomA = Record->AtomA.Get();
	AAtomBase* AtomB = Record->AtomB.Get();
	if (!AtomA || !AtomB)
	{
		return false;
	}

	const EBondType OldBondType = Record->BondType;
	if (!AtomA->AddBondSlotByUid(BondUid, AtomAAdditionalSlot, AtomBAdditionalSlot))
	{
		return false;
	}

	if (!AtomB->AddBondSlotByUid(BondUid, AtomBAdditionalSlot, AtomAAdditionalSlot))
	{
		AtomA->RemoveBondSlotByUid(BondUid, AtomAAdditionalSlot, AtomBAdditionalSlot);
		UE_LOG(LogChemicalBondDirector, Warning,
			TEXT("[Game:Connection] Rollback bond slot change because AtomB failed to add slot. BondUid=%s AtomA=%s AtomB=%s"),
			*BondUid.ToString(),
			*GetNameSafe(AtomA),
			*GetNameSafe(AtomB));
		return false;
	}

	AtomA->SetBondTypeByUid(BondUid, NewBondType);
	AtomB->SetBondTypeByUid(BondUid, NewBondType);
	RemoveBondUidFromTypeList(BondUid, OldBondType);
	AddBondUidToTypeList(BondUid, NewBondType);
	Record->BondType = NewBondType;

	RefreshAtomBondLayouts(AtomA, AtomB);
	SpawnOrUpdateBondVisual(BondUid);
	AssertBondRegistryConsistency();
	return true;
}

bool AChemicalBondGameDirector::FindExistingBondBetween(
	AAtomBase* AtomA,
	AAtomBase* AtomB,
	FGuid& OutBondUid,
	EBondType& OutBondType) const
{
	OutBondUid.Invalidate();
	OutBondType = EBondType::Single;
	if (!AtomA || !AtomB)
	{
		return false;
	}

	const FGuid AtomAUid = AtomA->GetAtomUid();
	const FGuid AtomBUid = AtomB->GetAtomUid();
	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FChemicalBondRegistryRecord& Record = BondPair.Value;
		const bool bSameOrder = Record.AtomAUid == AtomAUid && Record.AtomBUid == AtomBUid;
		const bool bReverseOrder = Record.AtomAUid == AtomBUid && Record.AtomBUid == AtomAUid;
		if (bSameOrder || bReverseOrder)
		{
			OutBondUid = BondPair.Key;
			OutBondType = Record.BondType;
			return true;
		}
	}

	return false;
}

void AChemicalBondGameDirector::SetAtomGroupState(AAtomBase* RootAtom, EAtomState NewState)
{
	if (!RootAtom)
	{
		return;
	}

	TArray<AAtomBase*> PendingAtoms;
	TSet<FGuid> VisitedAtomUids;
	PendingAtoms.Add(RootAtom);

	while (!PendingAtoms.IsEmpty())
	{
		AAtomBase* Atom = PendingAtoms.Pop();
		if (!Atom)
		{
			continue;
		}

		const FGuid AtomUid = Atom->GetAtomUid();
		if (!AtomUid.IsValid() || VisitedAtomUids.Contains(AtomUid))
		{
			continue;
		}

		VisitedAtomUids.Add(AtomUid);
		Atom->SetAtomState(NewState);

		for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
		{
			const FChemicalBondRegistryRecord& Record = BondPair.Value;
			if (Record.AtomAUid == AtomUid)
			{
				PendingAtoms.Add(Record.AtomB.Get());
			}
			else if (Record.AtomBUid == AtomUid)
			{
				PendingAtoms.Add(Record.AtomA.Get());
			}
		}
	}
}

void AChemicalBondGameDirector::ApplyFixedConnectionConstraint(AAtomBase* AtomA, AAtomBase* AtomB, const TCHAR* Reason)
{
	if (!AtomA)
	{
		return;
	}

	if (!AtomB)
	{
		return;
	}

	if (AtomA == AtomB)
	{
		return;
	}

	AtomA->ConstrainToGameplayPlane();
	AtomB->ConstrainToGameplayPlane();

	FGuid ExistingBondUid;
	EBondType ExistingBondType = EBondType::Single;
	if (FindExistingBondBetween(AtomA, AtomB, ExistingBondUid, ExistingBondType))
	{
		bool bFoundRecord = false;
		const FChemicalBondRegistryRecord Record = GetBondRecord(ExistingBondUid, bFoundRecord);
		if (bFoundRecord)
		{
			AlignAtomsForSlotConnection(
				Record.AtomA.Get(),
				Record.AtomASlotIndex,
				Record.AtomB.Get(),
				Record.AtomBSlotIndex,
				Reason);
			SpawnOrUpdateBondVisual(ExistingBondUid);
			return;
		}
	}

	const FVector AtomALocation = ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation());
	const FVector AtomBLocation = ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation());
	FVector PlanarDelta = AtomBLocation - AtomALocation;
	PlanarDelta = ChemicalBondGameplayPlane::ProjectVector(PlanarDelta);
	if (PlanarDelta.IsNearlyZero())
	{
		PlanarDelta = FVector::ForwardVector;
	}

	const float CurrentDistance = PlanarDelta.Size();
	if (FMath::Abs(CurrentDistance - TemporaryConnectedAtomDistance) <= ConnectionConstraintTolerance)
	{
		return;
	}

	const FVector Direction = PlanarDelta.GetSafeNormal();
	AAtomBase* AnchorAtom = ChooseConnectionAnchor(AtomA, AtomB);

	if (AnchorAtom == AtomA)
	{
		FVector NewAtomBLocation = AtomALocation + Direction * TemporaryConnectedAtomDistance;
		NewAtomBLocation = ChemicalBondGameplayPlane::ProjectLocation(NewAtomBLocation);
		AtomB->SetActorLocation(NewAtomBLocation, false);
		StopConstrainedAtomMotion(AtomB);
	}
	else if (AnchorAtom == AtomB)
	{
		FVector NewAtomALocation = AtomBLocation - Direction * TemporaryConnectedAtomDistance;
		NewAtomALocation = ChemicalBondGameplayPlane::ProjectLocation(NewAtomALocation);
		AtomA->SetActorLocation(NewAtomALocation, false);
		StopConstrainedAtomMotion(AtomA);
	}
	else
	{
		const FVector Midpoint = (AtomALocation + AtomBLocation) * 0.5f;
		FVector NewAtomALocation = Midpoint - Direction * TemporaryConnectedAtomDistance * 0.5f;
		FVector NewAtomBLocation = Midpoint + Direction * TemporaryConnectedAtomDistance * 0.5f;
		NewAtomALocation = ChemicalBondGameplayPlane::ProjectLocation(NewAtomALocation);
		NewAtomBLocation = ChemicalBondGameplayPlane::ProjectLocation(NewAtomBLocation);
		AtomA->SetActorLocation(NewAtomALocation, false);
		AtomB->SetActorLocation(NewAtomBLocation, false);
		StopConstrainedAtomMotion(AtomA);
		StopConstrainedAtomMotion(AtomB);
	}

	UE_LOG(LogChemicalBondDirector, VeryVerbose,
		TEXT("[Game:Connection] Fixed connection constraint applied. AtomA=%s AtomB=%s Reason=%s Distance=%.2f Target=%.2f"),
		*GetNameSafe(AtomA),
		*GetNameSafe(AtomB),
		Reason ? Reason : TEXT("Unknown"),
		CurrentDistance,
		TemporaryConnectedAtomDistance);
}

AAtomBase* AChemicalBondGameDirector::ChooseConnectionAnchor(AAtomBase* AtomA, AAtomBase* AtomB) const
{
	if (!AtomA || !AtomB)
	{
		return nullptr;
	}

	if (AtomA->IsPlayerControlled())
	{
		return AtomA;
	}

	if (AtomB->IsPlayerControlled())
	{
		return AtomB;
	}

	if (AtomA->GetAtomState() == EAtomState::PlayerConnected && AtomB->GetAtomState() != EAtomState::PlayerConnected)
	{
		return AtomA;
	}

	if (AtomB->GetAtomState() == EAtomState::PlayerConnected && AtomA->GetAtomState() != EAtomState::PlayerConnected)
	{
		return AtomB;
	}

	return nullptr;
}

void AChemicalBondGameDirector::StopConstrainedAtomMotion(AAtomBase* Atom) const
{
	if (!Atom || Atom->IsPlayerControlled())
	{
		return;
	}

	if (UFluidMotionComponent* FluidMotionComponent = Atom->GetFluidMotionComponent())
	{
		FluidMotionComponent->StopFluidMotion();
	}
}

void AChemicalBondGameDirector::ApplyGentleRepulsion(AAtomBase* AtomA, AAtomBase* AtomB, const TCHAR* Reason)
{
	if (!AtomA || !AtomB)
	{
		return;
	}

	AtomA->ConstrainToGameplayPlane();
	AtomB->ConstrainToGameplayPlane();

	FVector Direction =
		ChemicalBondGameplayPlane::ProjectLocation(AtomB->GetActorLocation()) -
		ChemicalBondGameplayPlane::ProjectLocation(AtomA->GetActorLocation());
	Direction = ChemicalBondGameplayPlane::ProjectVector(Direction);
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}
	Direction = Direction.GetSafeNormal();

	const float AverageMass = (FMath::Max(AtomA->GetMass(), 1.f) + FMath::Max(AtomB->GetMass(), 1.f)) * 0.5f;
	const float ImpulseStrength = GentleRepulsionImpulse + AverageMass * GentleRepulsionMassScale;
	if (UFluidMotionComponent* AtomAFluidMotion = AtomA->GetFluidMotionComponent())
	{
		AtomAFluidMotion->AddLinearImpulse(-Direction * ImpulseStrength);
	}
	if (UFluidMotionComponent* AtomBFluidMotion = AtomB->GetFluidMotionComponent())
	{
		AtomBFluidMotion->AddLinearImpulse(Direction * ImpulseStrength);
	}

	AtomA->BeginInteractionCooldown(InteractionCooldownSeconds);
	AtomB->BeginInteractionCooldown(InteractionCooldownSeconds);

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Connection] Gentle repulsion applied. AtomA=%s AtomB=%s Reason=%s Impulse=%.2f Cooldown=%.2f"),
		*GetNameSafe(AtomA),
		*GetNameSafe(AtomB),
		Reason ? Reason : TEXT("Unknown"),
		ImpulseStrength,
		InteractionCooldownSeconds);
}

bool AChemicalBondGameDirector::CanAtomsStartConnection(AAtomBase* AtomA, AAtomBase* AtomB, FString& OutReason) const
{
	if (!AtomA)
	{
		OutReason = TEXT("AtomA is null");
		return false;
	}
	if (!AtomB)
	{
		OutReason = TEXT("AtomB is null");
		return false;
	}
	if (AtomA == AtomB)
	{
		OutReason = TEXT("Same atom");
		return false;
	}
	if (!AtomA->GetAtomUid().IsValid() || !AtomB->GetAtomUid().IsValid())
	{
		OutReason = TEXT("Atom UID is invalid");
		return false;
	}
	if (AtomA->IsInteractionCoolingDown() || AtomB->IsInteractionCoolingDown())
	{
		OutReason = TEXT("Interaction cooldown");
		return false;
	}
	if (IsAtomLocked(AtomA) || IsAtomLocked(AtomB))
	{
		OutReason = TEXT("Atom locked");
		return false;
	}
	if (AtomA->GetAvailableSlotCount() <= 0 || AtomB->GetAvailableSlotCount() <= 0)
	{
		OutReason = TEXT("No free slot");
		return false;
	}
	if (AtomA->GetAtomState() == EAtomState::PendingDecision || AtomB->GetAtomState() == EAtomState::PendingDecision)
	{
		OutReason = TEXT("Pending decision");
		return false;
	}
	if (AtomA->GetAtomState() == EAtomState::PlayerConnected && AtomB->GetAtomState() == EAtomState::PlayerConnected)
	{
		OutReason = TEXT("Both atoms are already in player group");
		return false;
	}

	OutReason.Reset();
	return true;
}

bool AChemicalBondGameDirector::IsAtomLocked(AAtomBase* Atom) const
{
	return Atom && LockedAtomUids.Contains(Atom->GetAtomUid());
}

void AChemicalBondGameDirector::LockAtom(AAtomBase* Atom)
{
	if (Atom && Atom->GetAtomUid().IsValid())
	{
		LockedAtomUids.Add(Atom->GetAtomUid());
	}
}

void AChemicalBondGameDirector::UnlockAtom(AAtomBase* Atom)
{
	if (Atom)
	{
		LockedAtomUids.Remove(Atom->GetAtomUid());
	}
}

void AChemicalBondGameDirector::ReleaseDecisionPair(const FAtomInteractionPairKey& PairKey)
{
	QueuedDecisionPairKeys.Remove(PairKey);
	ConnectionCandidates.Remove(PairKey);
}

void AChemicalBondGameDirector::InitializeRefreshRuntime()
{
	CurrentMainGuideRegion = FMath::RandRange(0, 7);
	RefreshTimeRemaining = FMath::FRandRange(
		FMath::Min(RefreshIntervalMin, RefreshIntervalMax),
		FMath::Max(RefreshIntervalMin, RefreshIntervalMax));
	GuideTimeRemaining = FMath::FRandRange(
		FMath::Min(GuideIntervalMin, GuideIntervalMax),
		FMath::Max(GuideIntervalMin, GuideIntervalMax));

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Refresh] Runtime initialized. MainGuide=%d RefreshIn=%.2f GuideIn=%.2f"),
		CurrentMainGuideRegion,
		RefreshTimeRemaining,
		GuideTimeRemaining);
}

void AChemicalBondGameDirector::ProcessSceneRefresh(float DeltaSeconds)
{
	if (DeltaSeconds <= 0.f)
	{
		return;
	}

	ProcessLifeSpanRecycling();
	ProcessRefreshGuide(DeltaSeconds);

	RefreshTimeRemaining -= DeltaSeconds;
	if (RefreshTimeRemaining > 0.f)
	{
		return;
	}

	ExecuteGlobalRefresh();
	RefreshTimeRemaining = FMath::FRandRange(
		FMath::Min(RefreshIntervalMin, RefreshIntervalMax),
		FMath::Max(RefreshIntervalMin, RefreshIntervalMax));
}

void AChemicalBondGameDirector::ProcessRefreshGuide(float DeltaSeconds)
{
	if (CurrentMainGuideRegion == INDEX_NONE)
	{
		CurrentMainGuideRegion = FMath::RandRange(0, 7);
	}

	GuideTimeRemaining -= DeltaSeconds;
	if (GuideTimeRemaining > 0.f)
	{
		return;
	}

	const int32 PreviousRegion = GetPreviousGuideRegion(CurrentMainGuideRegion);
	const int32 NextRegion = GetNextGuideRegion(CurrentMainGuideRegion);
	const int32 PreviousOpposite = GetOppositeGuideRegion(PreviousRegion);
	const int32 NextOpposite = GetOppositeGuideRegion(NextRegion);
	const int32 PreviousOppositeCount = CountFreeAtomsInRegion(PreviousOpposite);
	const int32 NextOppositeCount = CountFreeAtomsInRegion(NextOpposite);

	if (PreviousOppositeCount != NextOppositeCount)
	{
		CurrentMainGuideRegion =
			PreviousOppositeCount > NextOppositeCount
				? GetOppositeGuideRegion(PreviousOpposite)
				: GetOppositeGuideRegion(NextOpposite);
	}

	GuideTimeRemaining = FMath::FRandRange(
		FMath::Min(GuideIntervalMin, GuideIntervalMax),
		FMath::Max(GuideIntervalMin, GuideIntervalMax));

	UE_LOG(LogChemicalBondDirector, Log,
		TEXT("[Game:Refresh] Guide updated. MainGuide=%d Counts=(%d:%d,%d:%d) NextGuideIn=%.2f"),
		CurrentMainGuideRegion,
		PreviousOpposite,
		PreviousOppositeCount,
		NextOpposite,
		NextOppositeCount,
		GuideTimeRemaining);
}

void AChemicalBondGameDirector::ProcessLifeSpanRecycling()
{
	PruneStaleAtomRegistryEntries();

	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return;
	}

	TArray<TPair<FGuid, AAtomBase*>> AtomsToRecycle;
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom || Atom->IsPlayerControlled() || Atom->GetAtomState() != EAtomState::Free)
		{
			continue;
		}

		const FVector2D LocalLocation = Frame.ToLocal2D(Atom->GetActorLocation());
		const bool bInsideLifeSpan =
			FMath::Abs(LocalLocation.X) <= Frame.LifeSpanHalfExtent.X
			&& FMath::Abs(LocalLocation.Y) <= Frame.LifeSpanHalfExtent.Y;
		if (!bInsideLifeSpan)
		{
			AtomsToRecycle.Add(TPair<FGuid, AAtomBase*>(AtomPair.Key, Atom));
		}
	}

	for (const TPair<FGuid, AAtomBase*>& RecyclePair : AtomsToRecycle)
	{
		TerminateAtomByUid(RecyclePair.Key);
		if (RecyclePair.Value)
		{
			RecyclePair.Value->Destroy();
		}
	}
}

void AChemicalBondGameDirector::ExecuteGlobalRefresh()
{
	if (!RefreshAtomClass)
	{
		UE_LOG(LogChemicalBondDirector, Warning, TEXT("[Game:Refresh] Refresh skipped because RefreshAtomClass is null."));
		return;
	}

	int32 FreeAtomCount = CountFreeAtomsInLogicRange();
	if (FreeAtomCount >= TargetFreeAtomCount)
	{
		return;
	}

	for (int32 RegionIndex = 0; RegionIndex < 8; ++RegionIndex)
	{
		const int32 SpawnCount = FMath::FloorToInt(RefreshBaseCount * GetRefreshCoefficientForRegion(RegionIndex));
		for (int32 SpawnIndex = 0; SpawnIndex < SpawnCount && FreeAtomCount < TargetFreeAtomCount; ++SpawnIndex)
		{
			if (TrySpawnRefreshAtomInRegion(RegionIndex))
			{
				++FreeAtomCount;
			}
		}
	}
}

bool AChemicalBondGameDirector::TrySpawnRefreshAtomInRegion(int32 RegionIndex)
{
	UWorld* World = GetWorld();
	if (!World || !RefreshAtomClass)
	{
		return false;
	}

	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return false;
	}

	FVector2D RegionMin;
	FVector2D RegionMax;
	if (!GetRefreshRegionBounds(RegionIndex, RegionMin, RegionMax))
	{
		return false;
	}

	const EAtomElementType ElementType = PickRefreshElementType();
	const float SpawnProximityRadius = GetRefreshElementProximityRadius(ElementType);
	for (int32 AttemptIndex = 0; AttemptIndex < SpawnPlacementAttempts; ++AttemptIndex)
	{
		const FVector2D LocalSpawnLocation(
			FMath::FRandRange(RegionMin.X, RegionMax.X),
			FMath::FRandRange(RegionMin.Y, RegionMax.Y));
		const FVector SpawnLocation = Frame.ToWorld(LocalSpawnLocation);
		if (!IsSpawnLocationLegal(SpawnLocation, SpawnProximityRadius))
		{
			continue;
		}

		const FRotator SpawnRotation(0.f, FMath::FRandRange(0.f, 360.f), 0.f);
		const FTransform SpawnTransform(SpawnRotation, SpawnLocation);
		AAtomBase* SpawnedAtom = World->SpawnActorDeferred<AAtomBase>(
			RefreshAtomClass,
			SpawnTransform,
			this,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!SpawnedAtom)
		{
			return false;
		}

		SpawnedAtom->ConfigureElementType(ElementType);
		SpawnedAtom->FinishSpawning(SpawnTransform);
		SpawnAtom(SpawnedAtom);
		ApplySpawnInitialImpulse(SpawnedAtom, Frame.Center);
		return true;
	}

	return false;
}

EAtomElementType AChemicalBondGameDirector::PickRefreshElementType() const
{
	static constexpr EAtomElementType RefreshElements[] =
	{
		EAtomElementType::H,
		EAtomElementType::H_Normal,
		EAtomElementType::C_Normal,
		EAtomElementType::C_Ring,
		EAtomElementType::O_Normal,
		EAtomElementType::O_Ring,
		EAtomElementType::N_Normal,
		EAtomElementType::N_Ring,
		EAtomElementType::P_Normal,
		EAtomElementType::P_Ring
	};

	return RefreshElements[FMath::RandHelper(UE_ARRAY_COUNT(RefreshElements))];
}

void AChemicalBondGameDirector::ApplySpawnInitialImpulse(AAtomBase* SpawnedAtom, const FVector& PlayerLocation)
{
	if (!SpawnedAtom)
	{
		return;
	}

	UFluidMotionComponent* FluidMotionComponent = SpawnedAtom->GetFluidMotionComponent();
	if (!FluidMotionComponent)
	{
		return;
	}

	const bool bUseDirectedImpulse = FMath::FRand() < FMath::Clamp(DirectedSpawnRate, 0.f, 1.f);
	FVector Direction = FVector::ZeroVector;
	float MinImpulse = 0.f;
	float MaxImpulse = 0.f;
	if (bUseDirectedImpulse)
	{
		Direction = ChemicalBondGameplayPlane::ProjectVector(PlayerLocation - SpawnedAtom->GetActorLocation()).GetSafeNormal();
		MinImpulse = DirectedSpawnImpulseMin;
		MaxImpulse = DirectedSpawnImpulseMax;
	}
	else
	{
		Direction = MakePlanarDirectionFromYaw(FMath::FRandRange(0.f, 360.f));
		MinImpulse = RandomSpawnImpulseMin;
		MaxImpulse = RandomSpawnImpulseMax;
	}

	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}

	const float ImpulseMagnitude = FMath::FRandRange(FMath::Min(MinImpulse, MaxImpulse), FMath::Max(MinImpulse, MaxImpulse));
	FluidMotionComponent->AddLinearImpulse(Direction * ImpulseMagnitude);
}

int32 AChemicalBondGameDirector::CountFreeAtomsInLogicRange() const
{
	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return 0;
	}

	int32 Count = 0;
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		const AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom || Atom->IsPlayerControlled() || Atom->GetAtomState() != EAtomState::Free)
		{
			continue;
		}

		const FVector2D LocalLocation = Frame.ToLocal2D(Atom->GetActorLocation());
		if (FMath::Abs(LocalLocation.X) <= Frame.LogicHalfExtent.X
			&& FMath::Abs(LocalLocation.Y) <= Frame.LogicHalfExtent.Y)
		{
			++Count;
		}
	}

	return Count;
}

int32 AChemicalBondGameDirector::CountFreeAtomsInRegion(int32 RegionIndex) const
{
	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return 0;
	}

	FVector2D RegionMin;
	FVector2D RegionMax;
	if (!GetRefreshRegionBounds(RegionIndex, RegionMin, RegionMax))
	{
		return 0;
	}

	int32 Count = 0;
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		const AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom || Atom->IsPlayerControlled() || Atom->GetAtomState() != EAtomState::Free)
		{
			continue;
		}

		const FVector2D LocalLocation = Frame.ToLocal2D(Atom->GetActorLocation());
		if (LocalLocation.X >= RegionMin.X && LocalLocation.X <= RegionMax.X
			&& LocalLocation.Y >= RegionMin.Y && LocalLocation.Y <= RegionMax.Y)
		{
			++Count;
		}
	}

	return Count;
}

float AChemicalBondGameDirector::GetRefreshCoefficientForRegion(int32 RegionIndex) const
{
	if (RegionIndex == CurrentMainGuideRegion)
	{
		return MainGuideRefreshCoefficient;
	}

	if (RegionIndex == GetPreviousGuideRegion(CurrentMainGuideRegion)
		|| RegionIndex == GetNextGuideRegion(CurrentMainGuideRegion))
	{
		return SubGuideRefreshCoefficient;
	}

	if (RegionIndex == GetPreviousGuideRegion(GetPreviousGuideRegion(CurrentMainGuideRegion))
		|| RegionIndex == GetNextGuideRegion(GetNextGuideRegion(CurrentMainGuideRegion)))
	{
		return WeakGuideRefreshCoefficient;
	}

	return NoneGuideRefreshCoefficient;
}

int32 AChemicalBondGameDirector::GetPreviousGuideRegion(int32 RegionIndex) const
{
	return (FMath::Clamp(RegionIndex, 0, 7) + 7) % 8;
}

int32 AChemicalBondGameDirector::GetNextGuideRegion(int32 RegionIndex) const
{
	return (FMath::Clamp(RegionIndex, 0, 7) + 1) % 8;
}

int32 AChemicalBondGameDirector::GetOppositeGuideRegion(int32 RegionIndex) const
{
	return 7 - FMath::Clamp(RegionIndex, 0, 7);
}

bool AChemicalBondGameDirector::GetRefreshRegionBounds(int32 RegionIndex, FVector2D& OutMin, FVector2D& OutMax) const
{
	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame) || RegionIndex < 0 || RegionIndex > 7)
	{
		return false;
	}

	static constexpr int32 RegionRows[] = { 0, 0, 0, 1, 1, 2, 2, 2 };
	static constexpr int32 RegionColumns[] = { 0, 1, 2, 0, 2, 0, 1, 2 };
	const int32 Row = RegionRows[RegionIndex];
	const int32 Column = RegionColumns[RegionIndex];

	const float X0 = -Frame.LifeSpanHalfExtent.X;
	const float X1 = -Frame.LifeSpanHalfExtent.X / 3.f;
	const float X2 = Frame.LifeSpanHalfExtent.X / 3.f;
	const float X3 = Frame.LifeSpanHalfExtent.X;
	const float Y0 = Frame.LifeSpanHalfExtent.Y;
	const float Y1 = Frame.LifeSpanHalfExtent.Y / 3.f;
	const float Y2 = -Frame.LifeSpanHalfExtent.Y / 3.f;
	const float Y3 = -Frame.LifeSpanHalfExtent.Y;

	const float MinXByColumn[] = { X0, X1, X2 };
	const float MaxXByColumn[] = { X1, X2, X3 };
	const float MinYByRow[] = { Y1, Y2, Y3 };
	const float MaxYByRow[] = { Y0, Y1, Y2 };

	OutMin = FVector2D(MinXByColumn[Column], MinYByRow[Row]);
	OutMax = FVector2D(MaxXByColumn[Column], MaxYByRow[Row]);
	return true;
}

bool AChemicalBondGameDirector::IsWorldLocationInsideRefreshHalfExtent(
	const FVector& WorldLocation,
	const FVector2D& HalfExtent) const
{
	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return false;
	}

	const FVector2D LocalLocation = Frame.ToLocal2D(WorldLocation);
	return FMath::Abs(LocalLocation.X) <= HalfExtent.X && FMath::Abs(LocalLocation.Y) <= HalfExtent.Y;
}

bool AChemicalBondGameDirector::IsSpawnLocationLegal(const FVector& SpawnLocation, float SpawnProximityRadius) const
{
	FRefreshRangeFrame Frame;
	if (!TryBuildRefreshRangeFrame(this, Frame))
	{
		return false;
	}

	const FVector2D LocalLocation = Frame.ToLocal2D(SpawnLocation);
	if (FMath::Abs(LocalLocation.X) <= Frame.ViewPortHalfExtent.X
		&& FMath::Abs(LocalLocation.Y) <= Frame.ViewPortHalfExtent.Y)
	{
		return false;
	}

	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		const AAtomBase* Atom = AtomPair.Value.Get();
		if (!Atom || Atom->IsPlayerControlled() || Atom->GetAtomState() != EAtomState::Free)
		{
			continue;
		}

		const float CombinedRadius = SpawnProximityRadius + Atom->GetProximityRadius();
		if (DistanceSquaredPointToSegment2D(Atom->GetActorLocation(), SpawnLocation, Frame.Center)
			<= FMath::Square(CombinedRadius))
		{
			return false;
		}
	}

	return true;
}

FGuid AChemicalBondGameDirector::GenerateUniqueAtomUid() const
{
	FGuid NewUid;
	do
	{
		NewUid = FGuid::NewGuid();
	}
	while (!NewUid.IsValid() || AtomRegistry.Contains(NewUid));

	return NewUid;
}

FGuid AChemicalBondGameDirector::GenerateUniqueBondUid() const
{
	FGuid NewUid;
	do
	{
		NewUid = FGuid::NewGuid();
	}
	while (!NewUid.IsValid() || BondRegistry.Contains(NewUid));

	return NewUid;
}

void AChemicalBondGameDirector::RegisterExistingAtomsInWorld()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AAtomBase> AtomIt(World); AtomIt; ++AtomIt)
	{
		SpawnAtom(*AtomIt);
	}
}

void AChemicalBondGameDirector::AddBondUidToTypeList(FGuid BondUid, EBondType BondType)
{
	switch (BondType)
	{
	case EBondType::Single:
		SingleBondUids.AddUnique(BondUid);
		break;
	case EBondType::Double:
		DoubleBondUids.AddUnique(BondUid);
		break;
	case EBondType::Triple:
		TripleBondUids.AddUnique(BondUid);
		break;
	default:
		break;
	}
}

void AChemicalBondGameDirector::RemoveBondUidFromTypeList(FGuid BondUid, EBondType BondType)
{
	switch (BondType)
	{
	case EBondType::Single:
		SingleBondUids.Remove(BondUid);
		break;
	case EBondType::Double:
		DoubleBondUids.Remove(BondUid);
		break;
	case EBondType::Triple:
		TripleBondUids.Remove(BondUid);
		break;
	default:
		break;
	}
}

TArray<FGuid> AChemicalBondGameDirector::GetAtomBondUidsForTermination(FGuid AtomUid, AAtomBase* Atom) const
{
	TArray<FGuid> Result;

	if (Atom)
	{
		Result = Atom->GetBondUids();
	}

	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FChemicalBondRegistryRecord& Record = BondPair.Value;
		if (Record.AtomAUid == AtomUid || Record.AtomBUid == AtomUid)
		{
			Result.AddUnique(BondPair.Key);
		}
	}

	return Result;
}

bool AChemicalBondGameDirector::DoesTypeListContainBond(FGuid BondUid, EBondType BondType) const
{
	switch (BondType)
	{
	case EBondType::Single:
		return SingleBondUids.Contains(BondUid);
	case EBondType::Double:
		return DoubleBondUids.Contains(BondUid);
	case EBondType::Triple:
		return TripleBondUids.Contains(BondUid);
	default:
		return false;
	}
}

bool AChemicalBondGameDirector::DoesOtherTypeListContainBond(FGuid BondUid, EBondType BondType) const
{
	if (BondType != EBondType::Single && SingleBondUids.Contains(BondUid))
	{
		return true;
	}

	if (BondType != EBondType::Double && DoubleBondUids.Contains(BondUid))
	{
		return true;
	}

	return BondType != EBondType::Triple && TripleBondUids.Contains(BondUid);
}

void AChemicalBondGameDirector::RebuildRegistriesFromAtomData()
{
	TArray<AAtomBase*> AtomsToRegister;

	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		if (AAtomBase* Atom = AtomPair.Value.Get())
		{
			AtomsToRegister.AddUnique(Atom);
		}
	}

	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AAtomBase> AtomIt(World); AtomIt; ++AtomIt)
		{
			AtomsToRegister.AddUnique(*AtomIt);
		}
	}

	AtomRegistry.Reset();
	BondRegistry.Reset();
	SingleBondUids.Reset();
	DoubleBondUids.Reset();
	TripleBondUids.Reset();

	auto EnsureAtomRegistered = [this](AAtomBase* Atom)
	{
		if (!Atom)
		{
			return FGuid();
		}

		FGuid AtomUid = Atom->GetAtomUid();
		if (AtomUid.IsValid())
		{
			if (const TWeakObjectPtr<AAtomBase>* ExistingAtomPtr = AtomRegistry.Find(AtomUid))
			{
				if (ExistingAtomPtr->Get() == Atom)
				{
					return AtomUid;
				}
			}
		}

		if (!AtomUid.IsValid() || AtomRegistry.Contains(AtomUid))
		{
			AtomUid = GenerateUniqueAtomUid();
			Atom->AssignAtomUid(AtomUid);
		}

		AtomRegistry.Add(AtomUid, Atom);
		return AtomUid;
	};

	for (AAtomBase* Atom : AtomsToRegister)
	{
		EnsureAtomRegistered(Atom);
	}

	for (AAtomBase* Atom : AtomsToRegister)
	{
		if (!Atom)
		{
			continue;
		}

		const FGuid AtomUid = Atom->GetAtomUid();
		for (const FBondRecord& LocalRecord : Atom->GetBonds())
		{
			if (!LocalRecord.BondUid.IsValid() || BondRegistry.Contains(LocalRecord.BondUid))
			{
				continue;
			}

			AAtomBase* PartnerAtom = LocalRecord.PartnerAtom.Get();
			const FGuid PartnerUid = EnsureAtomRegistered(PartnerAtom);

			FChemicalBondRegistryRecord DirectorRecord;
			DirectorRecord.BondUid = LocalRecord.BondUid;
			DirectorRecord.BondType = LocalRecord.BondType;
			DirectorRecord.AtomAUid = AtomUid;
			DirectorRecord.AtomBUid = PartnerUid;
			DirectorRecord.AtomA = Atom;
			DirectorRecord.AtomB = PartnerAtom;
			DirectorRecord.AtomASlotIndex = LocalRecord.MySlotIndex;
			DirectorRecord.AtomBSlotIndex = LocalRecord.PartnerSlotIndex;

			BondRegistry.Add(LocalRecord.BondUid, DirectorRecord);
			AddBondUidToTypeList(LocalRecord.BondUid, LocalRecord.BondType);
		}
	}
}

void AChemicalBondGameDirector::HandleBondRegistryMismatch(const FString& ErrorMessage)
{
	UE_LOG(LogChemicalBondDirector, Error,
		TEXT("[Game:Director] Bond registry mismatch detected. Atom-local data is authoritative; rebuilding Director registries before fatal assertion. Director=%s Error=%s"),
		*GetNameSafe(this),
		*ErrorMessage);

	RebuildRegistriesFromAtomData();

	UE_LOG(LogChemicalBondDirector, Fatal,
		TEXT("[Game:Director] Bond registry mismatch is fatal. Director registries were rebuilt from atom-local data before throwing. Director=%s Error=%s"),
		*GetNameSafe(this),
		*ErrorMessage);
}

void AChemicalBondGameDirector::MarkPlayerMoleculeDirty()
{
	bPlayerMoleculeDirty = true;
}

void AChemicalBondGameDirector::ProcessPlayerMoleculeDetection()
{
	if (!bPlayerMoleculeDirty)
	{
		return;
	}

	bPlayerMoleculeDirty = false;

	FChemicalBondMoleculeSnapshot Snapshot;
	if (!BuildPlayerMoleculeSnapshot(Snapshot))
	{
		RingDecisionQueue.Reset();
		bHasActiveRingDecision = false;
		return;
	}

	TArray<FChemicalBondRing> ClosedRings;
	TArray<FChemicalBondRingCandidate> RingCandidates;
	AnalyzeRings(Snapshot, ClosedRings, RingCandidates);
	EnqueueRingCandidates(RingCandidates);
	StartNextRingDecision();

	if (EvaluateVictory(Snapshot, ClosedRings))
	{
		ReportVictory();
	}

	TArray<FChemicalBondDebuffMatch> DebuffMatches;
	DetectDebuffGroups(Snapshot, DebuffMatches);
	for (const FChemicalBondDebuffMatch& Match : DebuffMatches)
	{
		ExecuteDebuff(Match);
	}

	OnPlayerMoleculeChanged();
}

void AChemicalBondGameDirector::OnPlayerMoleculeChanged()
{
	UE_LOG(LogChemicalBondDirector, Verbose, TEXT("[Game:Molecule] Player molecule detection refreshed."));
}

AAtomBase* AChemicalBondGameDirector::FindPlayerAnchorAtom() const
{
	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		AAtomBase* Atom = AtomPair.Value.Get();
		if (Atom && Atom->IsPlayerControlled())
		{
			return Atom;
		}
	}

	for (const TPair<FGuid, TWeakObjectPtr<AAtomBase>>& AtomPair : AtomRegistry)
	{
		AAtomBase* Atom = AtomPair.Value.Get();
		if (Atom && Atom->GetAtomState() == EAtomState::PlayerConnected)
		{
			return Atom;
		}
	}

	return nullptr;
}

bool AChemicalBondGameDirector::BuildPlayerMoleculeSnapshot(FChemicalBondMoleculeSnapshot& OutSnapshot) const
{
	OutSnapshot = FChemicalBondMoleculeSnapshot();

	AAtomBase* AnchorAtom = FindPlayerAnchorAtom();
	if (!AnchorAtom)
	{
		return false;
	}

	TArray<AAtomBase*> PendingAtoms;
	TSet<FGuid> VisitedAtomUids;
	PendingAtoms.Add(AnchorAtom);

	while (!PendingAtoms.IsEmpty())
	{
		AAtomBase* Atom = PendingAtoms.Pop(EAllowShrinking::No);
		if (!Atom)
		{
			continue;
		}

		const FGuid AtomUid = Atom->GetAtomUid();
		if (!AtomUid.IsValid() || VisitedAtomUids.Contains(AtomUid))
		{
			continue;
		}

		VisitedAtomUids.Add(AtomUid);

		FChemicalBondMoleculeNode Node;
		Node.AtomUid = AtomUid;
		Node.ElementType = Atom->GetElementType();
		Node.BaseElement = ChemicalBondElement::GetBaseElement(Node.ElementType);
		// 已拒绝全部候选或已完成一次闭合的成环原子，后续按普通原子处理。
		Node.bCanFormRing = Atom->CanFormRing()
			&& !DemotedRingAtomUids.Contains(AtomUid)
			&& !CompletedRingAtomUids.Contains(AtomUid);
		OutSnapshot.UidToIndex.Add(AtomUid, OutSnapshot.Nodes.Num());
		OutSnapshot.Nodes.Add(Node);

		int32& ElementCount = OutSnapshot.ElementCounts.FindOrAdd(ChemicalBondElement::GetBaseElement(Atom->GetElementType()));
		++ElementCount;

		for (const FBondRecord& BondRecord : Atom->GetBonds())
		{
			if (AAtomBase* PartnerAtom = BondRecord.PartnerAtom.Get())
			{
				PendingAtoms.Add(PartnerAtom);
			}
		}
	}

	for (const TPair<FGuid, FChemicalBondRegistryRecord>& BondPair : BondRegistry)
	{
		const FChemicalBondRegistryRecord& Record = BondPair.Value;
		const int32 AtomAIndex = OutSnapshot.IndexOfUid(Record.AtomAUid);
		const int32 AtomBIndex = OutSnapshot.IndexOfUid(Record.AtomBUid);
		if (AtomAIndex == INDEX_NONE || AtomBIndex == INDEX_NONE)
		{
			continue;
		}

		FChemicalBondMoleculeEdge EdgeToB;
		EdgeToB.NeighborIndex = AtomBIndex;
		EdgeToB.BondType = Record.BondType;
		EdgeToB.BondUid = Record.BondUid;
		OutSnapshot.Nodes[AtomAIndex].Edges.Add(EdgeToB);

		FChemicalBondMoleculeEdge EdgeToA;
		EdgeToA.NeighborIndex = AtomAIndex;
		EdgeToA.BondType = Record.BondType;
		EdgeToA.BondUid = Record.BondUid;
		OutSnapshot.Nodes[AtomBIndex].Edges.Add(EdgeToA);

		switch (Record.BondType)
		{
		case EBondType::Single:
			++OutSnapshot.SingleBondCount;
			break;
		case EBondType::Double:
			++OutSnapshot.DoubleBondCount;
			break;
		case EBondType::Triple:
			++OutSnapshot.TripleBondCount;
			break;
		default:
			break;
		}
	}

	return OutSnapshot.NumAtoms() > 0;
}

void AChemicalBondGameDirector::AnalyzeRings(
	const FChemicalBondMoleculeSnapshot& Snapshot,
	TArray<FChemicalBondRing>& OutClosedRings,
	TArray<FChemicalBondRingCandidate>& OutCandidates) const
{
	OutClosedRings.Reset();
	OutCandidates.Reset();

	TSet<FString> ClosedRingKeys;
	TSet<FString> CandidateKeys;

	auto MakeUidSetKey = [](const TArray<FGuid>& Uids)
	{
		TArray<FString> Parts;
		for (const FGuid& Uid : Uids)
		{
			Parts.Add(Uid.ToString(EGuidFormats::Digits));
		}
		Parts.Sort();
		return FString::Join(Parts, TEXT("|"));
	};

	for (int32 StartIndex = 0; StartIndex < Snapshot.Nodes.Num(); ++StartIndex)
	{
		TArray<int32> Path;
		TSet<int32> Visited;

		TFunction<void(int32)> WalkClosedRings = [&](int32 CurrentIndex)
		{
			if (Path.Num() > 6)
			{
				return;
			}

			const FChemicalBondMoleculeNode& CurrentNode = Snapshot.Nodes[CurrentIndex];
			for (const FChemicalBondMoleculeEdge& Edge : CurrentNode.Edges)
			{
				const int32 NeighborIndex = Edge.NeighborIndex;
				if (NeighborIndex == StartIndex && Path.Num() >= 4 && Path.Num() <= 6)
				{
					TArray<FGuid> RingUids;
					for (const int32 PathIndex : Path)
					{
						RingUids.Add(Snapshot.Nodes[PathIndex].AtomUid);
					}

					const FString RingKey = MakeUidSetKey(RingUids);
					if (!ClosedRingKeys.Contains(RingKey))
					{
						ClosedRingKeys.Add(RingKey);

						FChemicalBondRing Ring;
						Ring.NodeIndices = Path;
						Ring.Size = Path.Num();
						Ring.Topology = ChemicalBondElement::RingSizeToTopology(Ring.Size);
						OutClosedRings.Add(Ring);
					}
					continue;
				}

				if (NeighborIndex <= StartIndex || Visited.Contains(NeighborIndex) || Path.Num() >= 6)
				{
					continue;
				}

				Visited.Add(NeighborIndex);
				Path.Add(NeighborIndex);
				WalkClosedRings(NeighborIndex);
				Path.Pop(EAllowShrinking::No);
				Visited.Remove(NeighborIndex);
			}
		};

		Visited.Add(StartIndex);
		Path.Add(StartIndex);
		WalkClosedRings(StartIndex);
	}

	for (int32 StartIndex = 0; StartIndex < Snapshot.Nodes.Num(); ++StartIndex)
	{
		const FChemicalBondMoleculeNode& StartNode = Snapshot.Nodes[StartIndex];
		AAtomBase* RingAtom = GetAtomByUid(StartNode.AtomUid);
		if (!StartNode.bCanFormRing || !RingAtom || RingAtom->GetAvailableSlotCount() <= 0)
		{
			continue;
		}

		TArray<int32> Path;
		TSet<int32> Visited;
		Path.Add(StartIndex);
		Visited.Add(StartIndex);

		TFunction<void(int32)> WalkCandidates = [&](int32 CurrentIndex)
		{
			if (Path.Num() > 6)
			{
				return;
			}

			if (Path.Num() >= 4)
			{
				const FChemicalBondMoleculeNode& TargetNode = Snapshot.Nodes[CurrentIndex];
				AAtomBase* TargetAtom = GetAtomByUid(TargetNode.AtomUid);
				FGuid ExistingBondUid;
				EBondType ExistingBondType = EBondType::Single;
				if (TargetAtom
					&& TargetAtom != RingAtom
					&& TargetAtom->GetAvailableSlotCount() > 0
					&& !FindExistingBondBetween(RingAtom, TargetAtom, ExistingBondUid, ExistingBondType))
				{
					FChemicalBondRingCandidate Candidate;
					Candidate.RingAtomUid = StartNode.AtomUid;
					Candidate.TargetAtomUid = TargetNode.AtomUid;
					Candidate.RingSize = Path.Num();
					for (const int32 PathIndex : Path)
					{
						Candidate.PathAtomUids.Add(Snapshot.Nodes[PathIndex].AtomUid);
					}

					const FString CandidateKey = MakeUidSetKey(Candidate.PathAtomUids)
						+ TEXT("|")
						+ Candidate.RingAtomUid.ToString(EGuidFormats::Digits)
						+ TEXT("|")
						+ Candidate.TargetAtomUid.ToString(EGuidFormats::Digits);
					if (!CandidateKeys.Contains(CandidateKey))
					{
						CandidateKeys.Add(CandidateKey);
						OutCandidates.Add(Candidate);
					}
				}
			}

			if (Path.Num() >= 6)
			{
				return;
			}

			for (const FChemicalBondMoleculeEdge& Edge : Snapshot.Nodes[CurrentIndex].Edges)
			{
				if (Visited.Contains(Edge.NeighborIndex))
				{
					continue;
				}

				Visited.Add(Edge.NeighborIndex);
				Path.Add(Edge.NeighborIndex);
				WalkCandidates(Edge.NeighborIndex);
				Path.Pop(EAllowShrinking::No);
				Visited.Remove(Edge.NeighborIndex);
			}
		};

		WalkCandidates(StartIndex);
	}
}

void AChemicalBondGameDirector::EnqueueRingCandidates(const TArray<FChemicalBondRingCandidate>& Candidates)
{
	for (const FChemicalBondRingCandidate& Candidate : Candidates)
	{
		if (!IsRingCandidateQueuedOrActive(Candidate))
		{
			RingDecisionQueue.Add(Candidate);
		}
	}
}

bool AChemicalBondGameDirector::IsRingCandidateQueuedOrActive(const FChemicalBondRingCandidate& Candidate) const
{
	if (bHasActiveRingDecision && ActiveRingCandidate.IsSameCandidate(Candidate))
	{
		return true;
	}

	for (const FChemicalBondRingCandidate& QueuedCandidate : RingDecisionQueue)
	{
		if (QueuedCandidate.IsSameCandidate(Candidate))
		{
			return true;
		}
	}

	return false;
}

void AChemicalBondGameDirector::StartNextRingDecision()
{
	if (bHasActiveRingDecision)
	{
		return;
	}

	while (!RingDecisionQueue.IsEmpty())
	{
		ActiveRingCandidate = RingDecisionQueue.Last();
		RingDecisionQueue.RemoveAt(RingDecisionQueue.Num() - 1, 1, EAllowShrinking::No);
		ActiveRingClosingBondUid.Invalidate();
		bHasActiveRingDecision = true;

		if (DemotedRingAtomUids.Contains(ActiveRingCandidate.RingAtomUid)
			|| CompletedRingAtomUids.Contains(ActiveRingCandidate.RingAtomUid))
		{
			bHasActiveRingDecision = false;
			ActiveRingCandidate = FChemicalBondRingCandidate();
			continue;
		}

		if (IsActiveRingCandidateStillValid())
		{
			UE_LOG(LogChemicalBondDirector, Log,
				TEXT("[Game:Ring] Ring decision started. RingAtom=%s Target=%s Size=%d"),
				*ActiveRingCandidate.RingAtomUid.ToString(),
				*ActiveRingCandidate.TargetAtomUid.ToString(),
				ActiveRingCandidate.RingSize);
			return;
		}

		bHasActiveRingDecision = false;
	}
}

bool AChemicalBondGameDirector::HandleRingDecisionConfirmInput()
{
	if (!IsActiveRingCandidateStillValid())
	{
		FinishActiveRingDecision(true);
		return false;
	}

	AAtomBase* RingAtom = GetAtomByUid(ActiveRingCandidate.RingAtomUid);
	AAtomBase* TargetAtom = GetAtomByUid(ActiveRingCandidate.TargetAtomUid);
	if (!RingAtom || !TargetAtom)
	{
		FinishActiveRingDecision(true);
		return false;
	}

	if (!ActiveRingClosingBondUid.IsValid())
	{
		int32 RingSlot = INDEX_NONE;
		int32 TargetSlot = INDEX_NONE;
		if (!FindClosestFreeSlotPair(RingAtom, TargetAtom, RingSlot, TargetSlot))
		{
			FinishActiveRingDecision(true);
			return false;
		}

		ActiveRingClosingBondUid = LinkAtoms(RingAtom, TargetAtom, EBondType::Single, RingSlot, TargetSlot);
		if (!ActiveRingClosingBondUid.IsValid())
		{
			FinishActiveRingDecision(true);
			return false;
		}

		TArray<AAtomBase*> RingAtoms;
		for (const FGuid& AtomUid : ActiveRingCandidate.PathAtomUids)
		{
			RingAtoms.Add(GetAtomByUid(AtomUid));
		}
		ArrangeRingAsRegularPolygon(RingAtoms);
		RecapturePlayerGroupLayoutFromCurrent(FindPlayerAnchorAtom());
		CompletedRingAtomUids.Add(ActiveRingCandidate.RingAtomUid);
		for (int32 CandidateIndex = RingDecisionQueue.Num() - 1; CandidateIndex >= 0; --CandidateIndex)
		{
			if (RingDecisionQueue[CandidateIndex].RingAtomUid == ActiveRingCandidate.RingAtomUid)
			{
				RingDecisionQueue.RemoveAt(CandidateIndex, 1, EAllowShrinking::No);
			}
		}
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Ring] Ring atom completed and will be treated as normal atom in future ring detection. RingAtom=%s"),
			*ActiveRingCandidate.RingAtomUid.ToString());
		return true;
	}

	bool bFoundBond = false;
	const FChemicalBondRegistryRecord BondRecord = GetBondRecord(ActiveRingClosingBondUid, bFoundBond);
	if (!bFoundBond || BondRecord.BondType == EBondType::Triple)
	{
		FinishActiveRingDecision(false);
		return bFoundBond;
	}

	int32 RingSlot = INDEX_NONE;
	int32 TargetSlot = INDEX_NONE;
	if (!FindClosestFreeSlotPair(RingAtom, TargetAtom, RingSlot, TargetSlot))
	{
		FinishActiveRingDecision(false);
		return false;
	}

	const EBondType NewBondType = BondRecord.BondType == EBondType::Single ? EBondType::Double : EBondType::Triple;
	const bool bChanged = ChangeBondType(ActiveRingClosingBondUid, NewBondType, RingSlot, TargetSlot);
	if (bChanged)
	{
		MarkPlayerMoleculeDirty();
	}
	return bChanged;
}

bool AChemicalBondGameDirector::HandleRingDecisionRejectInput()
{
	if (!bHasActiveRingDecision)
	{
		return false;
	}

	// 已闭合：F 结束当前成环并保留已形成的环。
	if (ActiveRingClosingBondUid.IsValid())
	{
		FinishActiveRingDecision(false);
		return true;
	}

	// 未闭合：拒绝当前候选。
	const FGuid RejectedRingAtomUid = ActiveRingCandidate.RingAtomUid;

	bHasActiveRingDecision = false;
	ActiveRingCandidate = FChemicalBondRingCandidate();
	ActiveRingClosingBondUid.Invalidate();

	// 该成环原子在队列里是否还有其他候选。
	bool bRingAtomHasMoreCandidates = false;
	for (const FChemicalBondRingCandidate& QueuedCandidate : RingDecisionQueue)
	{
		if (QueuedCandidate.RingAtomUid == RejectedRingAtomUid)
		{
			bRingAtomHasMoreCandidates = true;
			break;
		}
	}

	// 玩家拒绝了该成环原子的全部成环可能性：降级为普通原子，后续不再检测成环。
	if (!bRingAtomHasMoreCandidates && RejectedRingAtomUid.IsValid())
	{
		DemotedRingAtomUids.Add(RejectedRingAtomUid);
		UE_LOG(LogChemicalBondDirector, Log,
			TEXT("[Game:Ring] Ring atom demoted to normal after all candidates rejected. RingAtom=%s"),
			*RejectedRingAtomUid.ToString());
	}

	StartNextRingDecision();
	return true;
}

void AChemicalBondGameDirector::FinishActiveRingDecision(bool bRejected)
{
	// 只清当前激活的成环决策状态，不清空队列；队列中其他成环原子的候选继续依次处理。
	bHasActiveRingDecision = false;
	ActiveRingCandidate = FChemicalBondRingCandidate();
	ActiveRingClosingBondUid.Invalidate();

	StartNextRingDecision();
}

bool AChemicalBondGameDirector::IsActiveRingCandidateStillValid() const
{
	if (!bHasActiveRingDecision)
	{
		return false;
	}

	AAtomBase* RingAtom = GetAtomByUid(ActiveRingCandidate.RingAtomUid);
	AAtomBase* TargetAtom = GetAtomByUid(ActiveRingCandidate.TargetAtomUid);
	if (!RingAtom || !TargetAtom || RingAtom == TargetAtom)
	{
		return false;
	}

	if (!ActiveRingClosingBondUid.IsValid()
		&& (RingAtom->GetAvailableSlotCount() <= 0 || TargetAtom->GetAvailableSlotCount() <= 0))
	{
		return false;
	}

	for (const FGuid& AtomUid : ActiveRingCandidate.PathAtomUids)
	{
		if (!GetAtomByUid(AtomUid))
		{
			return false;
		}
	}

	return ActiveRingCandidate.RingSize >= 4 && ActiveRingCandidate.RingSize <= 6;
}

void AChemicalBondGameDirector::ArrangeRingAsRegularPolygon(const TArray<AAtomBase*>& RingAtoms)
{
	const int32 RingSize = RingAtoms.Num();
	if (RingSize < 4)
	{
		return;
	}

	FVector Center = FVector::ZeroVector;
	for (AAtomBase* Atom : RingAtoms)
	{
		if (!Atom)
		{
			return;
		}
		Center += ChemicalBondGameplayPlane::ProjectLocation(Atom->GetActorLocation());
	}
	Center /= RingSize;

	const float Radius = RingPolygonEdgeLength / (2.f * FMath::Sin(PI / static_cast<float>(RingSize)));
	for (int32 AtomIndex = 0; AtomIndex < RingSize; ++AtomIndex)
	{
		AAtomBase* Atom = RingAtoms[AtomIndex];
		const float AngleRadians = 2.f * PI * static_cast<float>(AtomIndex) / static_cast<float>(RingSize);
		const FVector Location = ChemicalBondGameplayPlane::ProjectLocation(
			Center + FVector(FMath::Cos(AngleRadians) * Radius, FMath::Sin(AngleRadians) * Radius, 0.f));
		Atom->SetActorLocation(Location, false);
		StopConstrainedAtomMotion(Atom);
	}

	for (int32 AtomIndex = 0; AtomIndex < RingSize; ++AtomIndex)
	{
		AAtomBase* Atom = RingAtoms[AtomIndex];
		Atom->ClearAllRingSlotAngleOverrides();

		const int32 PrevIndex = (AtomIndex + RingSize - 1) % RingSize;
		const int32 NextIndex = (AtomIndex + 1) % RingSize;
		const TSet<AAtomBase*> RingNeighbors = { RingAtoms[PrevIndex], RingAtoms[NextIndex] };

		for (const FBondRecord& BondRecord : Atom->GetBonds())
		{
			AAtomBase* PartnerAtom = BondRecord.PartnerAtom.Get();
			if (!RingNeighbors.Contains(PartnerAtom))
			{
				continue;
			}

			const FVector Direction = ChemicalBondGameplayPlane::ProjectVector(
				PartnerAtom->GetActorLocation() - Atom->GetActorLocation());
			const float WorldYaw = GetPlanarYawDegrees(Direction);
			const float LocalYaw = FindShortestAngleDegrees(Atom->GetActorRotation().Yaw, WorldYaw);
			Atom->SetRingSlotAngleOverrideDegrees(BondRecord.MySlotIndex, LocalYaw);
			for (const int32 SlotIndex : BondRecord.MySlotIndices)
			{
				Atom->SetRingSlotAngleOverrideDegrees(SlotIndex, LocalYaw);
			}
		}

		Atom->NotifyBondLayoutChanged();
	}

	UpdateAllBondVisuals();
}

void AChemicalBondGameDirector::RecapturePlayerGroupLayoutFromCurrent(AAtomBase* AnchorAtom)
{
	if (!AnchorAtom)
	{
		return;
	}

	TArray<AAtomBase*> GroupAtoms;
	TSet<FAtomInteractionPairKey> GroupPairKeys;
	CollectBondedGroup(AnchorAtom, GroupAtoms, GroupPairKeys);

	TMap<FGuid, FTransform>& LocalTransforms = RigidGroupLocalTransforms.FindOrAdd(AnchorAtom->GetAtomUid());
	LocalTransforms.Reset();
	const FTransform AnchorTransform = AnchorAtom->GetActorTransform();
	for (AAtomBase* GroupAtom : GroupAtoms)
	{
		if (GroupAtom && GroupAtom != AnchorAtom)
		{
			LocalTransforms.Add(GroupAtom->GetAtomUid(), GroupAtom->GetActorTransform().GetRelativeTransform(AnchorTransform));
		}
	}
}

bool AChemicalBondGameDirector::EvaluateVictory(
	const FChemicalBondMoleculeSnapshot& Snapshot,
	const TArray<FChemicalBondRing>& ClosedRings) const
{
	if (!LevelGoalAsset || bVictoryReported)
	{
		return false;
	}

	FString UnmetReason;
	const bool bGoalMet = LevelGoalAsset->EvaluateAgainst(Snapshot, ClosedRings, UnmetReason);
	if (!bGoalMet)
	{
		UE_LOG(LogChemicalBondDirector, Verbose, TEXT("[Game:Victory] Goal not met. Reason=%s"), *UnmetReason);
	}
	return bGoalMet;
}

void AChemicalBondGameDirector::ReportVictory()
{
	if (bVictoryReported)
	{
		return;
	}

	bVictoryReported = true;
	UE_LOG(LogChemicalBondDirector, Log, TEXT("[Game:Victory] Level goal met."));
}

void AChemicalBondGameDirector::DetectDebuffGroups(
	const FChemicalBondMoleculeSnapshot& Snapshot,
	TArray<FChemicalBondDebuffMatch>& OutMatches) const
{
	OutMatches.Reset();
	TSet<FString> MatchKeys;

	auto AddMatch = [&OutMatches, &MatchKeys](EChemicalBondDebuffType DebuffType, TArray<FGuid> MemberUids)
	{
		TArray<FString> Parts;
		for (const FGuid& Uid : MemberUids)
		{
			Parts.Add(Uid.ToString(EGuidFormats::Digits));
		}
		Parts.Sort();
		const FString Key = FString::FromInt(static_cast<int32>(DebuffType)) + TEXT("|") + FString::Join(Parts, TEXT("|"));
		if (MatchKeys.Contains(Key))
		{
			return;
		}

		MatchKeys.Add(Key);
		FChemicalBondDebuffMatch Match;
		Match.DebuffType = DebuffType;
		Match.MemberAtomUids = MoveTemp(MemberUids);
		OutMatches.Add(Match);
	};

	for (int32 NodeIndex = 0; NodeIndex < Snapshot.Nodes.Num(); ++NodeIndex)
	{
		const FChemicalBondMoleculeNode& Node = Snapshot.Nodes[NodeIndex];

		if (Node.BaseElement == EChemicalBondBaseElement::Nitrogen)
		{
			TArray<FGuid> DoubleOxygens;
			for (const FChemicalBondMoleculeEdge& Edge : Node.Edges)
			{
				const FChemicalBondMoleculeNode& Neighbor = Snapshot.Nodes[Edge.NeighborIndex];
				if (Neighbor.BaseElement == EChemicalBondBaseElement::Oxygen && Edge.BondType == EBondType::Double)
				{
					DoubleOxygens.Add(Neighbor.AtomUid);
				}
			}

			if (DoubleOxygens.Num() >= 2)
			{
				AddMatch(EChemicalBondDebuffType::NitroNO2, { Node.AtomUid, DoubleOxygens[0], DoubleOxygens[1] });
			}
		}

		for (const FChemicalBondMoleculeEdge& Edge : Node.Edges)
		{
			const FChemicalBondMoleculeNode& Neighbor = Snapshot.Nodes[Edge.NeighborIndex];
			if (Node.BaseElement == EChemicalBondBaseElement::Carbon
				&& Neighbor.BaseElement == EChemicalBondBaseElement::Nitrogen
				&& Edge.BondType == EBondType::Triple)
			{
				AddMatch(EChemicalBondDebuffType::CyanoCN, { Node.AtomUid, Neighbor.AtomUid });
			}

			if (Node.BaseElement == EChemicalBondBaseElement::Oxygen
				&& Neighbor.BaseElement == EChemicalBondBaseElement::Oxygen
				&& Edge.BondType == EBondType::Single)
			{
				AddMatch(EChemicalBondDebuffType::PeroxideOO, { Node.AtomUid, Neighbor.AtomUid });
			}

			if (Node.BaseElement == EChemicalBondBaseElement::Nitrogen
				&& Neighbor.BaseElement == EChemicalBondBaseElement::Carbon
				&& Edge.BondType == EBondType::Double)
			{
				for (const FChemicalBondMoleculeEdge& CarbonEdge : Neighbor.Edges)
				{
					const FChemicalBondMoleculeNode& CarbonNeighbor = Snapshot.Nodes[CarbonEdge.NeighborIndex];
					if (CarbonNeighbor.BaseElement == EChemicalBondBaseElement::Oxygen
						&& CarbonEdge.BondType == EBondType::Double)
					{
						AddMatch(EChemicalBondDebuffType::IsocyanateNCO, { Node.AtomUid, Neighbor.AtomUid, CarbonNeighbor.AtomUid });
					}
				}
			}
		}
	}
}

void AChemicalBondGameDirector::ExecuteDebuff(const FChemicalBondDebuffMatch& Match)
{
	// 占位实现：当前迭代只识别危害基团存在并触发效果入口，不实现具体危害效果。
	const TCHAR* DebuffTypeName = TEXT("Unknown");
	switch (Match.DebuffType)
	{
	case EChemicalBondDebuffType::NitroNO2:
		DebuffTypeName = TEXT("-NO2");
		break;
	case EChemicalBondDebuffType::CyanoCN:
		DebuffTypeName = TEXT("-C三N");
		break;
	case EChemicalBondDebuffType::PeroxideOO:
		DebuffTypeName = TEXT("-O-O-");
		break;
	case EChemicalBondDebuffType::IsocyanateNCO:
		DebuffTypeName = TEXT("-N=C=O");
		break;
	default:
		break;
	}

	UE_LOG(LogChemicalBondDirector, Warning,
		TEXT("[Game:Debuff] Debuff group present and triggered effect. Type=%s MemberCount=%d"),
		DebuffTypeName,
		Match.MemberAtomUids.Num());
}
