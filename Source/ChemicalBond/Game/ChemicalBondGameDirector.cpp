// Fill out your copyright notice in the Description page of Project Settings.

#include "ChemicalBondGameDirector.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "../AtomBase.h"
#include "ChemicalBondGameMode.h"
#include "../Movement/FluidMotionComponent.h"
#include "AI/NavigationSystemBase.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"

DEFINE_LOG_CATEGORY(LogChemicalBondDirector);

namespace
{
// TODO: 该临时连接距离需要在正式原子模型尺寸和策划数值表确认后改为配置项。
constexpr float TemporaryConnectedAtomDistance = 90.f;
constexpr float ConnectionConstraintTolerance = 1.f;
constexpr float PendingConnectionPullAlpha = 0.18f;
constexpr float PendingConnectionMaxStep = 30.f;

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
}

AChemicalBondGameDirector::AChemicalBondGameDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
}

void AChemicalBondGameDirector::BeginPlay()
{
	Super::BeginPlay();
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
	bHasActiveDecisionRequest = false;
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
	if (Atom)
	{
		Atom->ClearAtomUid();
	}

	AssertBondRegistryConsistency();
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
	AssertBondRegistryConsistency();
	return BondUid;
}

bool AChemicalBondGameDirector::CutBond(FGuid BondUid)
{
	if (!BondUid.IsValid())
	{
		return false;
	}

	AssertBondRegistryConsistency();

	FChemicalBondRegistryRecord Record;
	if (!BondRegistry.RemoveAndCopyValue(BondUid, Record))
	{
		return false;
	}

	RemoveBondUidFromTypeList(BondUid, Record.BondType);
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

	ApplyGentleRepulsion(AtomA, AtomB, TEXT("CutBond"));
	AssertBondRegistryConsistency();
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
}

FVector AChemicalBondGameDirector::GetViewBoxRange(UCameraComponent* Camera,float SpringArmLength,float DeltaTime)
{
	FVector HalfSize;

	if (Camera)
	{
		FMinimalViewInfo ViewInfo;
		Camera->GetCameraView(DeltaTime,ViewInfo);
		
		HalfSize.Y=FMath::Tan(FMath::DegreesToRadians(ViewInfo.FOV/2))*SpringArmLength;
		HalfSize.X=HalfSize.Y*ViewInfo.AspectRatio;
		HalfSize.Z=SpringArmLength/2;
	}
	
	return  HalfSize;
	
}

TArray<FVector> AChemicalBondGameDirector::SplitBoxRange(FVector BigBoxHalfSize,FVector& SubBoxHalfSize,uint8 SplitNum)
{
	TArray<FVector> BoxsCenter;

	// 获得实际范围
	FVector BigBoxSize=BigBoxHalfSize*2;
	
	
	// 计算8个区域的中心坐标
	
	BoxsCenter.Add(FVector(-1*BigBoxSize.X/(SplitNum+1),BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	BoxsCenter.Add(FVector(0,BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	BoxsCenter.Add(FVector(BigBoxSize.X/(SplitNum+1),BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	
	BoxsCenter.Add(FVector(-1*BigBoxSize.X/(SplitNum+1),0,BigBoxSize.Z));
	BoxsCenter.Add(FVector(BigBoxSize.X/(SplitNum+1),0,BigBoxSize.Z));
	
	BoxsCenter.Add(FVector(-1*BigBoxSize.X/(SplitNum+1),-1*BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	BoxsCenter.Add(FVector(0,BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	BoxsCenter.Add(FVector(BigBoxSize.X/(SplitNum+1),-1*BigBoxSize.Y/(SplitNum+1),BigBoxSize.Z));
	
	//计算子区域的大小范围
	SubBoxHalfSize=BigBoxSize/(SplitNum+1);
	
	return BoxsCenter;
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

		const int32 AtomASlot = AtomA->FindFreeSlotIndex();
		const int32 AtomBSlot = AtomB->FindFreeSlotIndex();
		if (AtomASlot == INDEX_NONE || AtomBSlot == INDEX_NONE)
		{
			ApplyGentleRepulsion(AtomA, AtomB, TEXT("NoFreeSlot"));
			return;
		}

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
		bHasActiveDecisionRequest = true;
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
		const int32 ConnectedSlot = ConnectedAtom->FindFreeSlotIndex();
		const int32 FreeSlot = FreeAtom->FindFreeSlotIndex();
		if (ConnectedSlot == INDEX_NONE || FreeSlot == INDEX_NONE)
		{
			FinishActiveDecision(false);
			return false;
		}

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
		const int32 ConnectedSlot = ConnectedAtom->FindFreeSlotIndex();
		const int32 FreeSlot = FreeAtom->FindFreeSlotIndex();
		if (ConnectedSlot == INDEX_NONE || FreeSlot == INDEX_NONE)
		{
			FinishActiveDecision(false);
			return false;
		}

		const EBondType NewBondType = CurrentBondType == EBondType::Single ? EBondType::Double : EBondType::Triple;
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
