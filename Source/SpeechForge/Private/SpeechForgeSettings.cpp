#include "SpeechForgeSettings.h"

#include "SpeechForge.h"
#include "SpeechCredentialStore.h"
#include "ISpeechProvider.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"

namespace SpeechForgeSettingsPrivate
{
	/**
	 * Accept a content folder, or say why not.
	 *
	 * Deliberately strict about the mount point. A path under a root nothing has mounted produces
	 * packages that are created in memory, never written, and reported as successes - so the whole
	 * pipeline runs green and leaves nothing on disk.
	 */
	static bool ValidateContentRoot(const FString& In, FString& OutNormalised, FString& OutProblem)
	{
		OutNormalised = In.TrimStartAndEnd();

		while (OutNormalised.EndsWith(TEXT("/")))
		{
			OutNormalised.LeftChopInline(1);
		}

		if (OutNormalised.IsEmpty())
		{
			OutProblem = TEXT("Empty. Give a content path such as /Game/_EP1/Speech.");
			return false;
		}

		if (!OutNormalised.StartsWith(TEXT("/")))
		{
			OutProblem = FString::Printf(
				TEXT("'%s' is not a content path. It must start with a mount point, such as /Game/."),
				*OutNormalised);
			return false;
		}

		FText Reason;
		if (FPackageName::DoesPackageNameContainInvalidCharacters(OutNormalised, &Reason))
		{
			OutProblem = FString::Printf(TEXT("'%s' is not a usable content path: %s"),
				*OutNormalised, *Reason.ToString());
			return false;
		}

		if (FPackageName::GetPackageMountPoint(OutNormalised).IsNone())
		{
			OutProblem = FString::Printf(
				TEXT("Nothing is mounted at the root of '%s'. Content written there would be created ")
				TEXT("in memory and never saved. Use /Game/... unless you mean a specific plugin's ")
				TEXT("mount point."),
				*OutNormalised);
			return false;
		}

		return true;
	}
}

USpeechForgeSettings::USpeechForgeSettings()
{
}

const USpeechForgeSettings* USpeechForgeSettings::Get()
{
	return GetDefault<USpeechForgeSettings>();
}

FSpeechOutputPaths USpeechForgeSettings::GetOutputPaths() const
{
	FSpeechOutputPaths Paths;

	Paths.Root   = OutputContentPath;
	Paths.Banks  = GetBanksPath();
	Paths.Voices = GetVoicesPath();
	Paths.Sounds = GetSoundsPath();

	return Paths;
}

FSpeechOutputPaths USpeechForgeSettings::SetOutputRoot(const FString& ContentPath)
{
	USpeechForgeSettings* Settings = GetMutableDefault<USpeechForgeSettings>();

	FString Normalised;
	FString Problem;

	if (!SpeechForgeSettingsPrivate::ValidateContentRoot(ContentPath, Normalised, Problem))
	{
		// Report what is configured, not what was asked for. A caller that ignores Problem then reads
		// the truth rather than believing the move happened.
		FSpeechOutputPaths Paths = Settings->GetOutputPaths();
		Paths.Problem = Problem;
		return Paths;
	}

	Settings->OutputContentPath = Normalised;
	Settings->TryUpdateDefaultConfigFile();

	return Settings->GetOutputPaths();
}

FString USpeechForgeSettings::GetAbsoluteStagingDirectory() const
{
	const FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / StagingDirectory);

	if (!IFileManager::Get().DirectoryExists(*Absolute))
	{
		IFileManager::Get().MakeDirectory(*Absolute, true);
	}

	return Absolute;
}

#if WITH_EDITOR

void USpeechForgeSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(USpeechForgeSettings, RequestedSampleRate))
	{
		UE_LOG(LogSpeechForge, Log,
			TEXT("Requested sample rate is now %d Hz. Providers gate some rates behind a subscription ")
			TEXT("tier, and not always the ones you would expect - 44100 is gated on ElevenLabs where ")
			TEXT("48000 is not. The rate is read back off the returned audio and checked either way."),
			RequestedSampleRate);
	}
}

#endif
