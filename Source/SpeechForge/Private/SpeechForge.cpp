#include "SpeechForge.h"

#include "ISpeechProvider.h"
#include "SpeechSources.h"
#include "SpeechCredentialStore.h"
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

			if (const USpeechForgeSettings* Settings = USpeechForgeSettings::Get())
			{
				const_cast<USpeechForgeSettings*>(Settings)->RefreshStatus();
			}
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

			if (const USpeechForgeSettings* Settings = USpeechForgeSettings::Get())
			{
				const_cast<USpeechForgeSettings*>(Settings)->RefreshStatus();
			}
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

	OnProvidersChanged.Broadcast();
}

void FSpeechForgeModule::UnregisterProvider(FName ProviderId)
{
	if (Providers.Remove(ProviderId) > 0)
	{
		UE_LOG(LogSpeechForge, Log, TEXT("Unregistered speech provider '%s'."), *ProviderId.ToString());
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
