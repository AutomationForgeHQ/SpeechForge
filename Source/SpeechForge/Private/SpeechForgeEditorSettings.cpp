#include "SpeechForgeEditorSettings.h"

#include "ISpeechProvider.h"
#include "SpeechCredentialStore.h"
#include "SpeechForge.h"

USpeechForgeEditorSettings::USpeechForgeEditorSettings()
{
	CategoryName = TEXT("Automation Forge");
	SectionName = TEXT("SpeechForge");
}

USpeechForgeEditorSettings* USpeechForgeEditorSettings::Get()
{
	return GetMutableDefault<USpeechForgeEditorSettings>();
}

namespace
{
	/**
	 * Resolve the service through the provider rather than assuming it equals the provider id.
	 * A provider is free to share a vault entry with something else, and the page should report what
	 * is actually read rather than what would be read if it did not.
	 */
	FString ResolveService(FName ProviderId, bool& bOutNoProviders)
	{
		bOutNoProviders = false;
		FString Service = ProviderId.ToString();

		if (FSpeechForgeModule* Module = FSpeechForgeModule::GetPtrIfLoaded())
		{
			// An empty field means "whichever provider is the default" - the same resolution
			// generation uses, so this page and a batch never disagree about whose key it is.
			if (ProviderId.IsNone())
			{
				ProviderId = Module->ResolveDefaultProviderId();
				Service = ProviderId.ToString();
			}

			if (TSharedPtr<ISpeechProvider> Provider = Module->FindProvider(ProviderId))
			{
				Service = Provider->GetCredentialServiceName();
			}
			else if (Module->GetProviderIds().Num() == 0)
			{
				bOutNoProviders = true;
			}
		}
		return Service;
	}
}

void USpeechForgeEditorSettings::RefreshStatus()
{
	bool bNoProviders = false;
	const FString Service = ResolveService(CredentialProviderId, bNoProviders);

	CredentialStatus = bNoProviders
		? TEXT("No providers are registered. Enable a provider plugin first.")
		: FSpeechCredentialStore::DescribeSource(Service);
}

#if WITH_EDITOR

void USpeechForgeEditorSettings::PostInitProperties()
{
	Super::PostInitProperties();
	RefreshStatus();
}

void USpeechForgeEditorSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(USpeechForgeEditorSettings, ApiKeyEntry))
	{
		if (!ApiKeyEntry.IsEmpty())
		{
			bool bNoProviders = false;
			const FString Service = ResolveService(CredentialProviderId, bNoProviders);

			const bool bStored = FSpeechCredentialStore::Set(Service, ApiKeyEntry);

			// Blank the field whether or not the write succeeded: a key sitting in a details panel
			// invites itself into a screenshot, and it is transient so it would be lost anyway.
			ApiKeyEntry.Empty();

			if (!bStored)
			{
				UE_LOG(LogSpeechForge, Error,
					TEXT("Could not store the key for '%s'. Set the %s environment variable instead."),
					*Service, *FSpeechCredentialStore::GetEnvironmentVariableName(Service));
			}
		}

		RefreshStatus();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(USpeechForgeEditorSettings, CredentialProviderId))
	{
		RefreshStatus();
	}
}

#endif
