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
#include "Editor/GroupActor.h"

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

		// Ctrl+C or Ctrl+X - Copy transform (don't consume, let normal copy/cut happen too)
		if (InKeyEvent.GetKey() == EKeys::C || InKeyEvent.GetKey() == EKeys::X)
		{
			CopySelectedTransform();
			return false; // Don't consume - allow normal Ctrl+C/X to proceed
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
	// Ctrl+C / Ctrl+X: remember the selection's transform.
	//
	// For a single actor this is just its transform. For a group / multi-selection it's the
	// CENTROID of the members (with the first member's rotation), which is the same measure
	// PasteTransformToSelected() moves rigidly - so copy a group, paste onto a group, and the two
	// centroids line up. Reading Selection index 0 instead would pick either the passive
	// AGroupActor container or an arbitrary member, depending on click order.
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

		TArray<AActor*> Actors;
		GetActorsToTransform(Selection, Actors);
		if (Actors.Num() == 0)
		{
			return;
		}

		if (Actors.Num() == 1)
		{
			CopiedTransform = Actors[0]->GetActorTransform();
		}
		else
		{
			FVector Centroid = FVector::ZeroVector;
			for (AActor* Actor : Actors)
			{
				Centroid += Actor->GetActorLocation();
			}
			Centroid /= (double)Actors.Num();

			CopiedTransform = Actors[0]->GetActorTransform();
			CopiedTransform.SetLocation(Centroid);
		}

		bHasCopiedTransform = true;
	}

	// Ctrl+T: paste the copied transform onto the selection.
	//
	// This used to loop over every selected actor and slam each one to the IDENTICAL copied
	// location and rotation. Select a group and every member collapses onto a single point wearing
	// one shared rotation - the formation is gone. Same bug the B-snaps and the N camera-snap were
	// already fixed for; Ctrl+T was missed.
	//
	// Now: a single actor takes the copied location AND rotation (unchanged everyday behaviour).
	// A group / multi-selection moves RIGIDLY - the selection's centroid lands on the copied
	// location and every member keeps its relative offset and its own rotation.
	//
	// GetActorsToTransform() drops the passive AGroupActor container and any actor whose selected
	// ancestor already carries it via attachment, so nothing is moved twice. RefreshEditorPivot()
	// re-centres a locked group's root and the gizmo afterwards.
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

		TArray<AActor*> Actors;
		GetActorsToTransform(Selection, Actors);
		if (Actors.Num() == 0)
		{
			return false;
		}

		// Create undo transaction
		FScopedTransaction Transaction(FText::FromString(TEXT("Paste Transform")));

		if (Actors.Num() == 1)
		{
			AActor* Actor = Actors[0];
			Actor->Modify();
			Actor->SetActorLocation(CopiedTransform.GetLocation());
			Actor->SetActorRotation(CopiedTransform.GetRotation().Rotator());
			// Keep original scale - don't apply CopiedTransform scale
			Actor->PostEditMove(true);
		}
		else
		{
			FVector Centroid = FVector::ZeroVector;
			for (AActor* Actor : Actors)
			{
				Centroid += Actor->GetActorLocation();
			}
			Centroid /= (double)Actors.Num();

			const FVector Delta = CopiedTransform.GetLocation() - Centroid;
			for (AActor* Actor : Actors)
			{
				Actor->Modify();
				Actor->SetActorLocation(Actor->GetActorLocation() + Delta);
				// Rotations left alone: applying one shared rotation to every member is what
				// shredded the formation.
				Actor->PostEditMove(true);
			}
		}

		RefreshEditorPivot(Selection);
		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
		return true;
	}

	// ---- Group-safe ground snapping -------------------------------------------------
	//
	// Ctrl+B (snap to floor, inherits slope) and Shift+B (snap to floor, upright) used to loop over
	// every selected actor and drop each one independently onto whatever happened to be under it,
	// then overwrite its rotation. On a group that shreds the formation: members land at different
	// heights and each takes its own surface tilt.
	//
	// Now a single actor behaves exactly as before, and a group / multi-selection moves RIGIDLY:
	// one shared Z delta for everyone, rotations left alone. The shared delta is the LARGEST lift
	// any member needs, so nothing ends up buried in the terrain.

	// Distance in world Z from the actor's origin down to the bottom of its mesh/collision.
	// Uses LOCAL bounds so animation bloat doesn't inflate it.
	float GetMeshBottomOffset(AActor* Actor) const
	{
		const FVector ActorLocation = Actor->GetActorLocation();

		auto BottomOffsetFor = [&ActorLocation](UPrimitiveComponent* Comp) -> float
		{
			FBoxSphereBounds LocalBounds = Comp->CalcLocalBounds();
			FVector LocalBottom = FVector(0, 0, LocalBounds.Origin.Z - LocalBounds.BoxExtent.Z);
			FVector WorldBottom = Comp->GetComponentTransform().TransformPosition(LocalBottom);
			return ActorLocation.Z - WorldBottom.Z;
		};

		if (USkeletalMeshComponent* SkelMesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
		{
			return BottomOffsetFor(SkelMesh);
		}
		if (UStaticMeshComponent* StaticMesh = Actor->FindComponentByClass<UStaticMeshComponent>())
		{
			return BottomOffsetFor(StaticMesh);
		}

		// Fall back to the first primitive with blocking collision (skip query-only spheres, triggers, etc.)
		TArray<UPrimitiveComponent*> PrimComps;
		Actor->GetComponents<UPrimitiveComponent>(PrimComps);
		for (UPrimitiveComponent* Comp : PrimComps)
		{
			ECollisionEnabled::Type ColType = Comp->GetCollisionEnabled();
			if (ColType == ECollisionEnabled::QueryAndPhysics || ColType == ECollisionEnabled::PhysicsOnly)
			{
				return BottomOffsetFor(Comp);
			}
		}

		// No physics collision component: treat the root as the ground contact point.
		return 0.0f;
	}

	// Every actor a snap is about to move, plus everything attached beneath them, plus every member
	// of any group they belong to. A ground trace MUST ignore all of it: a member resting on top of
	// another member of the same group traces onto its own mate, reports "already grounded", and
	// that near-zero delta then vetoes the whole group's fall (see the shared-delta comment below).
	void BuildIgnoreSet(const TArray<AActor*>& MovingActors, TSet<AActor*>& OutIgnore) const
	{
		for (AActor* Actor : MovingActors)
		{
			if (!Actor)
			{
				continue;
			}
			OutIgnore.Add(Actor);

			TArray<AActor*> Attached;
			Actor->GetAttachedActors(Attached, true, true);
			for (AActor* Child : Attached)
			{
				OutIgnore.Add(Child);
			}

			if (AGroupActor* Root = AGroupActor::GetRootForActor(Actor, /*bMustBeLocked*/ false))
			{
				TArray<AActor*> Members;
				Root->GetGroupActors(Members, /*bRecurse*/ true);
				for (AActor* Member : Members)
				{
					if (!Member)
					{
						continue;
					}
					OutIgnore.Add(Member);

					TArray<AActor*> MemberAttached;
					Member->GetAttachedActors(MemberAttached, true, true);
					for (AActor* Child : MemberAttached)
					{
						OutIgnore.Add(Child);
					}
				}
			}
		}
	}

	// Traces straight down from the actor and returns the Z delta needed to sit it on the surface.
	// IgnoreActors is everything moving with this snap - see BuildIgnoreSet().
	bool ComputeGroundDelta(UWorld* World, AActor* Actor, const TSet<AActor*>& IgnoreActors, float& OutDeltaZ, FHitResult& OutHit) const
	{
		const FVector ActorLocation = Actor->GetActorLocation();
		const float MeshBottomOffset = GetMeshBottomOffset(Actor);

		// Start just above the actor's own mesh bottom, not a flat 500cm overhead. A high start
		// lets the downward trace hit whatever hangs ABOVE the actor (a mate's underside, a bridge,
		// a roof) and read it as "ground", which produces a big bogus LIFT. 100cm of headroom is
		// enough to recover an actor buried up to a metre in the terrain.
		const FVector TraceStart(ActorLocation.X, ActorLocation.Y, ActorLocation.Z - MeshBottomOffset + 100.f);
		const FVector TraceEnd = TraceStart - FVector(0, 0, 200000.f);

		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(Actor);

		// Recursively ignore all attached actors
		TArray<AActor*> AttachedActors;
		Actor->GetAttachedActors(AttachedActors, true, true);
		for (AActor* Attached : AttachedActors)
		{
			QueryParams.AddIgnoredActor(Attached);
		}

		for (AActor* Ignored : IgnoreActors)
		{
			if (Ignored)
			{
				QueryParams.AddIgnoredActor(Ignored);
			}
		}

		bool bHit = false;

		// Use channel trace (ECC_Visibility) - respects collision responses,
		// so query-only/overlap components are automatically skipped
		for (int32 Attempt = 0; Attempt < 50; Attempt++)
		{
			bHit = World->LineTraceSingleByChannel(OutHit, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
			if (!bHit)
			{
				break;
			}

			// Also skip components with query-only collision (blocks Visibility but no physics)
			UPrimitiveComponent* HitComp = OutHit.GetComponent();
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

		if (!bHit)
		{
			return false;
		}

		const float DesiredZ = OutHit.ImpactPoint.Z + MeshBottomOffset + 5.0f;
		OutDeltaZ = DesiredZ - ActorLocation.Z;
		return true;
	}

	// Build the list of actors a snap should move directly. Drops the passive AGroupActor container
	// (it has no geometry; the engine recentres it from its members) and any actor whose attachment
	// ancestor is also selected - that ancestor already carries it, so moving both double-applies
	// and scatters the formation.
	void GetActorsToTransform(USelection* Selection, TArray<AActor*>& OutActors) const
	{
		TSet<AActor*> SelectedSet;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			if (AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i)))
			{
				SelectedSet.Add(Actor);
			}
		}

		for (AActor* Actor : SelectedSet)
		{
			if (Actor->IsA(AGroupActor::StaticClass()))
			{
				continue;
			}

			bool bAncestorSelected = false;
			for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
			{
				if (SelectedSet.Contains(Parent))
				{
					bAncestorSelected = true;
					break;
				}
			}

			if (!bAncestorSelected)
			{
				OutActors.Add(Actor);
			}
		}
	}

	// We move actors directly via SetActorLocation, which bypasses the native gizmo drag path that
	// normally keeps the pivot in sync. For a LOCKED group the pivot is forced to the group ROOT's
	// own location (UUnrealEdEngine::UpdatePivotLocationForSelection), and we never touch the root,
	// so it stays pinned where the members used to be. CenterGroupLocation() moves only the root,
	// back to the members' new bounds centre - exactly what the engine does after a native move.
	void RefreshEditorPivot(USelection* Selection)
	{
		TSet<AGroupActor*> LockedRoots;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			if (AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i)))
			{
				if (AGroupActor* Root = AGroupActor::GetRootForActor(Actor, /*bMustBeLocked*/ true))
				{
					LockedRoots.Add(Root);
				}
			}
		}
		for (AGroupActor* Root : LockedRoots)
		{
			Root->CenterGroupLocation();
		}

		if (GUnrealEd)
		{
			GUnrealEd->UpdatePivotLocationForSelection();
		}
	}

	// bInheritSurfaceRotation: true = Ctrl+B (tilt to the slope), false = Shift+B (force upright).
	// Rotation is only ever touched for a SINGLE selected actor - a rigid group move must not
	// re-rotate its members.
	bool SnapSelectedToGroundInternal(bool bInheritSurfaceRotation)
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

		TArray<AActor*> Actors;
		GetActorsToTransform(Selection, Actors);
		if (Actors.Num() == 0)
		{
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(
			bInheritSurfaceRotation ? TEXT("Snap to Ground") : TEXT("Snap to Ground (No Rotation)")));

		// ---- Single actor: original behaviour, rotation included --------------------
		if (Actors.Num() == 1)
		{
			AActor* Actor = Actors[0];

			float DeltaZ = 0.0f;
			FHitResult HitResult;
			TSet<AActor*> IgnoreActors;
			BuildIgnoreSet(Actors, IgnoreActors);
			if (!ComputeGroundDelta(World, Actor, IgnoreActors, DeltaZ, HitResult))
			{
				return false;
			}

			UE_LOG(LogTemp, Log, TEXT("[SnapDiag] single %s deltaZ=%.1f hit=%s"),
				*Actor->GetName(), DeltaZ,
				HitResult.GetActor() ? *HitResult.GetActor()->GetName() : TEXT("none"));

			Actor->Modify();
			Actor->SetActorLocation(Actor->GetActorLocation() + FVector(0, 0, DeltaZ));

			if (bInheritSurfaceRotation)
			{
				// Keep the actor's facing direction but tilt it to match the surface.
				const FVector NewUp = HitResult.ImpactNormal;
				FVector CurrentForward = Actor->GetActorRotation().Vector();
				CurrentForward.Z = 0;
				CurrentForward.Normalize();

				FVector NewRight = FVector::CrossProduct(NewUp, CurrentForward);
				NewRight.Normalize();
				FVector NewForward = FVector::CrossProduct(NewRight, NewUp);
				NewForward.Normalize();

				FMatrix RotationMatrix = FMatrix::Identity;
				RotationMatrix.SetAxes(&NewForward, &NewRight, &NewUp);
				Actor->SetActorRotation(RotationMatrix.Rotator());
			}
			else
			{
				Actor->SetActorRotation(FRotator::ZeroRotator);
			}

			Actor->PostEditMove(true);

			RefreshEditorPivot(Selection);
			GEditor->NoteSelectionChange();
			GEditor->RedrawLevelEditingViewports();
			return true;
		}

		// ---- Group / multi-selection: one shared lift, no rotation change -----------
		// Take the LARGEST required lift so the member sitting on the highest ground defines the
		// drop; anyone over a dip keeps its relative height instead of being buried.
		bool bAnyHit = false;
		float SharedDeltaZ = -FLT_MAX;
		AActor* DecidingActor = nullptr;
		AActor* DecidingHit = nullptr;

		TSet<AActor*> IgnoreActors;
		BuildIgnoreSet(Actors, IgnoreActors);

		for (AActor* Actor : Actors)
		{
			float DeltaZ = 0.0f;
			FHitResult HitResult;
			if (ComputeGroundDelta(World, Actor, IgnoreActors, DeltaZ, HitResult))
			{
				UE_LOG(LogTemp, Log, TEXT("[SnapDiag]   member %s deltaZ=%.1f hit=%s"),
					*Actor->GetName(), DeltaZ,
					HitResult.GetActor() ? *HitResult.GetActor()->GetName() : TEXT("none"));

				if (DeltaZ > SharedDeltaZ)
				{
					SharedDeltaZ = DeltaZ;
					DecidingActor = Actor;
					DecidingHit = HitResult.GetActor();
				}
				bAnyHit = true;
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("[SnapDiag]   member %s NO GROUND under it"), *Actor->GetName());
			}
		}

		if (!bAnyHit)
		{
			UE_LOG(LogTemp, Warning, TEXT("[SnapDiag] group of %d: no member found ground, nothing moved"), Actors.Num());
			return false;
		}

		// The shared lift is decided by ONE member - whichever needs the biggest lift (i.e. touches
		// down first on the way down). If a snap refuses to fall, this line names the member that
		// vetoed it and what it stood on.
		UE_LOG(LogTemp, Log, TEXT("[SnapDiag] group of %d: sharedDeltaZ=%.1f decided by %s standing on %s"),
			Actors.Num(), SharedDeltaZ,
			DecidingActor ? *DecidingActor->GetName() : TEXT("?"),
			DecidingHit ? *DecidingHit->GetName() : TEXT("?"));

		const FVector Offset(0, 0, SharedDeltaZ);
		for (AActor* Actor : Actors)
		{
			Actor->Modify();
			Actor->SetActorLocation(Actor->GetActorLocation() + Offset);
			Actor->PostEditMove(true);
		}

		RefreshEditorPivot(Selection);
		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
		return true;
	}

	bool SnapSelectedToGround()
	{
		return SnapSelectedToGroundInternal(/*bInheritSurfaceRotation*/ true);
	}

	// Snap to ground WITHOUT inheriting surface rotation (keeps world up)
	bool SnapSelectedToGroundNoRotation()
	{
		return SnapSelectedToGroundInternal(/*bInheritSurfaceRotation*/ false);
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
		// Use Min of counts: groups may change selection count after duplication
		Selection = GEditor->GetSelectedActors();
		if (Selection && Selection->Num() > 0)
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("Duplicate In Place")));

			const int32 Count = FMath::Min(Selection->Num(), OriginalTransforms.Num());
			for (int32 i = 0; i < Count; i++)
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
