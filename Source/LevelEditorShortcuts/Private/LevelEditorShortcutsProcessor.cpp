// LevelEditorShortcutsProcessor.cpp
// Editor-only input processor for level editor shortcuts:
// 1-2-3: Widget modes (Move, Rotate, Scale) - disabled in Landscape/Foliage modes
// Q+Drag: Move selected actor(s) horizontally (respects local/world space)
// E+Drag: Move selected actor(s) vertically (respects local/world space)
// R+Drag: Scale selected actor(s) uniformly (outward=up, inward=down)
// Q+Scroll: Rotate selected actor(s) around Z axis
// G tap: Toggle grid snapping on/off
// G+Scroll: Change grid snap size (when not in Landscape/Foliage modes)

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "LevelEditorActions.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "UnrealWidget.h"
#include "SceneView.h"
#include "Editor/GroupActor.h"

class FLevelEditorShortcutsProcessor : public IInputProcessor
{
public:
	static TSharedPtr<FLevelEditorShortcutsProcessor> Instance;

	static void Register()
	{
		if (!Instance.IsValid() && FSlateApplication::IsInitialized())
		{
			Instance = MakeShared<FLevelEditorShortcutsProcessor>();
			FSlateApplication::Get().RegisterInputPreProcessor(Instance);
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
		// Cache cursor for visibility control from key handlers
		CachedCursor = Cursor;

		// Don't process during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return;
		}

		// Q/E/R held = drag mode (no click required)
		if (bQKeyDown || bEKeyDown || bRKeyDown)
		{
			FVector2D CurrentMousePosition = SlateApp.GetCursorPos();
			FVector2D MouseDelta = CurrentMousePosition - LastMousePosition;

			// When cursor is hidden, warp it back to start position for infinite movement range
			if (bCursorHidden)
			{
				SlateApp.SetCursorPos(DragStartCursorPos);
				LastMousePosition = DragStartCursorPos;
			}
			else
			{
				// Always update last position to avoid accumulating delta over frames
				LastMousePosition = CurrentMousePosition;
			}

			// Skip if no movement
			if (MouseDelta.IsNearlyZero())
			{
				return;
			}

			if (bQKeyDown)
			{
				MoveSelectedActorsHorizontal(MouseDelta);
			}
			else if (bEKeyDown)
			{
				MoveSelectedActorsVertical(MouseDelta.Y);
			}
			else if (bRKeyDown)
			{
				ScaleSelectedActorsUniform(MouseDelta);
			}
		}
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		// Don't intercept input during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return false;
		}

		FKey Key = InKeyEvent.GetKey();

		// Check if in Landscape/Foliage mode - Q/E have different functions there
		bool bInLandscapeMode = GLevelEditorModeTools().IsModeActive(FBuiltinEditorModes::EM_Landscape);
		bool bInFoliageMode = GLevelEditorModeTools().IsModeActive(FBuiltinEditorModes::EM_Foliage);

		// Q and E - track state and consume to prevent default behavior
		// Only in Level Editor - let Blueprint editor use W/E/R for gizmos
		if (Key == EKeys::Q)
		{
			if (bInLandscapeMode || bInFoliageMode)
			{
				return false; // Let landscape/foliage processor handle it
			}
			if (!IsLevelEditorViewportFocused())
			{
				return false; // Let other editors handle Q (e.g., for gizmo switching)
			}
			if (!bQKeyDown) // First press
			{
				bQKeyDown = true;
				LastMousePosition = SlateApp.GetCursorPos(); // Capture start position
				DragStartCursorPos = LastMousePosition;
				SetCursorHidden(true);
			}
			if (!InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown() && !InKeyEvent.IsShiftDown())
			{
				return true; // Consume Q to prevent any default behavior
			}
		}
		if (Key == EKeys::E)
		{
			if (bInLandscapeMode || bInFoliageMode)
			{
				return false; // Let landscape/foliage processor handle it
			}
			if (!IsLevelEditorViewportFocused())
			{
				return false; // Let other editors handle E (e.g., for gizmo switching)
			}
			if (!bEKeyDown) // First press
			{
				bEKeyDown = true;
				LastMousePosition = SlateApp.GetCursorPos(); // Capture start position
				DragStartCursorPos = LastMousePosition;
				SetCursorHidden(true);
			}
			if (!InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown() && !InKeyEvent.IsShiftDown())
			{
				return true; // Consume E to prevent any default behavior
			}
		}
		if (Key == EKeys::G)
		{
			bGKeyDown = true;
			bGScrolledWhileDown = false;
		}

		// R+Drag: Uniform scale (Level Editor only)
		// In Blueprint editor and other editors, R is NOT consumed so default W/E/R bindings work
		if (Key == EKeys::R)
		{
			if (bInLandscapeMode || bInFoliageMode)
			{
				return false;
			}
			if (!IsLevelEditorViewportFocused())
			{
				return false;
			}
			if (!bRKeyDown) // First press
			{
				bRKeyDown = true;
				LastMousePosition = SlateApp.GetCursorPos();
				DragStartCursorPos = LastMousePosition;
				SetCursorHidden(true);
			}
			if (!InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown() && !InKeyEvent.IsShiftDown())
			{
				return true; // Consume R to prevent any default behavior
			}
		}

		// 1-2-3 for widget modes (Move, Rotate, Scale) - only without modifiers
		// Only works in Level Editor viewport - Blueprint editor doesn't expose mode tools safely
		if (!InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown() && !InKeyEvent.IsShiftDown())
		{
			// Don't intercept in Landscape/Foliage modes (they use 1-9 for tools)
			if (!bInLandscapeMode && !bInFoliageMode)
			{
				if (Key == EKeys::One)
				{
					if (SetWidgetModeOnActiveViewport(UE::Widget::WM_Translate))
					{
						return true;
					}
				}
				if (Key == EKeys::Two)
				{
					if (SetWidgetModeOnActiveViewport(UE::Widget::WM_Rotate))
					{
						return true;
					}
				}
				if (Key == EKeys::Three)
				{
					if (SetWidgetModeOnActiveViewport(UE::Widget::WM_Scale))
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		// Don't intercept input during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return false;
		}

		if (InKeyEvent.GetKey() == EKeys::Q)
		{
			// If we rotated with Q+scroll, restore the move gizmo
			if (bQScrolledWhileDown)
			{
				GLevelEditorModeTools().SetWidgetMode(UE::Widget::WM_Translate);
			}
			bQKeyDown = false;
			bQScrolledWhileDown = false;
			EndDragTransaction();
			SetCursorHidden(false);
			// Update gizmo to new actor position
			if (GEditor)
			{
				GEditor->NoteSelectionChange();
				GEditor->RedrawLevelEditingViewports();
			}
			return true;
		}
		if (InKeyEvent.GetKey() == EKeys::E)
		{
			bEKeyDown = false;
			EndDragTransaction();
			SetCursorHidden(false);
			// Update gizmo to new actor position
			if (GEditor)
			{
				GEditor->NoteSelectionChange();
				GEditor->RedrawLevelEditingViewports();
			}
			return true;
		}
		if (InKeyEvent.GetKey() == EKeys::R)
		{
			bRKeyDown = false;
			EndDragTransaction();
			SetCursorHidden(false);
			if (GEditor)
			{
				GEditor->NoteSelectionChange();
				GEditor->RedrawLevelEditingViewports();
			}
			return true;
		}
		if (InKeyEvent.GetKey() == EKeys::G)
		{
			// If G was released without scrolling, toggle grid snap
			if (!bGScrolledWhileDown)
			{
				ToggleGridSnap();
			}
			bGKeyDown = false;
			bGScrolledWhileDown = false;
		}
		return false; // Don't consume
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		// Don't intercept input during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return false;
		}

		// Shift+LMB in rotate mode: temporarily disable rotation snap
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && MouseEvent.IsShiftDown())
		{
			if (IsLevelEditorViewportFocused())
			{
				// Check if we're in rotate widget mode
				UE::Widget::EWidgetMode CurrentMode = GLevelEditorModeTools().GetWidgetMode();
				if (CurrentMode == UE::Widget::WM_Rotate)
				{
					ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
					if (ViewportSettings && ViewportSettings->RotGridEnabled)
					{
						ViewportSettings->RotGridEnabled = false;
						bTemporarilyDisabledRotSnap = true;
					}
				}
			}
		}

		return false;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		// Restore rotation snap if we temporarily disabled it
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && bTemporarilyDisabledRotSnap)
		{
			ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
			if (ViewportSettings)
			{
				ViewportSettings->RotGridEnabled = true;
			}
			bTemporarilyDisabledRotSnap = false;
		}

		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		return false;
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
	{
		// Don't intercept input during Play In Editor
		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			return false;
		}

		float ScrollDelta = InWheelEvent.GetWheelDelta();

		// Q+Scroll: Rotate selected actors (Shift bypasses rotation snap)
		if (bQKeyDown)
		{
			bQScrolledWhileDown = true;
			RotateSelectedActors(ScrollDelta, InWheelEvent.IsShiftDown());
			return true; // Consume
		}

		// G+Scroll: Change grid size (only when not in Landscape/Foliage modes)
		if (bGKeyDown)
		{
			// Check if we're in Landscape or Foliage mode - those modes use G+Scroll for brush size
			bool bInLandscapeMode = GLevelEditorModeTools().IsModeActive(FBuiltinEditorModes::EM_Landscape);
			bool bInFoliageMode = GLevelEditorModeTools().IsModeActive(FBuiltinEditorModes::EM_Foliage);

			if (!bInLandscapeMode && !bInFoliageMode)
			{
				bGScrolledWhileDown = true;
				ChangeGridSize(ScrollDelta > 0);
				return true; // Consume
			}
			// If in Landscape/Foliage mode, let the other processor handle it
			bGScrolledWhileDown = true; // Still mark as scrolled so we don't toggle on release
		}

		return false;
	}

private:
	bool bQKeyDown = false;
	bool bQScrolledWhileDown = false;
	bool bEKeyDown = false;
	bool bRKeyDown = false;
	bool bGKeyDown = false;
	bool bGScrolledWhileDown = false;
	FVector2D LastMousePosition = FVector2D::ZeroVector;

	// For Shift+Rotate to temporarily disable rotation snap
	bool bTemporarilyDisabledRotSnap = false;

	// For Q-hold cursor hiding and infinite movement
	bool bCursorHidden = false;
	FVector2D DragStartCursorPos = FVector2D::ZeroVector;
	TSharedPtr<ICursor> CachedCursor;

	// For precise cursor tracking - stores the initial offset from cursor to selection pivot
	FVector DragStartWorldPos = FVector::ZeroVector;
	FVector SelectionStartPivot = FVector::ZeroVector;
	bool bDragInitialized = false;

	// For snap accumulation - tracks unsnapped movement
	FVector AccumulatedMovement = FVector::ZeroVector;

	// For R+Drag uniform scale - stores initial scales at drag start and total accumulated delta
	float TotalScaleDelta = 0.0f;
	TArray<TPair<TWeakObjectPtr<AActor>, FVector>> ScaleDragInitialScales;

	// Transaction for continuous drag operations (single undo for entire drag)
	TUniquePtr<FScopedTransaction> DragTransaction;

	void EndDragTransaction()
	{
		if (DragTransaction.IsValid())
		{
			DragTransaction.Reset();
		}
		bDragInitialized = false;
		AccumulatedMovement = FVector::ZeroVector;
		TotalScaleDelta = 0.0f;
		ScaleDragInitialScales.Empty();
	}

	void SetCursorHidden(bool bHide)
	{
		if (bHide == bCursorHidden)
		{
			return;
		}
		bCursorHidden = bHide;
		if (CachedCursor.IsValid())
		{
			CachedCursor->Show(!bHide);
		}
	}

	void EnsureDragTransaction(const FText& Description)
	{
		if (!DragTransaction.IsValid())
		{
			DragTransaction = MakeUnique<FScopedTransaction>(Description);
		}
	}

	FLevelEditorViewportClient* GetActiveViewportClient()
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> ActiveViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		if (ActiveViewport.IsValid())
		{
			return &ActiveViewport->GetLevelViewportClient();
		}
		return nullptr;
	}

	// Check if Level Editor viewport is focused
	bool IsLevelEditorViewportFocused()
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> ActiveViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		if (ActiveViewport.IsValid())
		{
			return ActiveViewport->HasKeyboardFocus() || ActiveViewport->HasFocusedDescendants();
		}
		return false;
	}

	// Set widget mode on the currently active editor viewport
	bool SetWidgetModeOnActiveViewport(UE::Widget::EWidgetMode Mode)
	{
		// First, check if Level Editor viewport is focused - use GLevelEditorModeTools
		if (IsLevelEditorViewportFocused())
		{
			GLevelEditorModeTools().SetWidgetMode(Mode);
			return true;
		}

		// Try GEditor->GetActiveViewport() for other editor viewports (Blueprint, Static Mesh, etc.)
		FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
		if (ActiveViewport)
		{
			FViewportClient* Client = ActiveViewport->GetClient();
			if (Client)
			{
				FEditorViewportClient* EditorClient = static_cast<FEditorViewportClient*>(Client);
				if (EditorClient)
				{
					FEditorModeTools* ModeTools = EditorClient->GetModeTools();
					if (ModeTools)
					{
						ModeTools->SetWidgetMode(Mode);
						return true;
					}
				}
			}
		}

		return false;
	}

	// Returns the grid snap size if snapping is enabled, 0 otherwise
	float GetGridSnapSize()
	{
		ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
		if (!ViewportSettings || !ViewportSettings->GridEnabled)
		{
			return 0.0f;
		}

		const TArray<float>& GridSizes = ViewportSettings->bUsePowerOf2SnapSize
			? ViewportSettings->Pow2GridSizes
			: ViewportSettings->DecimalGridSizes;

		int32 CurrentIndex = ViewportSettings->CurrentPosGridSize;
		if (GridSizes.IsValidIndex(CurrentIndex))
		{
			return GridSizes[CurrentIndex];
		}
		return 0.0f;
	}

	// Get selection pivot (center of selected actors)
	FVector GetSelectionPivot()
	{
		if (!GEditor)
		{
			return FVector::ZeroVector;
		}

		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection || Selection->Num() == 0)
		{
			return FVector::ZeroVector;
		}

		FVector Sum = FVector::ZeroVector;
		int32 Count = 0;
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (Actor)
			{
				Sum += Actor->GetActorLocation();
				Count++;
			}
		}
		return Count > 0 ? Sum / Count : FVector::ZeroVector;
	}

	// Project screen position to world position on a horizontal plane at given Z
	bool ScreenToWorldOnPlane(FLevelEditorViewportClient* ViewportClient, const FVector2D& ScreenPos, float PlaneZ, FVector& OutWorldPos)
	{
		if (!ViewportClient)
		{
			return false;
		}

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			ViewportClient->Viewport,
			ViewportClient->GetScene(),
			ViewportClient->EngineShowFlags));

		FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
		if (!View)
		{
			return false;
		}

		// Deproject screen to world ray
		FVector WorldOrigin, WorldDirection;
		FMatrix InvViewProjMatrix = View->ViewMatrices.GetInvViewProjectionMatrix();

		// Convert screen pos to normalized device coordinates
		FIntRect ViewRect = View->UnscaledViewRect;
		float NormX = (ScreenPos.X - ViewRect.Min.X) / (float)ViewRect.Width() * 2.0f - 1.0f;
		float NormY = 1.0f - (ScreenPos.Y - ViewRect.Min.Y) / (float)ViewRect.Height() * 2.0f;

		FVector4 Near = InvViewProjMatrix.TransformFVector4(FVector4(NormX, NormY, 0.0f, 1.0f));
		FVector4 Far = InvViewProjMatrix.TransformFVector4(FVector4(NormX, NormY, 1.0f, 1.0f));

		WorldOrigin = FVector(Near) / Near.W;
		FVector WorldEnd = FVector(Far) / Far.W;
		WorldDirection = (WorldEnd - WorldOrigin).GetSafeNormal();

		// Intersect ray with horizontal plane at PlaneZ
		if (FMath::Abs(WorldDirection.Z) < KINDA_SMALL_NUMBER)
		{
			return false; // Ray is parallel to plane
		}

		float T = (PlaneZ - WorldOrigin.Z) / WorldDirection.Z;
		if (T < 0)
		{
			return false; // Intersection is behind camera
		}

		OutWorldPos = WorldOrigin + WorldDirection * T;
		return true;
	}

	void MoveSelectedActorsHorizontal(const FVector2D& MouseDelta)
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

		FLevelEditorViewportClient* ViewportClient = GetActiveViewportClient();
		if (!ViewportClient)
		{
			return;
		}

		// Initialize transaction on first movement
		if (!bDragInitialized)
		{
			bDragInitialized = true;
			EnsureDragTransaction(FText::FromString(TEXT("Move Horizontal")));
		}

		// Determine movement plane based on local/world coordinate system
		ECoordSystem CoordSystem = GLevelEditorModeTools().GetCoordSystem();
		FVector PlaneNormal = FVector::UpVector;

		if (CoordSystem == COORD_Local)
		{
			for (int32 i = 0; i < Selection->Num(); i++)
			{
				AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
				if (Actor)
				{
					PlaneNormal = Actor->GetActorRotation().Quaternion().GetUpVector();
					break;
				}
			}
		}

		// Get camera vectors and project onto movement plane
		FRotator CameraRotation = ViewportClient->GetViewRotation();
		FVector CameraForward = CameraRotation.Vector();
		FVector CameraRight = FRotationMatrix(CameraRotation).GetScaledAxis(EAxis::Y);

		CameraForward = FVector::VectorPlaneProject(CameraForward, PlaneNormal);
		CameraRight = FVector::VectorPlaneProject(CameraRight, PlaneNormal);
		CameraForward.Normalize();
		CameraRight.Normalize();

		// Calculate world units per pixel based on camera distance and FOV
		// This makes movement feel 1:1 with the cursor
		float Distance = (ViewportClient->GetViewLocation() - GetSelectionPivot()).Size();
		if (Distance < 100.0f) Distance = 1000.0f;

		// FOV-based scaling for accurate cursor tracking
		float FOV = ViewportClient->ViewFOV;
		float ViewportHeight = ViewportClient->Viewport->GetSizeXY().Y;
		float WorldUnitsPerPixel = (2.0f * Distance * FMath::Tan(FMath::DegreesToRadians(FOV * 0.5f))) / ViewportHeight;

		// Tilt correction based on angle between camera and movement plane.
		// Generalizes the old cos(pitch) for world XY to any oriented plane.
		float DotToNormal = FMath::Abs(FVector::DotProduct(CameraRotation.Vector(), PlaneNormal));
		float TiltCorrection = FMath::Sqrt(1.0f - DotToNormal * DotToNormal);
		if (TiltCorrection < 0.1f) TiltCorrection = 0.1f;
		WorldUnitsPerPixel *= TiltCorrection;

		// Additional correction factor to match cursor feel
		WorldUnitsPerPixel *= 0.4f;

		// Convert mouse delta to world movement on the plane
		FVector WorldDelta = (CameraRight * MouseDelta.X + CameraForward * -MouseDelta.Y) * WorldUnitsPerPixel;

		// Accumulate movement
		AccumulatedMovement += WorldDelta;

		// Check grid snapping
		float SnapSize = GetGridSnapSize();
		FVector ActualDelta = FVector::ZeroVector;

		if (SnapSize > 0.0f)
		{
			// Only move in snap increments
			if (FMath::Abs(AccumulatedMovement.X) >= SnapSize)
			{
				float SnappedX = FMath::GridSnap(AccumulatedMovement.X, SnapSize);
				ActualDelta.X = SnappedX;
				AccumulatedMovement.X -= SnappedX;
			}
			if (FMath::Abs(AccumulatedMovement.Y) >= SnapSize)
			{
				float SnappedY = FMath::GridSnap(AccumulatedMovement.Y, SnapSize);
				ActualDelta.Y = SnappedY;
				AccumulatedMovement.Y -= SnappedY;
			}
			if (FMath::Abs(AccumulatedMovement.Z) >= SnapSize)
			{
				float SnappedZ = FMath::GridSnap(AccumulatedMovement.Z, SnapSize);
				ActualDelta.Z = SnappedZ;
				AccumulatedMovement.Z -= SnappedZ;
			}

			if (ActualDelta.IsNearlyZero())
			{
				return; // Haven't accumulated enough movement for a snap
			}
		}
		else
		{
			// No snapping - use full accumulated movement
			ActualDelta = AccumulatedMovement;
			AccumulatedMovement = FVector::ZeroVector;
		}

		// Apply movement to all selected actors
		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (Actor)
			{
				Actor->Modify();
				FVector NewLocation = Actor->GetActorLocation() + ActualDelta;
				Actor->SetActorLocation(NewLocation);
				Actor->PostEditMove(false);
			}
		}

		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
	}

	void MoveSelectedActorsVertical(float MouseDeltaY)
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

		FLevelEditorViewportClient* ViewportClient = GetActiveViewportClient();
		if (!ViewportClient)
		{
			return;
		}

		// Initialize transaction on first movement
		if (!bDragInitialized)
		{
			bDragInitialized = true;
			EnsureDragTransaction(FText::FromString(TEXT("Move Vertical")));
		}

		// Determine vertical axis based on local/world coordinate system
		ECoordSystem CoordSystem = GLevelEditorModeTools().GetCoordSystem();
		FVector VerticalAxis = FVector::UpVector;

		if (CoordSystem == COORD_Local)
		{
			for (int32 i = 0; i < Selection->Num(); i++)
			{
				AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
				if (Actor)
				{
					VerticalAxis = Actor->GetActorRotation().Quaternion().GetUpVector();
					break;
				}
			}
		}

		// Use same FOV-based calculation as horizontal movement for consistent feel at distance
		float Distance = (ViewportClient->GetViewLocation() - GetSelectionPivot()).Size();
		if (Distance < 100.0f) Distance = 100.0f;

		float FOV = ViewportClient->ViewFOV;
		float ViewportHeight = ViewportClient->Viewport->GetSizeXY().Y;
		float WorldUnitsPerPixel = (2.0f * Distance * FMath::Tan(FMath::DegreesToRadians(FOV * 0.5f))) / ViewportHeight;
		WorldUnitsPerPixel *= 0.4f; // Match horizontal's base multiplier

		// Reduce sensitivity when close for finer control
		float CloseDistanceThreshold = 2000.0f;
		float MinSensitivityMultiplier = 0.3f;
		float SensitivityMultiplier = FMath::GetMappedRangeValueClamped(
			FVector2D(100.0f, CloseDistanceThreshold),
			FVector2D(MinSensitivityMultiplier, 1.0f),
			Distance);

		float Scale = WorldUnitsPerPixel * SensitivityMultiplier;

		// Mouse Y up = actor moves along vertical axis (negative delta = up in screen space)
		FVector Delta = VerticalAxis * (-MouseDeltaY * Scale);

		// Accumulate movement
		AccumulatedMovement += Delta;

		// Check grid snapping
		float SnapSize = GetGridSnapSize();
		FVector ActualDelta = FVector::ZeroVector;

		if (SnapSize > 0.0f)
		{
			// Snap on each world axis independently
			if (FMath::Abs(AccumulatedMovement.X) >= SnapSize)
			{
				float Snapped = FMath::GridSnap(AccumulatedMovement.X, SnapSize);
				ActualDelta.X = Snapped;
				AccumulatedMovement.X -= Snapped;
			}
			if (FMath::Abs(AccumulatedMovement.Y) >= SnapSize)
			{
				float Snapped = FMath::GridSnap(AccumulatedMovement.Y, SnapSize);
				ActualDelta.Y = Snapped;
				AccumulatedMovement.Y -= Snapped;
			}
			if (FMath::Abs(AccumulatedMovement.Z) >= SnapSize)
			{
				float Snapped = FMath::GridSnap(AccumulatedMovement.Z, SnapSize);
				ActualDelta.Z = Snapped;
				AccumulatedMovement.Z -= Snapped;
			}

			if (ActualDelta.IsNearlyZero())
			{
				return; // Haven't accumulated enough movement for a snap
			}
		}
		else
		{
			// No snapping - use full accumulated movement
			ActualDelta = AccumulatedMovement;
			AccumulatedMovement = FVector::ZeroVector;
		}

		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (Actor)
			{
				Actor->Modify();
				FVector NewLocation = Actor->GetActorLocation() + ActualDelta;
				Actor->SetActorLocation(NewLocation);
				Actor->PostEditMove(false);
			}
		}

		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
	}

	void ScaleSelectedActorsUniform(const FVector2D& MouseDelta)
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

		// Initialize transaction and capture initial scales on first movement
		if (!bDragInitialized)
		{
			bDragInitialized = true;
			EnsureDragTransaction(FText::FromString(TEXT("Scale Uniform")));

			ScaleDragInitialScales.Empty();
			for (int32 i = 0; i < Selection->Num(); i++)
			{
				AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
				if (Actor)
				{
					ScaleDragInitialScales.Add(TPair<TWeakObjectPtr<AActor>, FVector>(Actor, Actor->GetActorScale3D()));
				}
			}
		}

		// Outward = right or up increases scale, left or down decreases
		float RadialDelta = MouseDelta.X - MouseDelta.Y;

		// Sensitivity: ~250px of drag to double the object
		float Sensitivity = 0.004f;
		TotalScaleDelta += RadialDelta * Sensitivity;

		// Scale multiplier relative to initial scale (1.0 = no change)
		float ScaleMultiplier = FMath::Max(1.0f + TotalScaleDelta, 0.01f);

		// Snap the multiplier itself so all axes change at the same time
		ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
		bool bSnapScale = ViewportSettings && ViewportSettings->SnapScaleEnabled;
		if (bSnapScale)
		{
			float ScaleGridSize = GEditor->GetScaleGridSize();
			if (ScaleGridSize > 0.0f)
			{
				ScaleMultiplier = FMath::GridSnap(ScaleMultiplier, ScaleGridSize);
				if (ScaleMultiplier < ScaleGridSize) ScaleMultiplier = ScaleGridSize;
			}
		}

		for (auto& Pair : ScaleDragInitialScales)
		{
			AActor* Actor = Pair.Key.Get();
			if (!Actor)
			{
				continue;
			}

			Actor->Modify();
			FVector NewScale = Pair.Value * ScaleMultiplier;
			NewScale = NewScale.ComponentMax(FVector(0.001f));
			Actor->SetActorScale3D(NewScale);
			Actor->PostEditMove(false);
		}

		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
	}

	void RotateSelectedActors(float ScrollDelta, bool bIgnoreSnap = false)
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

		// Rotation increment per scroll tick (in degrees)
		float RotationIncrement = 15.0f;
		float RotationAmount = (ScrollDelta > 0) ? RotationIncrement : -RotationIncrement;

		// Check if rotation grid snap is enabled - if so, use that instead
		// (unless Shift is held to bypass snapping)
		ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
		if (!bIgnoreSnap && ViewportSettings && ViewportSettings->RotGridEnabled)
		{
			// Get current rotation grid size
			int32 RotGridIndex = ViewportSettings->CurrentRotGridSize;
			const TArray<float>& RotGridSizes = (ViewportSettings->CurrentRotGridMode == GridMode_DivisionsOf360)
				? ViewportSettings->DivisionsOf360RotGridSizes
				: ViewportSettings->CommonRotGridSizes;

			if (RotGridSizes.IsValidIndex(RotGridIndex))
			{
				RotationAmount = (ScrollDelta > 0) ? RotGridSizes[RotGridIndex] : -RotGridSizes[RotGridIndex];
			}
		}

		// Collect actors to rotate and check for groups
		TArray<AActor*> ActorsToRotate;
		AGroupActor* GroupActor = nullptr;

		for (int32 i = 0; i < Selection->Num(); i++)
		{
			AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
			if (Actor)
			{
				// Check if this actor is part of a group (GetRootForActor is exported, GetParentForActor is not)
				if (!GroupActor)
				{
					GroupActor = AGroupActor::GetRootForActor(Actor);
				}
				ActorsToRotate.Add(Actor);
			}
		}

		if (ActorsToRotate.Num() == 0)
		{
			return;
		}

		// Create undo transaction
		FScopedTransaction Transaction(FText::FromString(TEXT("Rotate Selected")));

		// Determine pivot point for rotation
		// If grouped or multiple selection, rotate around the center
		// If single actor, rotate around its own pivot
		FVector RotationPivot = FVector::ZeroVector;
		bool bRotateAroundPivot = (ActorsToRotate.Num() > 1) || (GroupActor != nullptr);

		if (bRotateAroundPivot)
		{
			// Calculate center of all actors
			for (AActor* Actor : ActorsToRotate)
			{
				RotationPivot += Actor->GetActorLocation();
			}
			RotationPivot /= ActorsToRotate.Num();
		}

		// Create rotation transform around Z axis
		FQuat RotationQuat = FQuat(FVector::UpVector, FMath::DegreesToRadians(RotationAmount));

		for (AActor* Actor : ActorsToRotate)
		{
			Actor->Modify();

			if (bRotateAroundPivot)
			{
				// Rotate position around the pivot point
				FVector RelativePos = Actor->GetActorLocation() - RotationPivot;
				FVector NewRelativePos = RotationQuat.RotateVector(RelativePos);
				Actor->SetActorLocation(RotationPivot + NewRelativePos);
			}

			// Also rotate the actor's own yaw
			FRotator CurrentRotation = Actor->GetActorRotation();
			CurrentRotation.Yaw += RotationAmount;
			Actor->SetActorRotation(CurrentRotation);

			Actor->PostEditMove(true);
		}

		GEditor->NoteSelectionChange();
		GEditor->RedrawLevelEditingViewports();
	}

	void ToggleGridSnap()
	{
		// Use the built-in toggle which handles all the proper notifications
		FLevelEditorActionCallbacks::LocationGridSnap_Clicked();

		// Also redraw viewports to update the grid visualization
		if (GEditor)
		{
			GEditor->RedrawLevelEditingViewports();
		}
	}

	void ChangeGridSize(bool bIncrement)
	{
		ULevelEditorViewportSettings* ViewportSettings = GetMutableDefault<ULevelEditorViewportSettings>();
		if (!ViewportSettings)
		{
			return;
		}

		// Get the appropriate grid sizes array
		const TArray<float>& GridSizes = ViewportSettings->bUsePowerOf2SnapSize
			? ViewportSettings->Pow2GridSizes
			: ViewportSettings->DecimalGridSizes;

		if (GridSizes.Num() == 0)
		{
			return;
		}

		int32 CurrentIndex = ViewportSettings->CurrentPosGridSize;
		int32 NewIndex = CurrentIndex;

		if (bIncrement)
		{
			NewIndex = FMath::Min(CurrentIndex + 1, GridSizes.Num() - 1);
		}
		else
		{
			NewIndex = FMath::Max(CurrentIndex - 1, 0);
		}

		if (NewIndex != CurrentIndex)
		{
			// Use the built-in function to set grid size (handles notifications)
			GEditor->SetGridSize(NewIndex);
			GEditor->RedrawLevelEditingViewports();
		}
	}
};

TSharedPtr<FLevelEditorShortcutsProcessor> FLevelEditorShortcutsProcessor::Instance;

// Namespace for module registration
namespace LevelEditorShortcuts
{
	void Register() { FLevelEditorShortcutsProcessor::Register(); }
	void Unregister() { FLevelEditorShortcutsProcessor::Unregister(); }
}
