// LockBrowserTab.cpp
// Dockable editor tab that lists Perforce-locked .uasset/.umap files in the active map,
// resolving each GUID file path back to its actor name by walking the loaded world.
// Designed for World Partition projects where map external-actor files are GUID-named.
//
// Shortcut: Shift+4 toggles the tab (handled by the existing input processor).
//
// Shared-checkout watcher: every WatchIntervalSeconds, polls `p4 opened -a` in the background and pops a
// toast when a file I have checked out is ALSO checked out by someone else (another user, or my own other
// workspace). Stock UE can't show this: the Perforce provider drops `otherOpen` once the file is mine.
// See Docs/Features/wp-revision-control.md.

#include "CoreMinimal.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ToolMenus.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformProperties.h"
#include "Internationalization/Text.h"
#include <atomic>
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"

#define LOCTEXT_NAMESPACE "LockBrowser"

DEFINE_LOG_CATEGORY_STATIC(LogLockBrowser, Log, All);

namespace LockBrowser
{
	static const FName TabId("LockBrowserTab");

	struct FLockEntry
	{
		FString DepotPath;       // //streamsDepot/.../GUID.uasset
		FString PackageName;     // /Game/__ExternalActors__/Maps/.../GUID
		FString Action;          // edit/add/delete
		FString User;
		FString Workspace;
		FString ActorName;       // resolved by walking the world (or "" if not loaded / not WP)
		FString ActorClass;
		bool bIsMine = false;
		FString AlsoOpenedBy;    // mine only: "user @ workspace, ..." of everyone else who has this file open
	};

	struct FP4Connection
	{
		FString Port;
		FString User;
		FString Client;
	};

	/** Read the Perforce connection Unreal's editor SCC is using, from Saved/Config/<Plat>/SourceControlSettings.ini.
	 *  Returns false if the active provider isn't Perforce. Any individual field may be empty (e.g. when UseP4Config=True). */
	static bool GetUnrealP4Connection(FP4Connection& Out)
	{
		const FString IniPath = FPaths::GeneratedConfigDir()
			+ ANSI_TO_TCHAR(FPlatformProperties::PlatformName())
			+ TEXT("/SourceControlSettings.ini");

		FString Provider;
		GConfig->GetString(TEXT("SourceControl.SourceControlSettings"), TEXT("Provider"), Provider, IniPath);
		if (!Provider.Equals(TEXT("Perforce"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		GConfig->GetString(TEXT("PerforceSourceControl.PerforceSourceControlSettings"), TEXT("Port"),      Out.Port,   IniPath);
		GConfig->GetString(TEXT("PerforceSourceControl.PerforceSourceControlSettings"), TEXT("UserName"),  Out.User,   IniPath);
		GConfig->GetString(TEXT("PerforceSourceControl.PerforceSourceControlSettings"), TEXT("Workspace"), Out.Client, IniPath);
		return true;
	}

	/** Builds leading `-p ... -u ... -c ...` flags for shelling out to p4, sourced from Unreal's SCC settings.
	 *  Empty fields are skipped, letting p4 fall back to its own resolution (env / P4CONFIG / registry). */
	static FString BuildP4ConnectionFlags(const FP4Connection& Conn)
	{
		FString Flags;
		if (!Conn.Port.IsEmpty())   { Flags += FString::Printf(TEXT("-p \"%s\" "), *Conn.Port); }
		if (!Conn.User.IsEmpty())   { Flags += FString::Printf(TEXT("-u \"%s\" "), *Conn.User); }
		if (!Conn.Client.IsEmpty()) { Flags += FString::Printf(TEXT("-c \"%s\" "), *Conn.Client); }
		return Flags;
	}

	/** Maps a Perforce depot path under .../Content/ to a UE package name like /Game/Foo/Bar.
	 *  Returns empty if the path isn't under any Content folder we recognise. */
	static FString DepotPathToPackageName(const FString& DepotPath)
	{
		// Look for "/Content/" anywhere in the path; everything after maps to /Game/.
		const FString ContentMarker = TEXT("/Content/");
		const int32 ContentIdx = DepotPath.Find(ContentMarker);
		if (ContentIdx == INDEX_NONE)
		{
			return FString();
		}
		FString Tail = DepotPath.Mid(ContentIdx + ContentMarker.Len());
		// Strip extension (.uasset / .umap).
		const int32 DotIdx = Tail.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (DotIdx != INDEX_NONE)
		{
			Tail = Tail.Left(DotIdx);
		}
		return TEXT("/Game/") + Tail;
	}

	/** Build a fast lookup from package name → Actor in the editor's current world. */
	static void BuildPackageToActorMap(TMap<FString, AActor*>& OutMap)
	{
		OutMap.Reset();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			UPackage* Pkg = Actor->GetExternalPackage();
			if (!Pkg)
			{
				Pkg = Actor->GetPackage();
			}
			if (Pkg)
			{
				OutMap.Add(Pkg->GetName(), Actor);
			}
		}
	}

	/** Run `p4 -F ... opened -a` and parse one entry per line. Synchronous; call from a worker thread. */
	static bool RunP4Opened(TArray<FLockEntry>& OutEntries, FString& OutError)
	{
		OutEntries.Reset();

		// Pull connection from Unreal's SCC settings so the CLI doesn't fall back to default `perforce:1666`
		// when a teammate's shell hasn't set P4PORT/P4USER/P4CLIENT.
		FP4Connection Conn;
		const bool bHaveSettings = GetUnrealP4Connection(Conn);
		const FString ConnFlags = bHaveSettings ? BuildP4ConnectionFlags(Conn) : FString();

		// Format pipe-separated so parsing is trivial.
		const FString Args = ConnFlags + TEXT("-F \"%depotFile%|%action%|%user%|%client%\" opened -a");

		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(TEXT("p4"), *Args, &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0)
		{
			OutError = StdErr.IsEmpty() ? FString::Printf(TEXT("p4 exited with code %d"), ReturnCode) : StdErr;
			return false;
		}

		// Determine the current p4 user so we can flag rows as "mine".
		// Prefer the value from UE's SCC settings (always trustworthy when bHaveSettings is true);
		// otherwise fall back to `p4 info` to honour env/P4CONFIG.
		// The workspace matters too: the same user in a second workspace (e.g. the laptop) is a real double checkout.
		FString MyUser = Conn.User;
		FString MyClient = Conn.Client;
		if (MyUser.IsEmpty() || MyClient.IsEmpty())
		{
			int32 RC2 = -1;
			FString Out2;
			FString Err2;
			const FString InfoArgs = ConnFlags + TEXT("-F \"%userName%|%clientName%\" -ztag info");
			FPlatformProcess::ExecProcess(TEXT("p4"), *InfoArgs, &RC2, &Out2, &Err2);
			if (RC2 == 0)
			{
				FString InfoLine = Out2.TrimStartAndEnd();
				// If multiple lines came back, take the first.
				int32 NL = INDEX_NONE;
				if (InfoLine.FindChar(TEXT('\n'), NL))
				{
					InfoLine = InfoLine.Left(NL).TrimStartAndEnd();
				}
				FString InfoUser, InfoClient;
				if (InfoLine.Split(TEXT("|"), &InfoUser, &InfoClient))
				{
					if (MyUser.IsEmpty())   { MyUser = InfoUser.TrimStartAndEnd(); }
					if (MyClient.IsEmpty()) { MyClient = InfoClient.TrimStartAndEnd(); }
				}
			}
		}

		TArray<FString> Lines;
		StdOut.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Cols;
			Line.ParseIntoArray(Cols, TEXT("|"), false);
			if (Cols.Num() < 4)
			{
				continue;
			}
			FLockEntry Entry;
			Entry.DepotPath = Cols[0].TrimStartAndEnd();
			Entry.Action    = Cols[1].TrimStartAndEnd();
			Entry.User      = Cols[2].TrimStartAndEnd();
			Entry.Workspace = Cols[3].TrimStartAndEnd();
			Entry.PackageName = DepotPathToPackageName(Entry.DepotPath);
			Entry.bIsMine = !MyUser.IsEmpty() && Entry.User.Equals(MyUser, ESearchCase::IgnoreCase)
				&& (MyClient.IsEmpty() || Entry.Workspace.Equals(MyClient, ESearchCase::IgnoreCase));
			OutEntries.Add(MoveTemp(Entry));
		}

		// Flag my files that someone else also has open.
		TMap<FString, FString> OthersByFile;
		for (const FLockEntry& E : OutEntries)
		{
			if (!E.bIsMine)
			{
				FString& Who = OthersByFile.FindOrAdd(E.DepotPath);
				Who += (Who.IsEmpty() ? TEXT("") : TEXT(", ")) + E.User + TEXT(" @ ") + E.Workspace;
			}
		}
		for (FLockEntry& E : OutEntries)
		{
			if (E.bIsMine)
			{
				if (const FString* Who = OthersByFile.Find(E.DepotPath))
				{
					E.AlsoOpenedBy = *Who;
				}
			}
		}
		return true;
	}

	/** Actor label + class for a package: the loaded editor world first, then the asset registry's
	 *  WP actor descriptor so actors in unloaded cells still get a readable name. */
	static void ResolveActor(const TMap<FString, AActor*>& PackageToActor, FLockEntry& E)
	{
		if (AActor* const* Found = PackageToActor.Find(E.PackageName))
		{
			if (AActor* Actor = *Found)
			{
				E.ActorName  = Actor->GetActorNameOrLabel();
				E.ActorClass = Actor->GetClass()->GetName();
				return;
			}
		}
		if (E.PackageName.IsEmpty() || !E.DepotPath.Contains(TEXT("__ExternalActors__")))
		{
			return;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(FName(*E.PackageName), Assets, /*bIncludeOnlyOnDiskAssets*/ true);
		for (const FAssetData& Asset : Assets)
		{
			if (TUniquePtr<FWorldPartitionActorDesc> Desc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(Asset))
			{
				E.ActorName  = Desc->GetActorLabelOrName().ToString();
				E.ActorClass = Desc->GetDisplayClassNameString();
				return;
			}
		}
	}

	/** Pretty "Label (Class)" or the bare GUID filename when unresolved. */
	static FString DisplayName(const FLockEntry& E)
	{
		return E.ActorName.IsEmpty()
			? FPaths::GetBaseFilename(E.DepotPath)
			: E.ActorName + (E.ActorClass.IsEmpty() ? FString() : FString::Printf(TEXT("  (%s)"), *E.ActorClass));
	}

	/** The Slate widget itself. */
	class SLockBrowser : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SLockBrowser) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ChildSlot
			[
				SNew(SVerticalBox)

				// Toolbar row: Refresh + filter + status text.
				+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Refresh", "Refresh"))
						.OnClicked_Lambda([this]() { Refresh(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 2.f, 0.f).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bShowOnlyOthers ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bShowOnlyOthers = (S == ECheckBoxState::Checked); RebuildVisible(); })
						[
							SNew(STextBlock).Text(LOCTEXT("HideMine", "Only others"))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 2.f, 0.f).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bShowOnlyShared ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bShowOnlyShared = (S == ECheckBoxState::Checked); RebuildVisible(); })
						.ToolTipText(LOCTEXT("OnlySharedTip", "Only files I have checked out that someone else also has checked out"))
						[
							SNew(STextBlock).Text(LOCTEXT("OnlyShared", "Only mine shared with others"))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 2.f, 0.f).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bExternalActorsOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { bExternalActorsOnly = (S == ECheckBoxState::Checked); RebuildVisible(); })
						[
							SNew(STextBlock).Text(LOCTEXT("ExternalActorsOnly", "External actors only"))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 2.f, 0.f).VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(260.f)
						[
							SNew(SSearchBox)
							.HintText(LOCTEXT("SearchHint", "Filter by actor / user / GUID…"))
							.OnTextChanged_Lambda([this](const FText& NewText)
							{
								SearchText = NewText.ToString();
								RebuildVisible();
							})
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(8.f, 0.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text_Lambda([this]() { return StatusText; })
					]
				]

				// Header row.
				+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
					.Padding(4.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(0.40f)[ SNew(STextBlock).Text(LOCTEXT("HdrActor", "Actor")) ]
						+ SHorizontalBox::Slot().FillWidth(0.20f)[ SNew(STextBlock).Text(LOCTEXT("HdrUser", "User")) ]
						+ SHorizontalBox::Slot().FillWidth(0.10f)[ SNew(STextBlock).Text(LOCTEXT("HdrAction", "Action")) ]
						+ SHorizontalBox::Slot().FillWidth(0.30f)[ SNew(STextBlock).Text(LOCTEXT("HdrFile", "File")) ]
					]
				]

				// Body list.
				+ SVerticalBox::Slot().FillHeight(1.f).Padding(4.f)
				[
					SAssignNew(ListView, SListView<TSharedPtr<FLockEntry>>)
					.ListItemsSource(&VisibleEntries)
					.OnGenerateRow(this, &SLockBrowser::OnGenerateRow)
					.SelectionMode(ESelectionMode::Single)
					.OnSelectionChanged(this, &SLockBrowser::OnSelectionChanged)
				]
			];

			StatusText = LOCTEXT("LoadingStatus", "Press Refresh to query Perforce.");
			Refresh();
		}

		void Refresh()
		{
			if (bRefreshing)
			{
				return;
			}
			bRefreshing = true;
			StatusText = LOCTEXT("RefreshingStatus", "Querying Perforce…");

			TWeakPtr<SLockBrowser> WeakSelf = SharedThis(this);
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakSelf]()
			{
				TArray<FLockEntry> Entries;
				FString Error;
				const bool bOk = RunP4Opened(Entries, Error);
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, Entries = MoveTemp(Entries), bOk, Error = MoveTemp(Error)]() mutable
				{
					if (TSharedPtr<SLockBrowser> Pinned = WeakSelf.Pin())
					{
						Pinned->OnP4Result(MoveTemp(Entries), bOk, Error);
					}
				});
			});
		}

	private:
		void OnP4Result(TArray<FLockEntry> Entries, bool bOk, const FString& Error)
		{
			bRefreshing = false;
			if (!bOk)
			{
				StatusText = FText::Format(LOCTEXT("ErrorStatus", "p4 failed: {0}"), FText::FromString(Error));
				AllEntries.Reset();
				RebuildVisible();
				return;
			}

			// Resolve actor names by walking the editor world once.
			TMap<FString, AActor*> PackageToActor;
			BuildPackageToActorMap(PackageToActor);

			AllEntries.Reset(Entries.Num());
			for (FLockEntry& E : Entries)
			{
				ResolveActor(PackageToActor, E);
				AllEntries.Add(MakeShared<FLockEntry>(MoveTemp(E)));
			}

			StatusText = FText::Format(
				LOCTEXT("ResultStatus", "{0} files open across all workspaces"),
				FText::AsNumber(AllEntries.Num()));
			RebuildVisible();
		}

		void RebuildVisible()
		{
			const FString Needle = SearchText.TrimStartAndEnd();
			const bool bHaveSearch = !Needle.IsEmpty();
			VisibleEntries.Reset(AllEntries.Num());
			for (const TSharedPtr<FLockEntry>& E : AllEntries)
			{
				if (!E.IsValid())
				{
					continue;
				}
				if (bShowOnlyOthers && E->bIsMine)
				{
					continue;
				}
				if (bShowOnlyShared && E->AlsoOpenedBy.IsEmpty())
				{
					continue;
				}
				if (bExternalActorsOnly && !E->DepotPath.Contains(TEXT("__ExternalActors__")))
				{
					continue;
				}
				if (bHaveSearch)
				{
					const bool bMatch =
						E->ActorName.Contains(Needle)   ||
						E->ActorClass.Contains(Needle)  ||
						E->User.Contains(Needle)        ||
						E->Action.Contains(Needle)      ||
						E->DepotPath.Contains(Needle)   ||
						E->PackageName.Contains(Needle);
					if (!bMatch)
					{
						continue;
					}
				}
				VisibleEntries.Add(E);
			}
			VisibleEntries.Sort([](const TSharedPtr<FLockEntry>& A, const TSharedPtr<FLockEntry>& B)
			{
				if (A->User != B->User) return A->User < B->User;
				return A->ActorName.IsEmpty() ? A->DepotPath < B->DepotPath : A->ActorName < B->ActorName;
			});
			if (ListView.IsValid())
			{
				ListView->RequestListRefresh();
			}
		}

		TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FLockEntry> Item, const TSharedRef<STableViewBase>& OwnerTable)
		{
			const FString Display = DisplayName(*Item);
			const bool bShared = !Item->AlsoOpenedBy.IsEmpty();

			// Green = mine, red = someone else's, orange = mine AND someone else has it too.
			const FSlateColor RowColor = bShared
				? FSlateColor(FLinearColor(1.0f, 0.65f, 0.15f))
				: Item->bIsMine
					? FSlateColor(FLinearColor(0.55f, 0.85f, 0.55f))
					: FSlateColor(FLinearColor(1.0f, 0.55f, 0.55f));
			const FString UserText = bShared
				? Item->User + TEXT("  + ") + Item->AlsoOpenedBy
				: Item->User;

			return SNew(STableRow<TSharedPtr<FLockEntry>>, OwnerTable)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.40f).Padding(2.f)
				[
					SNew(STextBlock).Text(FText::FromString(Display)).ColorAndOpacity(RowColor)
				]
				+ SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.f)
				[
					SNew(STextBlock).Text(FText::FromString(UserText)).ColorAndOpacity(bShared ? RowColor : FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().FillWidth(0.10f).Padding(2.f)
				[
					SNew(STextBlock).Text(FText::FromString(Item->Action))
				]
				+ SHorizontalBox::Slot().FillWidth(0.30f).Padding(2.f)
				[
					SNew(STextBlock).Text(FText::FromString(FPaths::GetBaseFilename(Item->DepotPath)))
					.ToolTipText(FText::FromString(Item->DepotPath))
				]
			];
		}

		void OnSelectionChanged(TSharedPtr<FLockEntry> Item, ESelectInfo::Type)
		{
			if (!Item.IsValid() || !GEditor)
			{
				return;
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				return;
			}
			// Try to select the matching actor in the editor.
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;
				UPackage* Pkg = Actor->GetExternalPackage();
				if (!Pkg) Pkg = Actor->GetPackage();
				if (Pkg && Pkg->GetName() == Item->PackageName)
				{
					GEditor->SelectNone(false, true);
					GEditor->SelectActor(Actor, true, true, true);
					return;
				}
			}
		}

		TArray<TSharedPtr<FLockEntry>> AllEntries;
		TArray<TSharedPtr<FLockEntry>> VisibleEntries;
		TSharedPtr<SListView<TSharedPtr<FLockEntry>>> ListView;
		FText StatusText;
		bool bRefreshing = false;
		bool bShowOnlyOthers = false;
		bool bShowOnlyShared = false;
		bool bExternalActorsOnly = true;
		FString SearchText;
	};

	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SLockBrowser)
			];
	}

	void Toggle();

	/** Background poll: toast when a file I have checked out is also checked out by someone else. */
	namespace SharedCheckoutWatch
	{
		static constexpr float FirstCheckSeconds = 20.f;
		static constexpr float WatchIntervalSeconds = 60.f;

		static FTSTicker::FDelegateHandle TickHandle;
		static std::atomic<bool> bQueryRunning{false};
		static bool bLoggedFailure = false;
		static float SecondsUntilCheck = FirstCheckSeconds;
		// "DepotPath|others" already toasted. Pruned each poll, so a share that ends and recurs warns again.
		static TSet<FString> NotifiedKeys;

		static void OnResult(TArray<FLockEntry> Entries, bool bOk, const FString& Error)
		{
			bQueryRunning = false;
			if (!bOk)
			{
				if (!bLoggedFailure)
				{
					UE_LOG(LogLockBrowser, Warning, TEXT("[SharedCheckout] p4 opened failed, watcher idle until it works: %s"), *Error);
					bLoggedFailure = true;
				}
				return;
			}
			bLoggedFailure = false;

			TMap<FString, AActor*> PackageToActor;
			bool bBuiltMap = false;

			TSet<FString> CurrentKeys;
			TArray<FString> NewLines;
			for (FLockEntry& E : Entries)
			{
				if (!E.bIsMine || E.AlsoOpenedBy.IsEmpty())
				{
					continue;
				}
				const FString Key = E.DepotPath + TEXT("|") + E.AlsoOpenedBy;
				CurrentKeys.Add(Key);
				if (NotifiedKeys.Contains(Key))
				{
					continue;
				}
				if (!bBuiltMap)
				{
					BuildPackageToActorMap(PackageToActor);
					bBuiltMap = true;
				}
				ResolveActor(PackageToActor, E);
				NewLines.Add(FString::Printf(TEXT("%s  —  %s"), *DisplayName(E), *E.AlsoOpenedBy));
				UE_LOG(LogLockBrowser, Warning, TEXT("[SharedCheckout] %s also checked out by %s (%s)"),
					*DisplayName(E), *E.AlsoOpenedBy, *E.DepotPath);
			}
			NotifiedKeys = MoveTemp(CurrentKeys);

			if (NewLines.Num() == 0)
			{
				return;
			}

			static constexpr int32 MaxListed = 6;
			FString Body;
			for (int32 i = 0; i < NewLines.Num() && i < MaxListed; ++i)
			{
				Body += NewLines[i] + TEXT("\n");
			}
			if (NewLines.Num() > MaxListed)
			{
				Body += FString::Printf(TEXT("…and %d more"), NewLines.Num() - MaxListed);
			}

			FNotificationInfo Info(FText::Format(
				LOCTEXT("SharedCheckoutTitle", "{0} of your checked-out files also checked out by someone else"),
				FText::AsNumber(NewLines.Num())));
			Info.SubText = FText::FromString(Body.TrimEnd());
			Info.ExpireDuration = 12.f;
			Info.bFireAndForget = true;
			Info.bUseLargeFont = false;
			Info.Hyperlink = FSimpleDelegate::CreateStatic(&Toggle);
			Info.HyperlinkText = LOCTEXT("SharedCheckoutLink", "Open Lock Browser");
			if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Item->SetCompletionState(SNotificationItem::CS_Fail);
			}
		}

		static bool Tick(float DeltaTime)
		{
			SecondsUntilCheck -= DeltaTime;
			if (SecondsUntilCheck > 0.f || bQueryRunning)
			{
				return true;
			}
			SecondsUntilCheck = WatchIntervalSeconds;
			bQueryRunning = true;
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []()
			{
				TArray<FLockEntry> Entries;
				FString Error;
				const bool bOk = RunP4Opened(Entries, Error);
				AsyncTask(ENamedThreads::GameThread, [Entries = MoveTemp(Entries), bOk, Error = MoveTemp(Error)]() mutable
				{
					if (TickHandle.IsValid())
					{
						OnResult(MoveTemp(Entries), bOk, Error);
					}
					else
					{
						bQueryRunning = false;
					}
				});
			});
			return true;
		}

		static void Start()
		{
			if (!TickHandle.IsValid() && !IsRunningCommandlet())
			{
				SecondsUntilCheck = FirstCheckSeconds;
				TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
			}
		}

		static void Stop()
		{
			if (TickHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
				TickHandle.Reset();
			}
			NotifiedKeys.Reset();
		}
	}

	void Register()
	{
		SharedCheckoutWatch::Start();
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&SpawnTab))
			.SetDisplayName(LOCTEXT("TabDisplayName", "Lock Browser"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Live view of Perforce-locked files in this project (Shift+4)."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory());
	}

	void Unregister()
	{
		SharedCheckoutWatch::Stop();
		if (FGlobalTabmanager::Get()->HasTabSpawner(TabId))
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
		}
	}

	void Toggle()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(TabId);
	}
}

#undef LOCTEXT_NAMESPACE
