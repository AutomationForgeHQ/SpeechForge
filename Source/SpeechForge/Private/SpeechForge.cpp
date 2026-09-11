#include "SpeechForge.h"

#if WITH_FORGE_KEYS
#include "ForgeKeyRegistry.h"
#endif
#include "ISpeechProvider.h"
#include "ISpeechTranslationProvider.h"
#include "PseudoTranslationProvider.h"
#include "SpeechSources.h"
#include "SpeechBank.h"
#include "SpeechCredentialStore.h"
#include "SpeechForgeEditorSettings.h"
#include "SpeechForgeSettings.h"
#include "HAL/IConsoleManager.h"
#include "SSpeechLibraryPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogSpeechForge);

#define LOCTEXT_NAMESPACE "FSpeechForgeModule"

namespace SpeechForgeConsole
{
	/**
	 * Console commands rather than buttons, and not for want of trying.
	 *
	 * UFUNCTION(CallInEditor) does not render on a UDeveloperSettings page: the details customization
	 * discards archetype objects before drawing them, and a settings panel edits the CDO, which is
	 * one. A control that silently does nothing is worse than no control, so the console is the
	 * honest surface until a real IDetailCustomization exists.
	 */
	static FAutoConsoleCommand CmdCredentialStatus(
		TEXT("SpeechForge.CredentialStatus"),
		TEXT("Report whether each registered provider has a usable API key, and where it is read from."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
			if (!Module)
			{
				return;
			}

			const TArray<FName> Ids = Module->GetProviderIds();
			if (Ids.Num() == 0)
			{
				UE_LOG(LogSpeechForge, Warning,
					TEXT("No providers are registered. A provider plugin has to be enabled before anything can generate."));
				return;
			}

			for (const FName Id : Ids)
			{
				TSharedPtr<ISpeechProvider> Provider = Module->FindProvider(Id);
				if (!Provider.IsValid())
				{
					continue;
				}

				const FString Service = Provider->GetCredentialServiceName();
				UE_LOG(LogSpeechForge, Log, TEXT("%s: %s"), *Id.ToString(), *FSpeechCredentialStore::DescribeSource(Service));
			}

			// Keep the Editor Preferences page in step, since the console just changed what it reports.
			USpeechForgeEditorSettings::Get()->RefreshStatus();
		}));

	static FAutoConsoleCommand CmdTestConnection(
		TEXT("SpeechForge.TestConnection"),
		TEXT("Make one cheap authenticated call to a provider. Costs nothing and generates nothing. Optional argument: provider id."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
			if (!Module)
			{
				return;
			}

			const FName ProviderId = Args.Num() > 0
				? FName(*Args[0])
				: Module->ResolveDefaultProviderId();

			TSharedPtr<ISpeechProvider> Provider = Module->FindProvider(ProviderId);
			if (!Provider.IsValid())
			{
				UE_LOG(LogSpeechForge, Error,
					TEXT("No provider named '%s'. Run SpeechForge.CredentialStatus to list the registered ones."),
					*ProviderId.ToString());
				return;
			}

			Provider->TestConnection([ProviderId](bool bSuccess, const FString& Message)
			{
				if (bSuccess)
				{
					UE_LOG(LogSpeechForge, Log, TEXT("%s: %s"), *ProviderId.ToString(), *Message);
				}
				else
				{
					UE_LOG(LogSpeechForge, Error, TEXT("%s: %s"), *ProviderId.ToString(), *Message);
				}
			});
		}));

	static FAutoConsoleCommand CmdClearKey(
		TEXT("SpeechForge.ClearKey"),
		TEXT("Forget the stored API key for a provider. Argument: provider id."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
			if (!Module)
			{
				return;
			}

			const FName ProviderId = Args.Num() > 0
				? FName(*Args[0])
				: Module->ResolveDefaultProviderId();

			TSharedPtr<ISpeechProvider> Provider = Module->FindProvider(ProviderId);
			if (!Provider.IsValid())
			{
				UE_LOG(LogSpeechForge, Error, TEXT("No provider named '%s'."), *ProviderId.ToString());
				return;
			}

			const FString Service = Provider->GetCredentialServiceName();
			if (FSpeechCredentialStore::Remove(Service))
			{
				UE_LOG(LogSpeechForge, Log, TEXT("Cleared the stored key for '%s'."), *Service);
			}
			else
			{
				UE_LOG(LogSpeechForge, Log, TEXT("There was no stored key for '%s' to clear."), *Service);
			}

			// Keep the Editor Preferences page in step, since the console just changed what it reports.
			USpeechForgeEditorSettings::Get()->RefreshStatus();
		}));
}

namespace SpeechLibrary
{
	static const FName TabName("SpeechLibrary");
}

void FSpeechForgeModule::StartupModule()
{
	UE_LOG(LogSpeechForge, Log, TEXT("SpeechForge started. No providers are built in - each registers itself."));

	// The one translation provider that ships with the core: keyless pseudo-localisation, so the
	// whole localisation pipeline is exercisable before any vendor plugin or key exists.
	RegisterTranslationProvider(MakeShared<FPseudoTranslationProvider>());

	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(SpeechLibrary::TabName,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab)
					.TabRole(ETabRole::NomadTab)
					[
						SNew(SSpeechLibraryPanel)
					];
			}))
		.SetDisplayName(NSLOCTEXT("SpeechForge", "LibraryTabTitle", "Speech Library"))
		.SetTooltipText(NSLOCTEXT("SpeechForge", "LibraryTabTip",
			"Every line of a bank: what it says, who says it, what its audio is - and the ways to "
			"give it one: generate it, or perform it."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.DialogueWave"))
		// Hidden from the auto-populated Tools list: the family's own "Automation Forge" section
		// below is the one entry, not a second one alphabetised among the engine's tools.
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	ToolMenusHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
		{
			UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
			FToolMenuSection& Section = Tools->FindOrAddSection("AutomationForge",
				NSLOCTEXT("SpeechForge", "ToolsSection", "Automation Forge"));

			Section.AddMenuEntry(
				"SpeechLibrary",
				NSLOCTEXT("SpeechForge", "LibraryMenuLabel", "Speech Library"),
				NSLOCTEXT("SpeechForge", "LibraryMenuTip",
					"Every line of a bank, and the ways to give it a voice: generate it, or perform it."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.DialogueWave"),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(SpeechLibrary::TabName);
				})));
		}));
}

void FSpeechForgeModule::ShutdownModule()
{
	if (UToolMenus* Menus = UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusHandle);
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SpeechLibrary::TabName);
	}

	Providers.Empty();
	TranslationProviders.Empty();
}

FSpeechForgeModule* FSpeechForgeModule::GetPtr()
{
	// Load rather than look up. An add-on calling this from its own StartupModule cannot know whether
	// SpeechForge's module has run yet - a .uplugin dependency guarantees only that it is enabled -
	// and merely looking it up would work on some runs and silently register nothing on others.
	return &FModuleManager::LoadModuleChecked<FSpeechForgeModule>(TEXT("SpeechForge"));
}

FSpeechForgeModule* FSpeechForgeModule::GetPtrIfLoaded()
{
	return FModuleManager::GetModulePtr<FSpeechForgeModule>(TEXT("SpeechForge"));
}

void FSpeechForgeModule::RegisterProvider(TSharedRef<ISpeechProvider> Provider)
{
	const FName Id = Provider->GetProviderId();
	if (Id.IsNone())
	{
		UE_LOG(LogSpeechForge, Error, TEXT("A provider tried to register without an id. Ignored."));
		return;
	}

	// Replacing rather than refusing is what makes hot reload survivable.
	Providers.Add(Id, Provider);
	UE_LOG(LogSpeechForge, Log, TEXT("Registered speech provider '%s'."), *Id.ToString());

	// Offer this provider's key to the shared Keys page — if ForgeKeys happens to be installed.
	// SpeechForge does not link it and does not require it; without it this is a null check and the
	// key is still set on SpeechForge's own settings page.
#if WITH_FORGE_KEYS
	if (IForgeKeysModule* Keys = IForgeKeysModule::GetOrLoad())
	{
		const FString Service = Provider->GetCredentialServiceName();
		const FString Display = Provider->GetDisplayName();

		FForgeKeyProvider Key;
		Key.Id = FName(*FString::Printf(TEXT("SpeechForge.%s"), *Id.ToString()));
		Key.DisplayName = FText::FromString(Display);
		Key.Owner = Provider->GetOwningPluginName();
		Key.Purpose = FText::Format(
			LOCTEXT("SpeechKeyPurpose", "Voice generation through {0}. Your own account — we never resell speech."),
			FText::FromString(Display));
		Key.HelpUrl = Provider->GetCredentialHelpUrl();
		Key.VaultEntryName = FString::Printf(TEXT("SpeechForge/%s"), *Service);
		Key.EnvironmentVariableName = FSpeechCredentialStore::GetEnvironmentVariableName(Service);

		Key.IsSet    = [Service]() { return FSpeechCredentialStore::Has(Service); };
		Key.Describe = [Service]() { return FSpeechCredentialStore::DescribeSource(Service); };
		Key.Store    = [Service](const FString& Secret) { return FSpeechCredentialStore::Set(Service, Secret); };
		Key.Clear    = [Service]() { return FSpeechCredentialStore::Remove(Service); };

		TWeakPtr<ISpeechProvider> WeakProvider = Provider.ToSharedPtr();
		Key.Test = [WeakProvider](FForgeKeyTestResult Done)
		{
			if (TSharedPtr<ISpeechProvider> Pinned = WeakProvider.Pin())
			{
				Pinned->TestConnection([Done](bool bSuccess, const FString& Message)
				{
					Done(bSuccess, FText::FromString(Message));
				});
			}
			else
			{
				Done(false, LOCTEXT("SpeechProviderGone", "That provider is no longer loaded."));
			}
		};
		Keys->Registry().Register(MoveTemp(Key));
	}
#endif

	OnProvidersChanged.Broadcast();
}

void FSpeechForgeModule::UnregisterProvider(FName ProviderId)
{
	if (Providers.Remove(ProviderId) > 0)
	{
		UE_LOG(LogSpeechForge, Log, TEXT("Unregistered speech provider '%s'."), *ProviderId.ToString());
#if WITH_FORGE_KEYS
		if (IForgeKeysModule* Keys = IForgeKeysModule::GetIfLoaded())
		{
			Keys->Registry().Unregister(FName(*FString::Printf(TEXT("SpeechForge.%s"), *ProviderId.ToString())));
		}
#endif
		OnProvidersChanged.Broadcast();
	}
}

TSharedPtr<ISpeechProvider> FSpeechForgeModule::FindProvider(FName ProviderId) const
{
	if (const TSharedPtr<ISpeechProvider>* Found = Providers.Find(ProviderId))
	{
		return *Found;
	}
	return nullptr;
}

TArray<FName> FSpeechForgeModule::GetProviderIds() const
{
	TArray<FName> Ids;
	Providers.GetKeys(Ids);
	Ids.Sort(FNameLexicalLess());
	return Ids;
}

FName FSpeechForgeModule::ResolveDefaultProviderId() const
{
	if (const USpeechForgeSettings* Settings = USpeechForgeSettings::Get())
	{
		if (!Settings->DefaultProviderId.IsNone())
		{
			return Settings->DefaultProviderId;
		}
	}

	if (Providers.Num() == 1)
	{
		for (const auto& Pair : Providers)
		{
			return Pair.Key;
		}
	}

	return NAME_None;
}

void FSpeechForgeModule::RegisterTranslationProvider(TSharedRef<ISpeechTranslationProvider> Provider)
{
	const FName Id = Provider->GetProviderId();
	if (Id.IsNone())
	{
		UE_LOG(LogSpeechForge, Error, TEXT("A translation provider tried to register without an id. Ignored."));
		return;
	}

	// Replacing rather than refusing is what makes hot reload survivable.
	TranslationProviders.Add(Id, Provider);
	UE_LOG(LogSpeechForge, Log, TEXT("Registered translation provider '%s'."), *Id.ToString());

	// Keyed providers join the shared Keys page the same way speech providers do.
#if WITH_FORGE_KEYS
	const FString Service = Provider->GetCredentialServiceName();
	if (!Service.IsEmpty())
	{
		if (IForgeKeysModule* Keys = IForgeKeysModule::GetOrLoad())
		{
			const FString Display = Provider->GetDisplayName();

			FForgeKeyProvider Key;
			Key.Id = FName(*FString::Printf(TEXT("SpeechForge.%s"), *Id.ToString()));
			Key.DisplayName = FText::FromString(Display);
			Key.Owner = LOCTEXT("TranslationKeyOwnerCore", "SpeechForge");
			Key.Purpose = FText::Format(
				LOCTEXT("TranslationKeyPurpose", "Line translation through {0}, for localised voice-over."),
				FText::FromString(Display));
			Key.HelpUrl = Provider->GetCredentialHelpUrl();
			Key.VaultEntryName = FString::Printf(TEXT("SpeechForge/%s"), *Service);
			Key.EnvironmentVariableName = FSpeechCredentialStore::GetEnvironmentVariableName(Service);

			Key.IsSet    = [Service]() { return FSpeechCredentialStore::Has(Service); };
			Key.Describe = [Service]() { return FSpeechCredentialStore::DescribeSource(Service); };
			Key.Store    = [Service](const FString& Secret) { return FSpeechCredentialStore::Set(Service, Secret); };
			Key.Clear    = [Service]() { return FSpeechCredentialStore::Remove(Service); };

			Keys->Registry().Register(MoveTemp(Key));
		}
	}
#endif

	OnProvidersChanged.Broadcast();
}

void FSpeechForgeModule::RegisterLocalizedBankClass(UClass* BankClass)
{
	if (!BankClass || !BankClass->IsChildOf(USpeechBank::StaticClass()))
	{
		UE_LOG(LogSpeechForge, Error,
			TEXT("A localised bank class must derive from USpeechBank; '%s' does not. Ignored."),
			BankClass ? *BankClass->GetName() : TEXT("null"));
		return;
	}

	if (LocalizedBankClass.IsValid() && LocalizedBankClass.Get() != BankClass)
	{
		UE_LOG(LogSpeechForge, Warning,
			TEXT("Localised bank class '%s' replaces '%s'. Two add-ons are competing for the same seat."),
			*BankClass->GetName(), *LocalizedBankClass->GetName());
	}

	LocalizedBankClass = BankClass;
	UE_LOG(LogSpeechForge, Log, TEXT("Localised banks are created as '%s'."), *BankClass->GetName());
}

void FSpeechForgeModule::UnregisterLocalizedBankClass(UClass* BankClass)
{
	if (LocalizedBankClass.Get() == BankClass)
	{
		LocalizedBankClass.Reset();
	}
}

UClass* FSpeechForgeModule::GetLocalizedBankClass() const
{
	return LocalizedBankClass.IsValid() ? LocalizedBankClass.Get() : USpeechBank::StaticClass();
}

void FSpeechForgeModule::UnregisterTranslationProvider(FName ProviderId)
{
	if (TranslationProviders.Remove(ProviderId) > 0)
	{
		UE_LOG(LogSpeechForge, Log, TEXT("Unregistered translation provider '%s'."), *ProviderId.ToString());
#if WITH_FORGE_KEYS
		if (IForgeKeysModule* Keys = IForgeKeysModule::GetIfLoaded())
		{
			Keys->Registry().Unregister(FName(*FString::Printf(TEXT("SpeechForge.%s"), *ProviderId.ToString())));
		}
#endif
		OnProvidersChanged.Broadcast();
	}
}

TSharedPtr<ISpeechTranslationProvider> FSpeechForgeModule::FindTranslationProvider(FName ProviderId) const
{
	if (const TSharedPtr<ISpeechTranslationProvider>* Found = TranslationProviders.Find(ProviderId))
	{
		return *Found;
	}
	return nullptr;
}

TArray<FName> FSpeechForgeModule::GetTranslationProviderIds() const
{
	TArray<FName> Ids;
	TranslationProviders.GetKeys(Ids);
	Ids.Sort(FNameLexicalLess());
	return Ids;
}

FName FSpeechForgeModule::ResolveDefaultTranslationProviderId() const
{
	// The sole real provider wins; Pseudo never wins by default while a real one is installed,
	// because placeholder text reaching a paid TTS call is money spent on gibberish.
	static const FName PseudoId(TEXT("Pseudo"));

	FName SoleReal = NAME_None;
	int32 RealCount = 0;
	for (const auto& Pair : TranslationProviders)
	{
		if (Pair.Key != PseudoId)
		{
			SoleReal = Pair.Key;
			++RealCount;
		}
	}

	if (RealCount == 1)
	{
		return SoleReal;
	}
	if (RealCount == 0 && TranslationProviders.Contains(PseudoId))
	{
		return PseudoId;
	}
	return NAME_None;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSpeechForgeModule, SpeechForge)
