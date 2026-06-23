#include "PlaygroundPlayerPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "../Game/ChemicalBondGameDirector.h"
#include "../Game/ChemicalBondGameMode.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"
#include "../Movement/FluidMotionComponent.h"
#include "UObject/ConstructorHelpers.h"

APlaygroundPlayerPawn::APlaygroundPlayerPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	CollisionRoot = CreateDefaultSubobject<USphereComponent>(TEXT("PawnCollision"));
	CollisionRoot->SetSphereRadius(70.f);
	CollisionRoot->SetCollisionProfileName(TEXT("Pawn"));
	CollisionRoot->SetupAttachment(RootComponent);

	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualMesh->SetRelativeScale3D(FVector(0.8f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		VisualMesh->SetStaticMesh(SphereMesh.Object);
	}

	ElementLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ElementLabel"));
	ElementLabel->SetupAttachment(RootComponent);
	ElementLabel->SetText(FText::FromString(TEXT("C_Player")));
	ElementLabel->SetHorizontalAlignment(EHTA_Center);
	ElementLabel->SetVerticalAlignment(EVRTA_TextCenter);
	ElementLabel->SetTextRenderColor(FColor(40, 230, 170));
	ElementLabel->SetWorldSize(110.f);
	ElementLabel->SetRelativeLocation(FVector(0.f, 0.f, 130.f));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->TargetArmLength = 1700.f;
	CameraBoom->SetWorldRotation(FRotator(-70.f, 0.f, 0.f));
	CameraBoom->bDoCollisionTest = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
}

void APlaygroundPlayerPawn::PreInitializeComponents()
{
	ApplyRuntimeAtomData(EAtomElementType::C_Player, PlayerAtomMass, 4, false);
	SetInitialAtomState(EAtomState::PlayerConnected);
	Super::PreInitializeComponents();
}

void APlaygroundPlayerPawn::BeginPlay()
{
	Super::BeginPlay();
	SetAtomState(EAtomState::PlayerConnected);

	if (UFluidMotionComponent* FluidMotion = GetFluidMotionComponent())
	{
		FluidMotion->SetEffectiveMass(PlayerAtomMass);
	}

	if (VisualMesh)
	{
		const FLinearColor PlayerColor(0.15f, 0.9f, 0.65f);
		const FVector PlayerColorVector(PlayerColor.R, PlayerColor.G, PlayerColor.B);
		VisualMesh->SetVectorParameterValueOnMaterials(TEXT("Color"), PlayerColorVector);
		VisualMesh->SetVectorParameterValueOnMaterials(TEXT("BaseColor"), PlayerColorVector);
	}
}

void APlaygroundPlayerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	PollPlaygroundInput();
}

void APlaygroundPlayerPawn::PollPlaygroundInput()
{
	UFluidMotionComponent* FluidMotion = GetFluidMotionComponent();
	if (!FluidMotion)
	{
		return;
	}

	const APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController)
	{
		FluidMotion->ClearMoveInput();
		FluidMotion->ClearYawInput();
		FluidMotion->SetSprintActive(false);
		bWasSpaceDown = false;
		bWasFDown = false;
		return;
	}

	FVector MoveDirection = FVector::ZeroVector;
	if (PlayerController->IsInputKeyDown(EKeys::W))
	{
		MoveDirection.X += 1.f;
	}
	if (PlayerController->IsInputKeyDown(EKeys::S))
	{
		MoveDirection.X -= 1.f;
	}
	if (PlayerController->IsInputKeyDown(EKeys::D))
	{
		MoveDirection.Y += 1.f;
	}
	if (PlayerController->IsInputKeyDown(EKeys::A))
	{
		MoveDirection.Y -= 1.f;
	}

	if (MoveDirection.IsNearlyZero())
	{
		FluidMotion->ClearMoveInput();
	}
	else
	{
		FluidMotion->SetMoveInput(MoveDirection, 1.f);
	}

	float YawInput = 0.f;
	if (PlayerController->IsInputKeyDown(EKeys::E))
	{
		YawInput += 1.f;
	}
	if (PlayerController->IsInputKeyDown(EKeys::Q))
	{
		YawInput -= 1.f;
	}
	FluidMotion->SetYawInput(YawInput);

	const bool bWantsSprint =
		PlayerController->IsInputKeyDown(EKeys::LeftShift) ||
		PlayerController->IsInputKeyDown(EKeys::RightShift);
	FluidMotion->SetSprintActive(bWantsSprint);

	PollConnectionDecisionInput(PlayerController);
}

void APlaygroundPlayerPawn::PollConnectionDecisionInput(const APlayerController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}

	const bool bSpaceDown = PlayerController->IsInputKeyDown(EKeys::SpaceBar);
	const bool bFDown = PlayerController->IsInputKeyDown(EKeys::F);

	UWorld* World = GetWorld();
	AChemicalBondGameMode* ChemicalBondGameMode = World ? World->GetAuthGameMode<AChemicalBondGameMode>() : nullptr;
	AChemicalBondGameDirector* GameDirector = ChemicalBondGameMode ? ChemicalBondGameMode->GetGameDirector() : nullptr;

	if (GameDirector && bSpaceDown && !bWasSpaceDown)
	{
		GameDirector->HandleDecisionConfirmInput();
	}

	if (GameDirector && bFDown && !bWasFDown)
	{
		GameDirector->HandleDecisionRejectInput();
	}

	bWasSpaceDown = bSpaceDown;
	bWasFDown = bFDown;
}
