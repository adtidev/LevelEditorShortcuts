#include "LevelEditorShortcutsModule.h"
#include "Framework/Application/SlateApplication.h"

// Forward declarations of registration functions
namespace TransformCopyPaste { void Register(); void Unregister(); }
namespace LevelEditorShortcuts { void Register(); void Unregister(); }

#define LOCTEXT_NAMESPACE "FLevelEditorShortcutsModule"

void FLevelEditorShortcutsModule::StartupModule()
{
	// Register input processors - module loads PostEngineInit so Slate is ready
	if (FSlateApplication::IsInitialized())
	{
		TransformCopyPaste::Register();
		LevelEditorShortcuts::Register();
	}
}

void FLevelEditorShortcutsModule::ShutdownModule()
{
	// Unregister input processors
	TransformCopyPaste::Unregister();
	LevelEditorShortcuts::Unregister();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLevelEditorShortcutsModule, LevelEditorShortcuts)
