// Fill out your copyright notice in the Description page of Project Settings.

#include "ChemicalBondGameDirector.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "../AtomBase.h"
#include "ChemicalBondGameMode.h"

DEFINE_LOG_CATEGORY(LogChemicalBondDirector);

AChemicalBondGameDirector::AChemicalBondGameDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
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
}

void AChemicalBondGameDirector::StopDirector()
{
	if (!bDirectorStarted)
	{
		return;
	}

	bDirectorStarted = false;
	SetActorTickEnabled(false);
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
		if (!AtomRegistry.Contains(ExistingUid))
		{
			AtomRegistry.Add(ExistingUid, Atom);
		}
		return ExistingUid;
	}

	const FGuid NewAtomUid = GenerateUniqueAtomUid();
	Atom->AssignAtomUid(NewAtomUid);
	AtomRegistry.Add(NewAtomUid, Atom);
	return NewAtomUid;
}

bool AChemicalBondGameDirector::TerminateAtom(AAtomBase* Atom)
{
	if (!Atom)
	{
		return false;
	}

	const FGuid AtomUid = Atom->GetAtomUid();
	if (!AtomUid.IsValid())
	{
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

	TWeakObjectPtr<AAtomBase>* AtomPtr = AtomRegistry.Find(AtomUid);
	AAtomBase* Atom = AtomPtr ? AtomPtr->Get() : nullptr;

	const TArray<FGuid> BondUidsToCut = GetAtomBondUidsForTermination(AtomUid, Atom);
	for (const FGuid& BondUid : BondUidsToCut)
	{
		CutBond(BondUid);
	}

	AtomRegistry.Remove(AtomUid);
	if (Atom)
	{
		Atom->ClearAtomUid();
	}

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
	return BondUid;
}

bool AChemicalBondGameDirector::CutBond(FGuid BondUid)
{
	if (!BondUid.IsValid())
	{
		return false;
	}

	FChemicalBondRegistryRecord Record;
	if (!BondRegistry.RemoveAndCopyValue(BondUid, Record))
	{
		return false;
	}

	RemoveBondUidFromTypeList(BondUid, Record.BondType);

	if (AAtomBase* AtomA = Record.AtomA.Get())
	{
		AtomA->RemoveBondByUid(BondUid);
	}

	if (AAtomBase* AtomB = Record.AtomB.Get())
	{
		AtomB->RemoveBondByUid(BondUid);
	}

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
