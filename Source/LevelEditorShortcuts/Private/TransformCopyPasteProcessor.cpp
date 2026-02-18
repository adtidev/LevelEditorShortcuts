// TransformCopyPasteProcessor.cpp
// Editor-only input processor for transform shortcuts:
// Ctrl+C: Copies location/rotation of selected actor (normal copy still works)
// Ctrl+T: Pastes location/rotation to selected actor(s), keeps original scale
// Ctrl+B: Snap selected actor(s) to ground
// Ctrl+Shift+V: Paste into selected folder in World Outliner

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h" // For TActorIterator
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

class FTransformCopyPasteProcessor : public IInputProcessor
{
public:
	static TSharedPtr<FTransformCopyPasteProcessor> Instance;
	static FTransform CopiedTransform;
	static bool bHasCopiedTransform;

	// For deferred paste-to-folder
	static FName PendingPasteFolderPath;
	static TSet<AActor*> ActorsBeforePaste;
	static bool bPasteToFolderPending;

	static void Register()
	{
		if (!Instance.IsValid() && FSlateApplication::IsInitialized())
		{
			Instance = MakeShared<FTransformCopyPasteProcessor>();
			FSlateApplication::Get().RegisterInputPreProcessor(Instance);
			bHasCopiedTransform = false;
		}
	}

	static void Unregister()
	{
		if (Instance.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(Instance);
			Instance.Reset();
		}
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override
	{
		// Handle deferred paste-to-folder
		if (bPasteToFolderPending)
		{
			bPasteToFolderPending = false;
			CompletePasteToFolder();
		}
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		// Don't intercept input during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return false;
		}

		// Shift+B (without Ctrl) - Snap to ground (keeps world up rotation)
		// Check this BEFORE the Ctrl check so it doesn't get skipped
		if (InKeyEvent.GetKey() == EKeys::B && InKeyEvent.IsShiftDown() && !InKeyEvent.IsControlDown())
		{
			if (SnapSelectedToGroundNoRotation())
			{
				return true; // Consume the event
			}
		}

		// All remaining shortcuts require Ctrl
		if (!InKeyEvent.IsControlDown())
		{
			return false;
		}

		// Ctrl+C - Copy transform (don't consume, let normal copy happen too)
		if (InKeyEvent.GetKey() == EKeys::C)
		{
			CopySelectedTransform();
			return false; // Don't consume - allow normal Ctrl+C copy to proceed
		}

		// Ctrl+T - Paste transform
		if (InKeyEvent.GetKey() == EKeys::T)
		{
			if (PasteTransformToSelected())
			{
				return true; // Consume the event
			}
		}

		// Ctrl+B - Snap to ground (inherits surface rotation)
		if (InKeyEvent.GetKey() == EKeys::B && !InKeyEvent.IsShiftDown())
		{
			if (SnapSelectedToGround())
			{
				return true; // Consume the event
			}
		}

		// Ctrl+D - Duplicate in place (no offset)
		if (InKeyEvent.GetKey() == EKeys::D)
		{
			if (DuplicateInPlace())
			{
				return true; // Consume the event
			}
		}

		// Ctrl+Shift+V - Paste into selected folder
		if (InKeyEvent.GetKey() == EKeys::V && InKeyEvent.IsShiftDown())
		{
			SetupPasteToFolder();

			// Use Windows API to simulate Ctrl+V keypress
			// This ensures we use the exact same paste path as manual Ctrl+V
			INPUT inputs[4] = {};

			// Key down: Ctrl
			inputs[0].type = INPUT_KEYBOARD;
			inputs[0].ki.wVk = VK_CONTROL;

			// Key down: V
			inputs[1].type = INPUT_KEYBOARD;
			inputs[1].ki.wVk = 'V';

			// Key up: V
			inputs[2].type = INPUT_KEYBOARD;
			inputs[2].ki.wVk = 'V';
			inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

			// Key up: Ctrl
			inputs[3].type = INPUT_KEYBOARD;
			inputs[3].ki.wVk = VK_CONTROL;
			inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

			SendInput(4, inputs, sizeof(INPUT));

			return true; // Consume the original Ctrl+Shift+V
		}

		return false;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		return false;
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
	{
		return false;
	}

private:
	void CopySelectedTransform()
	{
		if (!GEditor)
		{
			return;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return;
		}

		// Get the first selected actor
		AActor* SelectedActor = Cast<AActor>(Selection->GetSelectedObject(0));
		if (!SelectedActor)
		{
			return;
		}

		CopiedTransform = SelectedActor->GetActorTransform();
		bHasCopiedTransform = true;
	}

	bool PasteTransformToSelected()
	{
		if (!GEditor || !bHasCopiedTransform)
		{
			return false;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return false;
		}

		// Create undo transaction
		FScopedTransaction Transaction(FText::FromString(TEXT("Paste Transform")));

		int32 NumModified = 0;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (Actor)
			{
				Actor->Modify();
				Actor->SetActorLocation(CopiedTransform.GetLocation());
				Actor->SetActorRotation(CopiedTransform.GetRotation().Rotator());
				// Keep original scale - don't apply CopiedTransform scale
				Actor->PostEditMove(true);
				NumModified++;
			}
		}

		if (NumModified > 0)
		{
			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}

		return false;
	}

	bool SnapSelectedToGround()
	{
		if (!GEditor)
		{
			return false;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return false;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return false;
		}

		// Create undo transaction
		FScopedTransaction Transaction(FText::FromString(TEXT("Snap to Ground")));

		int32 NumModified = 0;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (!Actor)
			{
				continue;
			}

			FVector ActorLocation = Actor->GetActorLocation();

			// Get mesh component's LOCAL bounds (not affected by animation bloat)
			// This gives us the actual mesh geometry bounds
			float MeshBottomOffset = 0.0f; // Distance from actor origin to mesh bottom

			if (USkeletalMeshComponent* SkelMesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
			{
				FBoxSphereBounds LocalBounds = SkelMesh->CalcLocalBounds();
				FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
				FVector WorldBottom = SkelMesh->GetComponentTransform().TransformPosition(LocalBottom);
				MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
			}
			else if (UStaticMeshComponent* StaticMesh = Actor->FindComponentByClass<UStaticMeshComponent>())
			{
				FBoxSphereBounds LocalBounds = StaticMesh->CalcLocalBounds();
				FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
				FVector WorldBottom = StaticMesh->GetComponentTransform().TransformPosition(LocalBottom);
				MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
			}
			else
			{
				// Find first primitive component with blocking collision (skip query-only spheres, triggers, etc.)
				UPrimitiveComponent* BestComp = nullptr;
				TArray<UPrimitiveComponent*> PrimComps;
				Actor->GetComponents<UPrimitiveComponent>(PrimComps);
				for (UPrimitiveComponent* Comp : PrimComps)
				{
					ECollisionEnabled::Type ColType = Comp->GetCollisionEnabled();
					if (ColType == ECollisionEnabled::QueryAndPhysics || ColType == ECollisionEnabled::PhysicsOnly)
					{
						BestComp = Comp;
						break;
					}
				}

				if (BestComp)
				{
					FBoxSphereBounds LocalBounds = BestComp->CalcLocalBounds();
					FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
					FVector WorldBottom = BestComp->GetComponentTransform().TransformPosition(LocalBottom);
					MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
				}
				// else: no physics collision component, use 0 offset (root = ground)
			}

			// Line trace from actor position downward
			FVector TraceStart = ActorLocation + FVector(0, 0, 500.f);
			FVector TraceEnd = TraceStart - FVector(0, 0, 200000.f);

			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(Actor);

			// Recursively ignore all attached actors
			TArray<AActor*> AttachedActors;
			Actor->GetAttachedActors(AttachedActors, true, true);
			for (AActor* Attached : AttachedActors)
			{
				QueryParams.AddIgnoredActor(Attached);
			}

			FHitResult HitResult;
			bool bHit = false;

			// Use channel trace (ECC_Visibility) - respects collision responses,
			// so query-only/overlap components are automatically skipped
			for (int32 Attempt = 0; Attempt < 50; Attempt++)
			{
				bHit = World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
				if (!bHit)
				{
					break;
				}

				// Also skip components with query-only collision (blocks Visibility but no physics)
				UPrimitiveComponent* HitComp = HitResult.GetComponent();
				if (HitComp)
				{
					ECollisionEnabled::Type CollisionType = HitComp->GetCollisionEnabled();
					if (CollisionType == ECollisionEnabled::QueryAndPhysics || CollisionType == ECollisionEnabled::PhysicsOnly)
					{
						break; // Valid collidable surface
					}
				}

				// Skip this specific component and trace again
				if (HitComp)
				{
					QueryParams.AddIgnoredComponent(HitComp);
				}
				bHit = false;
			}

			if (bHit)
			{
				Actor->Modify();

				FVector NewLocation = ActorLocation;
				NewLocation.Z = HitResult.ImpactPoint.Z + MeshBottomOffset + 5.0f;

				Actor->SetActorLocation(NewLocation);

				// Align actor rotation to surface normal (inherit slope)
				// Keep the actor's current facing direction but tilt to match surface
				FVector SurfaceNormal = HitResult.ImpactNormal;
				FRotator CurrentRotation = Actor->GetActorRotation();

				// Get actor's current forward direction (on XY plane)
				FVector CurrentForward = CurrentRotation.Vector();
				CurrentForward.Z = 0;
				CurrentForward.Normalize();

				// Build a rotation matrix from surface normal and desired forward
				// NewUp = surface normal
				// NewForward = project current forward onto the surface plane
				FVector NewUp = SurfaceNormal;
				FVector NewRight = FVector::CrossProduct(NewUp, CurrentForward);
				NewRight.Normalize();
				FVector NewForward = FVector::CrossProduct(NewRight, NewUp);
				NewForward.Normalize();

				// Build rotation from these axes
				FMatrix RotationMatrix = FMatrix::Identity;
				RotationMatrix.SetAxes(&NewForward, &NewRight, &NewUp);
				FRotator AlignedRotation = RotationMatrix.Rotator();

				Actor->SetActorRotation(AlignedRotation);
				Actor->PostEditMove(true);

				NumModified++;
			}
		}

		if (NumModified > 0)
		{
			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}

		return false;
	}

	// Snap to ground WITHOUT inheriting surface rotation (keeps world up)
	bool SnapSelectedToGroundNoRotation()
	{
		if (!GEditor)
		{
			return false;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return false;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return false;
		}

		// Create undo transaction
		FScopedTransaction Transaction(FText::FromString(TEXT("Snap to Ground (No Rotation)")));

		int32 NumModified = 0;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (!Actor)
			{
				continue;
			}

			FVector ActorLocation = Actor->GetActorLocation();

			// Get mesh component's LOCAL bounds (not affected by animation bloat)
			float MeshBottomOffset = 0.0f;

			if (USkeletalMeshComponent* SkelMesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
			{
				FBoxSphereBounds LocalBounds = SkelMesh->CalcLocalBounds();
				FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
				FVector WorldBottom = SkelMesh->GetComponentTransform().TransformPosition(LocalBottom);
				MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
			}
			else if (UStaticMeshComponent* StaticMesh = Actor->FindComponentByClass<UStaticMeshComponent>())
			{
				FBoxSphereBounds LocalBounds = StaticMesh->CalcLocalBounds();
				FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
				FVector WorldBottom = StaticMesh->GetComponentTransform().TransformPosition(LocalBottom);
				MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
			}
			else
			{
				// Find first primitive component with blocking collision (skip query-only spheres, triggers, etc.)
				UPrimitiveComponent* BestComp = nullptr;
				TArray<UPrimitiveComponent*> PrimComps;
				Actor->GetComponents<UPrimitiveComponent>(PrimComps);
				for (UPrimitiveComponent* Comp : PrimComps)
				{
					ECollisionEnabled::Type ColType = Comp->GetCollisionEnabled();
					if (ColType == ECollisionEnabled::QueryAndPhysics || ColType == ECollisionEnabled::PhysicsOnly)
					{
						BestComp = Comp;
						break;
					}
				}

				if (BestComp)
				{
					FBoxSphereBounds LocalBounds = BestComp->CalcLocalBounds();
					FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
					FVector WorldBottom = BestComp->GetComponentTransform().TransformPosition(LocalBottom);
					MeshBottomOffset = ActorLocation.Z - WorldBottom.Z;
				}
				// else: no physics collision component, use 0 offset (root = ground)
			}

			// Line trace from actor position downward
			FVector TraceStart = ActorLocation + FVector(0, 0, 500.f);
			FVector TraceEnd = TraceStart - FVector(0, 0, 200000.f);

			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(Actor);

			// Recursively ignore all attached actors
			TArray<AActor*> AttachedActors;
			Actor->GetAttachedActors(AttachedActors, true, true);
			for (AActor* Attached : AttachedActors)
			{
				QueryParams.AddIgnoredActor(Attached);
			}

			FHitResult HitResult;
			bool bHit = false;

			// Use channel trace (ECC_Visibility) - respects collision responses,
			// so query-only/overlap components are automatically skipped
			for (int32 Attempt = 0; Attempt < 50; Attempt++)
			{
				bHit = World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
				if (!bHit)
				{
					break;
				}

				// Also skip components with query-only collision (blocks Visibility but no physics)
				UPrimitiveComponent* HitComp = HitResult.GetComponent();
				if (HitComp)
				{
					ECollisionEnabled::Type CollisionType = HitComp->GetCollisionEnabled();
					if (CollisionType == ECollisionEnabled::QueryAndPhysics || CollisionType == ECollisionEnabled::PhysicsOnly)
					{
						break; // Valid collidable surface
					}
				}

				// Skip this specific component and trace again
				if (HitComp)
				{
					QueryParams.AddIgnoredComponent(HitComp);
				}
				bHit = false;
			}

			if (bHit)
			{
				Actor->Modify();

				FVector NewLocation = ActorLocation;
				NewLocation.Z = HitResult.ImpactPoint.Z + MeshBottomOffset + 5.0f;

				Actor->SetActorLocation(NewLocation);
				// Reset rotation to 0,0,0 (world up, no rotation)
				Actor->SetActorRotation(FRotator::ZeroRotator);
				Actor->PostEditMove(true);

				NumModified++;
			}
		}

		if (NumModified > 0)
		{
			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}

		return false;
	}

	bool DuplicateInPlace()
	{
		if (!GEditor)
		{
			return false;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return false;
		}

		// Store original transforms before duplication
		TArray<FTransform> OriginalTransforms;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			if (AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i)))
			{
				OriginalTransforms.Add(Actor->GetActorTransform());
			}
		}

		if (OriginalTransforms.Num() == 0)
		{
			return false;
		}

		// Execute the standard duplicate command
		GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("DUPLICATE"));

		// The duplicated actors are now selected - move them back to original positions
		Selection = GEditor->GetSelectedActors();
		if (Selection && Selection->Num() == OriginalTransforms.Num())
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("Duplicate In Place")));

			for (int32 i = 0; i < Selection->Num(); i++)
			{
				if (AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i)))
				{
					Actor->Modify();
					Actor->SetActorTransform(OriginalTransforms[i]);
					Actor->PostEditMove(true);
				}
			}

			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
		}

		return true;
	}

	// Called when Ctrl+Shift+V is pressed - sets up deferred paste
	bool SetupPasteToFolder()
	{
		if (!GEditor)
		{
			return false;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return false;
		}

		// Get folder path from currently selected actor
		PendingPasteFolderPath = NAME_None;

		USelection* ActorSelection = GEditor->GetSelectedActors();
		if (ActorSelection)
		{
			for (int32 i = 0; i < ActorSelection->Num(); i++)
			{
				if (AActor* Actor = Cast<AActor>(ActorSelection->GetSelectedObject(i)))
				{
					PendingPasteFolderPath = Actor->GetFolderPath();
					break;
				}
			}
		}

		// Store all actors currently in the world
		ActorsBeforePaste.Empty();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			ActorsBeforePaste.Add(*It);
		}

		// Mark that we're waiting for paste to complete
		bPasteToFolderPending = true;

		// Return false to NOT consume the event - let normal Ctrl+V paste happen
		return false;
	}

	// Called on next tick after paste - moves actors to folder
	void CompletePasteToFolder()
	{
		if (!GEditor)
		{
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return;
		}

		// Find newly created actors
		TArray<AActor*> NewlyPastedActors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!ActorsBeforePaste.Contains(*It))
			{
				NewlyPastedActors.Add(*It);
			}
		}

		ActorsBeforePaste.Empty();

		// If no folder was specified or no new actors, we're done
		if (PendingPasteFolderPath.IsNone() || NewlyPastedActors.Num() == 0)
		{
			return;
		}

		// Move pasted actors to target folder
		FScopedTransaction Transaction(FText::FromString(TEXT("Paste to Folder")));

		for (AActor* Actor : NewlyPastedActors)
		{
			Actor->Modify();
			Actor->SetFolderPath(PendingPasteFolderPath);
		}

		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
	}
};

TSharedPtr<FTransformCopyPasteProcessor> FTransformCopyPasteProcessor::Instance;
FTransform FTransformCopyPasteProcessor::CopiedTransform;
bool FTransformCopyPasteProcessor::bHasCopiedTransform = false;
FName FTransformCopyPasteProcessor::PendingPasteFolderPath;
TSet<AActor*> FTransformCopyPasteProcessor::ActorsBeforePaste;
bool FTransformCopyPasteProcessor::bPasteToFolderPending = false;

// Namespace for module registration
namespace TransformCopyPaste
{
	void Register() { FTransformCopyPasteProcessor::Register(); }
	void Unregister() { FTransformCopyPasteProcessor::Unregister(); }
}
