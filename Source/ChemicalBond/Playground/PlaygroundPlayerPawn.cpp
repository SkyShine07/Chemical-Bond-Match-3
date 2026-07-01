#include "PlaygroundPlayerPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
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

namespace
{
	constexpr float RefreshBoxSafeHalfHeight = 10000.f;
}

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
	CameraBoom->SetWorldRotation(FRotator(-90.f, 0.f, 0.f));
	CameraBoom->bDoCollisionTest = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);

	auto ConfigureRefreshBox = [this](UBoxComponent* RefreshBox)
	{
		RefreshBox->SetupAttachment(RootComponent);
		RefreshBox->SetUsingAbsoluteRotation(true);
		RefreshBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RefreshBox->SetGenerateOverlapEvents(false);
		RefreshBox->SetHiddenInGame(true);
		RefreshBox->SetBoxExtent(FVector(1.f, 1.f, RefreshBoxSafeHalfHeight), false);
	};

	ViewPortBox = CreateDefaultSubobject<UBoxComponent>(TEXT("ViewPortBox"));
	ConfigureRefreshBox(ViewPortBox);

	LogicBox = CreateDefaultSubobject<UBoxComponent>(TEXT("LogicBox"));
	ConfigureRefreshBox(LogicBox);

	LifeSpanLimit = CreateDefaultSubobject<UBoxComponent>(TEXT("LifeSpanLimit"));
	ConfigureRefreshBox(LifeSpanLimit);
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

	UpdateRefreshRangeBoxes();
}

void APlaygroundPlayerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	PollPlaygroundInput();
	UpdateRefreshRangeBoxes();
}

bool APlaygroundPlayerPawn::GetRefreshRangeSnapshot(
	FVector& OutCenter,
	float& OutYawDegrees,
	FVector2D& OutViewPortHalfExtent,
	FVector2D& OutLogicHalfExtent,
	FVector2D& OutLifeSpanHalfExtent) const
{
	if (!ViewPortBox || !LogicBox || !LifeSpanLimit)
	{
		return false;
	}

	const FVector ViewPortExtent = ViewPortBox->GetScaledBoxExtent();
	const FVector LogicExtent = LogicBox->GetScaledBoxExtent();
	const FVector LifeSpanExtent = LifeSpanLimit->GetScaledBoxExtent();
	if (ViewPortExtent.X <= 0.f || ViewPortExtent.Y <= 0.f
		|| LogicExtent.X <= 0.f || LogicExtent.Y <= 0.f
		|| LifeSpanExtent.X <= 0.f || LifeSpanExtent.Y <= 0.f)
	{
		return false;
	}

	OutCenter = ChemicalBondGameplayPlane::ProjectLocation(LifeSpanLimit->GetComponentLocation());
	OutYawDegrees = LifeSpanLimit->GetComponentRotation().Yaw;
	OutViewPortHalfExtent = FVector2D(ViewPortExtent.X, ViewPortExtent.Y);
	OutLogicHalfExtent = FVector2D(LogicExtent.X, LogicExtent.Y);
	OutLifeSpanHalfExtent = FVector2D(LifeSpanExtent.X, LifeSpanExtent.Y);
	return true;
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

void APlaygroundPlayerPawn::UpdateRefreshRangeBoxes()
{
	const FVector2D CameraVisibleSize = CalculateCameraVisibleSize();
	if (CameraVisibleSize.IsNearlyZero())
	{
		return;
	}

	const float LogicScale = GetLogicRefreshScale();
	const float LifeSpanScale = GetLifeSpanRefreshScale();
	const FRotator RangeRotation(0.f, Camera ? Camera->GetComponentRotation().Yaw : 0.f, 0.f);

	auto UpdateBox = [&CameraVisibleSize, &RangeRotation](UBoxComponent* RefreshBox, float Scale)
	{
		if (!RefreshBox)
		{
			return;
		}

		RefreshBox->SetWorldRotation(RangeRotation);
		RefreshBox->SetBoxExtent(
			FVector(
				CameraVisibleSize.X * 0.5f * Scale,
				CameraVisibleSize.Y * 0.5f * Scale,
				RefreshBoxSafeHalfHeight),
			false);
	};

	UpdateBox(ViewPortBox, 1.f);
	UpdateBox(LogicBox, LogicScale);
	UpdateBox(LifeSpanLimit, LifeSpanScale);
}

FVector2D APlaygroundPlayerPawn::CalculateCameraVisibleSize() const
{
	if (!Camera)
	{
		return FVector2D::ZeroVector;
	}

	const float AspectRatio = FMath::Max(Camera->AspectRatio, KINDA_SMALL_NUMBER);
	if (Camera->ProjectionMode == ECameraProjectionMode::Orthographic)
	{
		const float Width = FMath::Max(Camera->OrthoWidth, 0.f);
		return FVector2D(Width, Width / AspectRatio);
	}

	const FVector CameraLocation = Camera->GetComponentLocation();
	const FVector CameraForward = Camera->GetForwardVector();
	if (FMath::IsNearlyZero(CameraForward.Z))
	{
		return FVector2D::ZeroVector;
	}

	const float DistanceToGameplayPlane = FMath::Abs((GetActorLocation().Z - CameraLocation.Z) / CameraForward.Z);
	const float HalfHorizontalSize =
		FMath::Tan(FMath::DegreesToRadians(Camera->FieldOfView * 0.5f)) * DistanceToGameplayPlane;
	const float Width = HalfHorizontalSize * 2.f;
	return FVector2D(Width, Width / AspectRatio);
}

float APlaygroundPlayerPawn::GetLogicRefreshScale() const
{
	UWorld* World = GetWorld();
	const AChemicalBondGameMode* ChemicalBondGameMode =
		World ? World->GetAuthGameMode<AChemicalBondGameMode>() : nullptr;
	return FMath::Max(ChemicalBondGameMode ? ChemicalBondGameMode->GetLogicSizeAdopter() : 1.2f, 0.01f);
}

float APlaygroundPlayerPawn::GetLifeSpanRefreshScale() const
{
	UWorld* World = GetWorld();
	const AChemicalBondGameMode* ChemicalBondGameMode =
		World ? World->GetAuthGameMode<AChemicalBondGameMode>() : nullptr;
	return FMath::Max(ChemicalBondGameMode ? ChemicalBondGameMode->GetLifeSpanSizeAdopter() : 2.f, 0.01f);
}
