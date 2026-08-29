#include "SpeechForge.h"

#if WITH_FORGE_KEYS
#include "ForgeKeyRegistry.h"
#endif
#include "ISpeechProvider.h"
#include "SpeechSources.h"
#include "SpeechCredentialStore.h"
#include "SpeechForgeEditorSettings.h"
#include "SpeechForgeSettings.h"
#include "HAL/IConsoleManager.h"

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
				: (USpeechForgeSettings::Get() ? USpeechForgeSettings::Get()->DefaultProviderId : NAME_None);

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
				: (USpeechForgeSettings::Get() ? USpeechForgeSettings::Get()->DefaultProviderId : NAME_None);

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

void FSpeechForgeModule::StartupModule()
{
	UE_LOG(LogSpeechForge, Log, TEXT("SpeechForge started. No providers are built in - each registers itself."));
}

void FSpeechForgeModule::ShutdownModule()
{
	Providers.Empty();
	VoiceSources.Empty();
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
		Key.Owner = LOCTEXT("SpeechForgeOwner", "SpeechForge");
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

void FSpeechForgeModule::RegisterVoiceSource(TSharedRef<ISpeechVoiceSource> Source)
{
	const FName Id = Source->GetVoiceSourceId();
	if (Id.IsNone())
	{
		UE_LOG(LogSpeechForge, Error, TEXT("A voice source tried to register without an id. Ignored."));
		return;
	}

	UnregisterVoiceSource(Id);
	VoiceSources.Add(Source);
	SortVoiceSources();

	UE_LOG(LogSpeechForge, Log, TEXT("Registered voice source '%s' at priority %d."), *Id.ToString(), Source->GetPriority());
	OnVoiceSourcesChanged.Broadcast();
}

void FSpeechForgeModule::UnregisterVoiceSource(FName SourceId)
{
	const int32 Removed = VoiceSources.RemoveAll([SourceId](const TSharedPtr<ISpeechVoiceSource>& Source)
	{
		return Source.IsValid() && Source->GetVoiceSourceId() == SourceId;
	});

	if (Removed > 0)
	{
		OnVoiceSourcesChanged.Broadcast();
	}
}

TArray<TSharedPtr<ISpeechVoiceSource>> FSpeechForgeModule::GetVoiceSources() const
{
	return VoiceSources;
}

void FSpeechForgeModule::SortVoiceSources()
{
	// Descending priority, and stable, so two sources at the same priority keep registration order
	// rather than resolving differently between runs.
	VoiceSources.StableSort([](const TSharedPtr<ISpeechVoiceSource>& A, const TSharedPtr<ISpeechVoiceSource>& B)
	{
		const int32 PriorityA = A.IsValid() ? A->GetPriority() : MIN_int32;
		const int32 PriorityB = B.IsValid() ? B->GetPriority() : MIN_int32;
		return PriorityA > PriorityB;
	});
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSpeechForgeModule, SpeechForge)
