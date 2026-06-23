#include "FluidMotionComponent.h"

#include "../AtomTypes.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY(LogChemicalBondFluidMotion);

UFluidMotionComponent::UFluidMotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	EffectiveMass = DefaultMass;
	FloatingSeed = FMath::FRandRange(0.f, 1000.f);
}

void UFluidMotionComponent::BeginPlay()
{
	Super::BeginPlay();

	SetEffectiveMass(EffectiveMass > 0.f ? EffectiveMass : DefaultMass);
	ConstrainOwnerToGameplayPlane();
}

void UFluidMotionComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (DeltaTime <= 0.f)
	{
		return;
	}

	ApplyConfiguredForces();
	IntegrateLinearVelocity(DeltaTime);
	IntegrateYawVelocity(DeltaTime);
	MoveOwner(DeltaTime);
	RotateOwner(DeltaTime);
}

void UFluidMotionComponent::SetMoveInput(FVector WorldDirection, float Scale)
{
	if (WorldDirection.IsNearlyZero() || FMath::IsNearlyZero(Scale))
	{
		MoveInput = FVector::ZeroVector;
		return;
	}

	MoveInput = ChemicalBondGameplayPlane::ProjectVector(WorldDirection).GetSafeNormal() * FMath::Clamp(Scale, -1.f, 1.f);
}

void UFluidMotionComponent::ClearMoveInput()
{
	MoveInput = FVector::ZeroVector;
}

void UFluidMotionComponent::SetYawInput(float Scale)
{
	YawInput = FMath::Clamp(Scale, -1.f, 1.f);
}

void UFluidMotionComponent::ClearYawInput()
{
	YawInput = 0.f;
}

void UFluidMotionComponent::SetSprintActive(bool bActive)
{
	bSprintActive = bActive;
}

void UFluidMotionComponent::AddLinearForce(FVector Force)
{
	PendingForce += ChemicalBondGameplayPlane::ProjectVector(Force);
}

void UFluidMotionComponent::AddLinearImpulse(FVector Impulse)
{
	LinearVelocity += ChemicalBondGameplayPlane::ProjectVector(Impulse) / GetSafeMass();
	LinearVelocity = ChemicalBondGameplayPlane::ProjectVector(LinearVelocity);
}

void UFluidMotionComponent::AddYawTorque(float Torque)
{
	PendingYawTorque += Torque;
}

void UFluidMotionComponent::AddYawImpulse(float Impulse)
{
	YawVelocity += Impulse / GetSafeMass();
}

void UFluidMotionComponent::StopFluidMotion()
{
	LinearVelocity = FVector::ZeroVector;
	PendingForce = FVector::ZeroVector;
	MoveInput = FVector::ZeroVector;
	YawVelocity = 0.f;
	PendingYawTorque = 0.f;
	YawInput = 0.f;
	bSprintActive = false;
}

void UFluidMotionComponent::SetEffectiveMass(float NewMass)
{
	EffectiveMass = FMath::Max(NewMass, MinimumMass);
}

void UFluidMotionComponent::ApplyConfiguredForces()
{
	if (!MoveInput.IsNearlyZero())
	{
		const FVector MoveDirection = MoveInput.GetSafeNormal();
		const float MoveScale = MoveInput.Size();
		AddLinearForce(MoveDirection * MoveForce * MoveScale);

		if (bSprintActive)
		{
			AddLinearForce(MoveDirection * SprintForce * MoveScale);
		}
	}

	if (!FMath::IsNearlyZero(YawInput))
	{
		AddYawTorque(YawTorque * YawInput);
	}

	if (FloatingDisturbanceForce > 0.f)
	{
		const AActor* Owner = GetOwner();
		const float TimeSeconds = Owner ? Owner->GetGameTimeSinceCreation() : 0.f;
		AddLinearForce(GetFloatingForce(TimeSeconds));
	}
}

void UFluidMotionComponent::IntegrateLinearVelocity(float DeltaTime)
{
	const float Speed = LinearVelocity.Size();
	if (Speed > KINDA_SMALL_NUMBER)
	{
		const FVector DragDirection = -LinearVelocity.GetSafeNormal();
		const FVector LinearDragForce = DragDirection * LinearDrag * Speed;
		const FVector QuadraticDragForce = DragDirection * QuadraticDrag * Speed * Speed;
		PendingForce += LinearDragForce + QuadraticDragForce;
	}

	LinearVelocity += (PendingForce / GetSafeMass()) * DeltaTime;
	LinearVelocity = ChemicalBondGameplayPlane::ProjectVector(LinearVelocity);
	PendingForce = FVector::ZeroVector;

	if (MaxLinearSpeed > 0.f)
	{
		LinearVelocity = LinearVelocity.GetClampedToMaxSize(MaxLinearSpeed);
		LinearVelocity = ChemicalBondGameplayPlane::ProjectVector(LinearVelocity);
	}

	if (MoveInput.IsNearlyZero() && LinearVelocity.SizeSquared() <= FMath::Square(StopLinearSpeedThreshold))
	{
		LinearVelocity = FVector::ZeroVector;
	}
}

void UFluidMotionComponent::IntegrateYawVelocity(float DeltaTime)
{
	if (!FMath::IsNearlyZero(YawVelocity))
	{
		PendingYawTorque += -FMath::Sign(YawVelocity) * AngularDrag * FMath::Abs(YawVelocity);
	}

	YawVelocity += (PendingYawTorque / GetSafeMass()) * DeltaTime;
	PendingYawTorque = 0.f;

	if (MaxYawSpeed > 0.f)
	{
		YawVelocity = FMath::Clamp(YawVelocity, -MaxYawSpeed, MaxYawSpeed);
	}

	if (FMath::IsNearlyZero(YawInput) && FMath::Abs(YawVelocity) <= StopYawSpeedThreshold)
	{
		YawVelocity = 0.f;
	}
}

void UFluidMotionComponent::MoveOwner(float DeltaTime)
{
	if (LinearVelocity.IsNearlyZero())
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogChemicalBondFluidMotion, Warning,
			TEXT("[Game:FluidMotion] Cannot move because owner is null. Component=%s"),
			*GetNameSafe(this));
		return;
	}

	FHitResult Hit;
	Owner->AddActorWorldOffset(ChemicalBondGameplayPlane::ProjectVector(LinearVelocity) * DeltaTime, true, &Hit);
	if (Hit.IsValidBlockingHit())
	{
		LinearVelocity = FVector::VectorPlaneProject(LinearVelocity, Hit.Normal);
		LinearVelocity = ChemicalBondGameplayPlane::ProjectVector(LinearVelocity);
	}

	ConstrainOwnerToGameplayPlane();
}

void UFluidMotionComponent::RotateOwner(float DeltaTime)
{
	if (FMath::IsNearlyZero(YawVelocity))
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogChemicalBondFluidMotion, Warning,
			TEXT("[Game:FluidMotion] Cannot rotate because owner is null. Component=%s"),
			*GetNameSafe(this));
		return;
	}

	Owner->AddActorWorldRotation(FRotator(0.f, YawVelocity * DeltaTime, 0.f));
	ConstrainOwnerToGameplayPlane();
}

void UFluidMotionComponent::ConstrainOwnerToGameplayPlane() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const FVector CurrentLocation = Owner->GetActorLocation();
	const FVector PlaneLocation = ChemicalBondGameplayPlane::ProjectLocation(CurrentLocation);
	if (!CurrentLocation.Equals(PlaneLocation, KINDA_SMALL_NUMBER))
	{
		Owner->SetActorLocation(PlaneLocation, false);
	}
}

float UFluidMotionComponent::GetSafeMass() const
{
	return FMath::Max(EffectiveMass, MinimumMass);
}

FVector UFluidMotionComponent::GetFloatingForce(float TimeSeconds) const
{
	const float T = (TimeSeconds + FloatingSeed) * FloatingDisturbanceFrequency;
	const FVector Noise(
		FMath::Sin(T * 1.7f),
		FMath::Cos(T * 1.3f),
		0.f);

	return Noise.GetSafeNormal() * FloatingDisturbanceForce;
}
