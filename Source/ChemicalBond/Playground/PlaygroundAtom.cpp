#include "PlaygroundAtom.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "UObject/ConstructorHelpers.h"

APlaygroundAtom::APlaygroundAtom()
{
	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualMesh->SetRelativeScale3D(FVector(0.65f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		VisualMesh->SetStaticMesh(SphereMesh.Object);
	}

	ElementLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ElementLabel"));
	ElementLabel->SetupAttachment(RootComponent);
	ElementLabel->SetHorizontalAlignment(EHTA_Center);
	ElementLabel->SetVerticalAlignment(EVRTA_TextCenter);
	ElementLabel->SetRelativeLocation(FVector(0.f, 0.f, 110.f));
	ElementLabel->SetWorldSize(90.f);

	ConfigurePlaygroundAtom(EAtomElementType::C_Normal);
}

void APlaygroundAtom::ConfigurePlaygroundAtom(EAtomElementType InElementType)
{
	float RuntimeMass = 1.f;
	int32 RuntimeSlots = 1;
	bool bRuntimeCanFormRing = false;
	GetPlaygroundAtomDefaults(InElementType, RuntimeMass, RuntimeSlots, bRuntimeCanFormRing);
	ApplyRuntimeAtomData(InElementType, RuntimeMass, RuntimeSlots, bRuntimeCanFormRing);
	RefreshVisuals();
}

void APlaygroundAtom::ConfigureElementType(EAtomElementType InElementType)
{
	ConfigurePlaygroundAtom(InElementType);
}

void APlaygroundAtom::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisuals();
}

void APlaygroundAtom::RefreshVisuals()
{
	const EAtomElementType CurrentElementType = GetElementType();
	const FLinearColor ElementColor = GetElementColor(CurrentElementType);

	if (VisualMesh)
	{
		const FVector ElementColorVector(ElementColor.R, ElementColor.G, ElementColor.B);
		VisualMesh->SetVectorParameterValueOnMaterials(TEXT("Color"), ElementColorVector);
		VisualMesh->SetVectorParameterValueOnMaterials(TEXT("BaseColor"), ElementColorVector);
	}

	if (ElementLabel)
	{
		ElementLabel->SetText(GetElementLabelText(CurrentElementType));
		ElementLabel->SetTextRenderColor(ElementColor.ToFColor(true));
	}
}

void APlaygroundAtom::GetPlaygroundAtomDefaults(
	EAtomElementType InElementType,
	float& OutMass,
	int32& OutSlots,
	bool& bOutCanFormRing)
{
	bOutCanFormRing = false;

	switch (InElementType)
	{
	case EAtomElementType::H:
	case EAtomElementType::H_Normal:
		OutMass = 1.f;
		OutSlots = 1;
		break;
	case EAtomElementType::O_Normal:
		OutMass = 16.f;
		OutSlots = 2;
		break;
	case EAtomElementType::O_Ring:
		OutMass = 16.f;
		OutSlots = 2;
		bOutCanFormRing = true;
		break;
	case EAtomElementType::N_Normal:
		OutMass = 14.f;
		OutSlots = 3;
		break;
	case EAtomElementType::N_Ring:
		OutMass = 14.f;
		OutSlots = 3;
		bOutCanFormRing = true;
		break;
	case EAtomElementType::P_Normal:
		OutMass = 31.f;
		OutSlots = 5;
		break;
	case EAtomElementType::P_Ring:
		OutMass = 31.f;
		OutSlots = 5;
		bOutCanFormRing = true;
		break;
	case EAtomElementType::C_Ring:
		OutMass = 12.f;
		OutSlots = 4;
		bOutCanFormRing = true;
		break;
	case EAtomElementType::C_Player:
	case EAtomElementType::C_Normal:
	default:
		OutMass = 12.f;
		OutSlots = 4;
		break;
	}
}

FText APlaygroundAtom::GetElementLabelText(EAtomElementType InElementType)
{
	switch (InElementType)
	{
	case EAtomElementType::H:
	case EAtomElementType::H_Normal:
		return FText::FromString(TEXT("H"));
	case EAtomElementType::O_Normal:
		return FText::FromString(TEXT("O"));
	case EAtomElementType::O_Ring:
		return FText::FromString(TEXT("O*"));
	case EAtomElementType::N_Normal:
		return FText::FromString(TEXT("N"));
	case EAtomElementType::N_Ring:
		return FText::FromString(TEXT("N*"));
	case EAtomElementType::P_Normal:
		return FText::FromString(TEXT("P"));
	case EAtomElementType::P_Ring:
		return FText::FromString(TEXT("P*"));
	case EAtomElementType::C_Player:
		return FText::FromString(TEXT("C_Player"));
	case EAtomElementType::C_Ring:
		return FText::FromString(TEXT("C*"));
	case EAtomElementType::C_Normal:
	default:
		return FText::FromString(TEXT("C"));
	}
}

FLinearColor APlaygroundAtom::GetElementColor(EAtomElementType InElementType)
{
	switch (InElementType)
	{
	case EAtomElementType::H:
	case EAtomElementType::H_Normal:
		return FLinearColor(0.9f, 0.95f, 1.f);
	case EAtomElementType::O_Normal:
	case EAtomElementType::O_Ring:
		return FLinearColor(0.95f, 0.2f, 0.24f);
	case EAtomElementType::N_Normal:
	case EAtomElementType::N_Ring:
		return FLinearColor(0.2f, 0.35f, 1.f);
	case EAtomElementType::P_Normal:
	case EAtomElementType::P_Ring:
		return FLinearColor(0.95f, 0.7f, 0.2f);
	case EAtomElementType::C_Player:
		return FLinearColor(0.15f, 0.9f, 0.65f);
	case EAtomElementType::C_Ring:
		return FLinearColor(0.45f, 0.95f, 0.55f);
	case EAtomElementType::C_Normal:
	default:
		return FLinearColor(0.35f, 0.35f, 0.35f);
	}
}
