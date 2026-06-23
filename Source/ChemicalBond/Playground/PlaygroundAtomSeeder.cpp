#include "PlaygroundAtomSeeder.h"

#include "../AtomTypes.h"
#include "../Movement/FluidMotionComponent.h"
#include "PlaygroundAtom.h"

APlaygroundAtomSeeder::APlaygroundAtomSeeder()
{
	PrimaryActorTick.bCanEverTick = false;
	AtomClass = APlaygroundAtom::StaticClass();

	ElementPool = {
		EAtomElementType::H,
		EAtomElementType::C_Normal,
		EAtomElementType::O_Normal,
		EAtomElementType::N_Normal,
		EAtomElementType::P_Normal,
		EAtomElementType::C_Ring,
		EAtomElementType::O_Ring,
		EAtomElementType::N_Ring
	};
}

void APlaygroundAtomSeeder::BeginPlay()
{
	Super::BeginPlay();
	SpawnAtoms();
}

void APlaygroundAtomSeeder::SpawnAtoms()
{
	if (!AtomClass)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Game:Playground] Cannot spawn atoms because AtomClass is null. Seeder=%s"),
			*GetNameSafe(this));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Game:Playground] Cannot spawn atoms because World is null. Seeder=%s"),
			*GetNameSafe(this));
		return;
	}

	FRandomStream RandomStream(RandomSeed);
	for (int32 Index = 0; Index < SpawnCount; ++Index)
	{
		const FVector SpawnLocation = ChemicalBondGameplayPlane::ProjectLocation(
			GetActorLocation() + GetRandomSpawnOffset(RandomStream));
		const FRotator SpawnRotation(0.f, RandomStream.FRandRange(0.f, 360.f), 0.f);
		const FTransform SpawnTransform(SpawnRotation, SpawnLocation);

		APlaygroundAtom* Atom = World->SpawnActorDeferred<APlaygroundAtom>(
			AtomClass,
			SpawnTransform,
			this,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Atom)
		{
			continue;
		}

		Atom->ConfigurePlaygroundAtom(PickElementType(RandomStream));
		Atom->FinishSpawning(SpawnTransform);

		if (UFluidMotionComponent* FluidMotion = Atom->GetFluidMotionComponent())
		{
			const FVector ImpulseDirection(
				RandomStream.FRandRange(-1.f, 1.f),
				RandomStream.FRandRange(-1.f, 1.f),
				0.f);
			FluidMotion->AddLinearImpulse(ImpulseDirection.GetSafeNormal() * InitialImpulseStrength);
		}
	}
}

FVector APlaygroundAtomSeeder::GetRandomSpawnOffset(FRandomStream& RandomStream) const
{
	const float SafeInnerRadius = FMath::Clamp(InnerClearRadius, 0.f, SpawnRadius);
	const float Radius = RandomStream.FRandRange(SafeInnerRadius, SpawnRadius);
	const float AngleRadians = RandomStream.FRandRange(0.f, UE_TWO_PI);
	return FVector(FMath::Cos(AngleRadians) * Radius, FMath::Sin(AngleRadians) * Radius, 0.f);
}

EAtomElementType APlaygroundAtomSeeder::PickElementType(FRandomStream& RandomStream) const
{
	if (ElementPool.IsEmpty())
	{
		return EAtomElementType::C_Normal;
	}

	const int32 ElementIndex = RandomStream.RandRange(0, ElementPool.Num() - 1);
	return ElementPool[ElementIndex];
}
