// Localisation: a sibling bank per language, translated line by line, dubbed take by take.
//
// The shape was decided by the face layer model (FaceForge/GRADUATION.md §4): translating a game
// replaces exactly one thing - the words, and with them the audio and the mouth. Everything else
// joins by line id. A sibling bank preserves the ids untouched, so hashes, takes, face banks and
// dialogue assignment all keep working without learning the word "language".

#include "SpeechForgeSubsystem.h"

#include "ISpeechProvider.h"
#include "ISpeechTranslationProvider.h"
#include "SpeechBank.h"
#include "SpeechForge.h"
#include "SpeechForgeSettings.h"
#include "SpeechImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace SpeechLocalizationPrivate
{
	/** Same construction as the subsystem's own SaveAsset - file-local there, so repeated here. */
	void SaveAsset(UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return;
		}

		UPackage* Package = Asset->GetOutermost();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Asset, *FileName, Args);
	}

	/** "pt-BR" -> "PT_BR": the suffix a localised bank's asset name carries. */
	FString LanguageSuffix(const FString& LanguageCode)
	{
		FString Suffix = LanguageCode.ToUpper();
		Suffix.ReplaceCharInline(TEXT('-'), TEXT('_'));
		return Suffix;
	}

	/** A language code sane enough to become an asset suffix and an API parameter. */
	bool IsPlausibleLanguageCode(const FString& Code)
	{
		if (Code.Len() < 2 || Code.Len() > 8)
		{
			return false;
		}
		for (const TCHAR Char : Code)
		{
			if (!FChar::IsAlpha(Char) && Char != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	/** Path matching in the registry's style: either form of a path matches the other. */
	bool PathsRefer(const FString& A, const FString& B)
	{
		return !A.IsEmpty() && !B.IsEmpty() && (A.Contains(B) || B.Contains(A));
	}
}

FString USpeechForgeSubsystem::LocalizeBank(
	const FString& BankPath,
	const FString& TargetLanguage,
	FName TranslationProviderId,
	bool bForce,
	TFunction<void(bool, const FString&)> OnComplete)
{
	using namespace SpeechLocalizationPrivate;

	USpeechBank* SourceBank = Cast<USpeechBank>(LoadSourceAsset(BankPath));
	if (SourceBank == nullptr)
	{
		return FString::Printf(TEXT("No speech bank at '%s'."), *BankPath);
	}

	if (!SourceBank->LanguageCode.IsEmpty())
	{
		return FString::Printf(
			TEXT("'%s' is itself a localised bank (%s). Localise its source bank instead - "
				 "translating a translation compounds every error in it."),
			*SourceBank->GetName(), *SourceBank->LanguageCode);
	}

	if (!IsPlausibleLanguageCode(TargetLanguage))
	{
		return FString::Printf(
			TEXT("'%s' does not look like a language code. Expected something like 'de' or 'pt-BR'."),
			*TargetLanguage);
	}

	FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
	const FName ProviderId = TranslationProviderId.IsNone()
		? Module->ResolveDefaultTranslationProviderId()
		: TranslationProviderId;

	TSharedPtr<ISpeechTranslationProvider> Translator = Module->FindTranslationProvider(ProviderId);
	if (!Translator.IsValid())
	{
		return FString::Printf(
			TEXT("No translation provider named '%s'. Registered: %s."),
			*ProviderId.ToString(),
			*FString::JoinBy(Module->GetTranslationProviderIds(), TEXT(", "),
				[](FName Id) { return Id.ToString(); }));
	}

	if (!Translator->HasCredential())
	{
		return FString::Printf(
			TEXT("%s has no API key. Add one on the Keys page or in Project Settings first."),
			*Translator->GetDisplayName());
	}

	// ---------------------------------------------------------------------------------------------
	// The sibling bank, created beside its source on first use.
	// ---------------------------------------------------------------------------------------------

	const FString SourcePackage = SourceBank->GetOutermost()->GetName();
	const FString LocalizedPackage = FPackageName::GetLongPackagePath(SourcePackage)
		/ FString::Printf(TEXT("%s_%s"),
			*FPackageName::GetShortName(SourcePackage), *LanguageSuffix(TargetLanguage));
	const FString LocalizedAssetName = FPackageName::GetShortName(LocalizedPackage);
	const FString LocalizedObjectPath = LocalizedPackage + TEXT(".") + LocalizedAssetName;

	USpeechBank* Localized = LoadObject<USpeechBank>(nullptr, *LocalizedObjectPath);
	const bool bIsNew = (Localized == nullptr);

	if (bIsNew)
	{
		UPackage* Package = CreatePackage(*LocalizedPackage);
		if (Package == nullptr)
		{
			return FString::Printf(TEXT("Could not create package '%s'."), *LocalizedPackage);
		}

		Localized = NewObject<USpeechBank>(
			Package, *LocalizedAssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Localized);
	}

	Localized->Description = SourceBank->Description;
	Localized->Defaults = SourceBank->Defaults;
	Localized->LanguageCode = TargetLanguage;
	Localized->SourceBankPath = SourceBank->GetPathName();

	// ---------------------------------------------------------------------------------------------
	// What needs translating: new lines, lines whose source text moved, or everything on bForce.
	// The rest still has its authoring fields refreshed, so a re-cast speaker follows the source.
	// ---------------------------------------------------------------------------------------------

	FSpeechTranslationRequest Request;
	Request.SourceLanguage = SourceBank->LanguageCode; // Empty: the service detects it.
	Request.TargetLanguage = TargetLanguage;
	Request.Context = FString::Printf(TEXT("Video game dialogue. %s"), *SourceBank->Description);

	TArray<FName> LinesToTranslate;

	for (const FSpeechLine& SourceLine : SourceBank->Lines)
	{
		const FString SourceTextHash = FSpeechLine::ComputeSpokenTextHash(SourceLine.Text);
		FSpeechLine* Target = Localized->FindLineMutable(SourceLine.LineId);

		FSpeechLineSpec Spec;
		Spec.LineId         = SourceLine.LineId;
		Spec.Direction      = SourceLine.Direction;
		Spec.SpeakerId      = SourceLine.SpeakerId;
		Spec.VoiceOverride  = SourceLine.VoiceOverride;
		Spec.ModelOverride  = SourceLine.ModelOverride;
		Spec.SeedOverride   = SourceLine.SeedOverride;
		Spec.PreviousLineId = SourceLine.PreviousLineId;

		if (Target != nullptr && !bForce && Target->TranslatedFromTextHash == SourceTextHash)
		{
			// Current translation; keep its text, follow everything else.
			Spec.Text = Target->Text;
			Localized->AddOrUpdateLine(Spec);
			continue;
		}

		// Placeholder until the translation lands - the source text, so a failed batch leaves a
		// readable bank rather than holes. TranslatedFromTextHash stays behind on purpose: the
		// line keeps reporting itself untranslated until real words arrive.
		Spec.Text = Target != nullptr ? Target->Text : SourceLine.Text;
		Localized->AddOrUpdateLine(Spec);

		Request.Texts.Add(SourceLine.Text);
		LinesToTranslate.Add(SourceLine.LineId);
	}

	if (LinesToTranslate.Num() == 0)
	{
		Localized->MarkPackageDirty();
		SaveAsset(Localized);

		const FString Summary = FString::Printf(
			TEXT("'%s' is current: all %d line(s) translated against today's source text."),
			*LocalizedAssetName, SourceBank->Lines.Num());
		if (OnComplete) { OnComplete(true, Summary); }
		return FString();
	}

	UE_LOG(LogSpeechForge, Log, TEXT("Translating %d line(s) of '%s' into '%s' through %s..."),
		LinesToTranslate.Num(), *SourceBank->GetName(), *TargetLanguage, *Translator->GetDisplayName());

	// Reloaded by path in the callback rather than captured: an HTTP translator completes frames
	// or seconds later, and a GC pass in between must cost a reload, not a crash.
	const FString SourceObjectPath = SourceBank->GetPathName();

	Translator->Translate(Request,
		[this, SourceObjectPath, LocalizedObjectPath, LocalizedAssetName, LinesToTranslate, OnComplete]
		(const FSpeechTranslationResult& Result)
	{
		if (!Result.bSuccess)
		{
			UE_LOG(LogSpeechForge, Error, TEXT("Translating for '%s' failed: %s"),
				*LocalizedAssetName, *Result.Error);
			if (OnComplete) { OnComplete(false, Result.Error); }
			return;
		}

		USpeechBank* SourceBank = LoadObject<USpeechBank>(nullptr, *SourceObjectPath);
		USpeechBank* Localized = LoadObject<USpeechBank>(nullptr, *LocalizedObjectPath);
		if (SourceBank == nullptr || Localized == nullptr)
		{
			if (OnComplete) { OnComplete(false, TEXT("A bank disappeared while its lines translated.")); }
			return;
		}

		if (Result.Translations.Num() != LinesToTranslate.Num())
		{
			const FString Error = FString::Printf(
				TEXT("The translator returned %d translation(s) for %d line(s). Nothing was written."),
				Result.Translations.Num(), LinesToTranslate.Num());
			UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
			if (OnComplete) { OnComplete(false, Error); }
			return;
		}

		int32 Applied = 0;
		for (int32 Index = 0; Index < LinesToTranslate.Num(); ++Index)
		{
			const FSpeechLine* SourceLine = SourceBank->FindLine(LinesToTranslate[Index]);
			FSpeechLine* Target = Localized->FindLineMutable(LinesToTranslate[Index]);
			if (SourceLine == nullptr || Target == nullptr)
			{
				continue;
			}

			Target->Text = Result.Translations[Index];
			Target->TranslatedFromTextHash = FSpeechLine::ComputeSpokenTextHash(SourceLine->Text);
			++Applied;
		}

		Localized->MarkPackageDirty();
		SpeechLocalizationPrivate::SaveAsset(Localized);

		const FString Summary = FString::Printf(
			TEXT("'%s': %d line(s) translated (%d characters billed). Generate the bank to give ")
			TEXT("them voices - same speakers, same casting; the sounds land in a per-language folder."),
			*LocalizedAssetName, Applied, Result.BilledCharacters);

		UE_LOG(LogSpeechForge, Log, TEXT("%s"), *Summary);
		if (OnComplete) { OnComplete(true, Summary); }
	});

	return FString();
}

TArray<FSpeechLocalizationStatus> USpeechForgeSubsystem::GetLocalizationStatus(const FString& BankPath) const
{
	using namespace SpeechLocalizationPrivate;

	TArray<FSpeechLocalizationStatus> Statuses;

	const USpeechBank* SourceBank = Cast<USpeechBank>(LoadSourceAsset(BankPath));
	if (SourceBank == nullptr)
	{
		return Statuses;
	}

	// The siblings are found from the registry's cache; only actual localisations are loaded.
	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Banks;
	Registry.Get().GetAssetsByClass(USpeechBank::StaticClass()->GetClassPathName(), Banks);

	for (const FAssetData& Data : Banks)
	{
		FString TaggedSource;
		Data.GetTagValue(GET_MEMBER_NAME_CHECKED(USpeechBank, SourceBankPath), TaggedSource);
		if (!PathsRefer(TaggedSource, BankPath) && !PathsRefer(TaggedSource, SourceBank->GetPathName()))
		{
			continue;
		}

		const USpeechBank* Localized = Cast<USpeechBank>(Data.GetAsset());
		if (Localized == nullptr)
		{
			continue;
		}

		FSpeechLocalizationStatus Status;
		Status.LanguageCode = Localized->LanguageCode;
		Status.BankPath = Localized->GetPathName();

		for (const FSpeechLine& SourceLine : SourceBank->Lines)
		{
			const FSpeechLine* Target = Localized->FindLine(SourceLine.LineId);
			if (Target == nullptr || Target->TranslatedFromTextHash.IsEmpty())
			{
				++Status.LinesMissing;
				continue;
			}

			const FString SourceTextHash = FSpeechLine::ComputeSpokenTextHash(SourceLine.Text);
			if (Target->TranslatedFromTextHash == SourceTextHash)
			{
				++Status.LinesCurrent;
			}
			else
			{
				++Status.LinesStale;
			}

			if (!Target->Sound.IsNull())
			{
				++Status.LinesWithAudio;
			}
		}

		Statuses.Add(MoveTemp(Status));
	}

	Statuses.Sort([](const FSpeechLocalizationStatus& A, const FSpeechLocalizationStatus& B)
	{
		return A.LanguageCode < B.LanguageCode;
	});
	return Statuses;
}

void USpeechForgeSubsystem::DubLineAudio(
	const FSpeechLineHandle& LocalizedHandle,
	FOnLineConverted OnComplete)
{
	using namespace SpeechLocalizationPrivate;

	const auto Fail = [&OnComplete](const FString& Error)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
		OnComplete(false, Error);
	};

	USpeechBank* LocalizedBank = Cast<USpeechBank>(LoadSourceAsset(LocalizedHandle.AssetPath));
	FSpeechLine* Line = LocalizedBank ? LocalizedBank->FindLineMutable(LocalizedHandle.LineId) : nullptr;
	if (Line == nullptr)
	{
		return Fail(FString::Printf(TEXT("No line at '%s'."), *LocalizedHandle.ToString()));
	}

	if (LocalizedBank->LanguageCode.IsEmpty() || LocalizedBank->SourceBankPath.IsEmpty())
	{
		return Fail(FString::Printf(
			TEXT("'%s' is not a localised bank. Dubbing carries a recording *across languages* - "
				 "for re-voicing in place, Convert is the operation."),
			*LocalizedBank->GetName()));
	}

	USpeechBank* SourceBank = LoadObject<USpeechBank>(nullptr, *LocalizedBank->SourceBankPath);
	const FSpeechLine* SourceLine = SourceBank ? SourceBank->FindLine(LocalizedHandle.LineId) : nullptr;
	if (SourceLine == nullptr)
	{
		return Fail(FString::Printf(
			TEXT("The source bank at '%s' has no line '%s' to dub from."),
			*LocalizedBank->SourceBankPath, *LocalizedHandle.LineId.ToString()));
	}

	// Which version of the source this dub will be of. Taken before the request goes out, because
	// afterwards the answer is whatever the source has become - and the whole point of holding it
	// is to notice when that stops matching.
	const FString SourceLineAudioHash = SourceLine->ImportedAudioHash;

	// The performance to dub: the source line's current sound, traced back to its file on disk -
	// for a recorded line that is the take's own WAV.
	USoundWave* SourceWave = SourceLine->Sound.LoadSynchronous();
	if (SourceWave == nullptr)
	{
		SourceWave = SourceLine->SourceSound.LoadSynchronous();
	}
	if (SourceWave == nullptr)
	{
		return Fail(FString::Printf(
			TEXT("Source line '%s' has no audio to dub. Record or generate it first."),
			*LocalizedHandle.LineId.ToString()));
	}

	FString AbsoluteSource;
#if WITH_EDITORONLY_DATA
	if (SourceWave->AssetImportData)
	{
		AbsoluteSource = SourceWave->AssetImportData->GetFirstFilename();
	}
#endif
	if (AbsoluteSource.IsEmpty() || !FPaths::FileExists(AbsoluteSource))
	{
		return Fail(FString::Printf(
			TEXT("'%s' has no source file on disk any more, so there is nothing to send."),
			*SourceWave->GetName()));
	}

	// The provider comes from the line's own casting, exactly as conversion does, so the account
	// spent is the account the speaker was cast on.
	const FSpeechVoiceResolution Resolution = ResolveVoice(LocalizedHandle);
	TSharedPtr<ISpeechProvider> Provider = FindProvider(
		Resolution.IsValid() ? Resolution.ProviderId : FSpeechForgeModule::GetPtr()->ResolveDefaultProviderId());
	if (!Provider.IsValid())
	{
		return Fail(TEXT("No speech provider is registered to dub through."));
	}

	if (!Provider->GetCaps().bSupportsDubbing)
	{
		return Fail(FString::Printf(
			TEXT("%s cannot dub a recording across languages. The line can still be generated as "
				 "TTS from its translated text."),
			*Provider->GetDisplayName()));
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();

	FSpeechDubbingRequest Request;
	Request.AbsoluteSourcePath = AbsoluteSource;
	Request.SourceLanguage = SourceBank->LanguageCode; // Empty: the service detects it.
	Request.TargetLanguage = LocalizedBank->LanguageCode;
	Request.AbsoluteOutputPath = Staging /
		FString::Printf(TEXT("%s_dubbed_%s_%s.mp3"),
			*FSpeechImporter::SanitizeAssetName(LocalizedHandle.LineId.ToString()),
			*LanguageSuffix(LocalizedBank->LanguageCode),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	const FString SourceHash = FSpeechImporter::HashFile(AbsoluteSource);
	const FString TargetLanguage = LocalizedBank->LanguageCode;
	const TWeakObjectPtr<USoundWave> SourceAsset = SourceWave;

	UE_LOG(LogSpeechForge, Log, TEXT("Dubbing '%s' into '%s'... (billed by the minute; this takes a while)"),
		*LocalizedHandle.LineId.ToString(), *TargetLanguage);

	Provider->DubSpeech(Request,
		[this, LocalizedHandle, SourceHash, SourceLineAudioHash, TargetLanguage, SourceAsset,
		 Resolution, OnComplete]
		(const FSpeechSynthesisResult& Result)
	{
		USpeechBank* LocalizedBank = Cast<USpeechBank>(LoadSourceAsset(LocalizedHandle.AssetPath));
		FSpeechLine* Line = LocalizedBank ? LocalizedBank->FindLineMutable(LocalizedHandle.LineId) : nullptr;
		if (Line == nullptr)
		{
			OnComplete(false, TEXT("The line disappeared while its audio was dubbed."));
			return;
		}

		if (!Result.bSuccess)
		{
			Line->LastError = Result.Error;
			LocalizedBank->MarkPackageDirty();
			UE_LOG(LogSpeechForge, Error, TEXT("Dubbing '%s' failed: %s"),
				*LocalizedHandle.LineId.ToString(), *Result.Error);
			OnComplete(false, Result.Error);
			return;
		}

		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
		FString SoundsPath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
		SoundsPath = SoundsPath / LanguageSuffix(TargetLanguage);

		FSpeechImportRequest Import;
		Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
		Import.DestinationPackagePath = SoundsPath;
		Import.AssetName = FSpeechImporter::SanitizeAssetName(
			FString::Printf(TEXT("SW_Dubbed_%s"), *LocalizedHandle.LineId.ToString()));
		Import.LineId = Line->LineId;
		Import.SpeakerId = Line->SpeakerId;
		Import.SourceAssetPath = LocalizedHandle.AssetPath;
		Import.bVerifyDuration = false;

		const FSpeechImportResult Imported = FSpeechImporter::Import(Import);
		if (!Imported.bSuccess)
		{
			Line->LastError = Imported.Error;
			LocalizedBank->MarkPackageDirty();
			OnComplete(false, Imported.Error);
			return;
		}

		// A dub is a performance wearing another language: it graduates as a recording, exactly
		// like a conversion, and the pipeline must never overwrite it with TTS.
		if (Line->GeneratedSound.IsNull() && !Line->Sound.IsNull() &&
			Line->Origin == ESpeechLineOrigin::Generated)
		{
			Line->GeneratedSound = Line->Sound;
		}

		Line->Sound = Imported.Sound;
		Line->Origin = ESpeechLineOrigin::Recorded;
		Line->Status = ESpeechLineStatus::Generated;
		Line->SourceSound = SourceAsset.Get();
		Line->SourceAudioHash = SourceHash;
		Line->ImportedAudioHash = Imported.AudioHash;

		// Which recording in the source language this is a dub of. The one fact that lets the line
		// notice, later, that the scene has moved on without it - a second take chosen in English
		// leaves this dub speaking the first one, and nothing else on the line can tell.
		Line->DubbedFromAudioHash = SourceLineAudioHash;
		Line->GeneratedAt = FDateTime::UtcNow();
		Line->ProviderRequestId = Result.RequestId;
		Line->LastError.Reset();

		// No character timings: a dub returns none, and the source's described other words.
		Line->Alignment = FSpeechAlignment();
		Line->Alignment.DurationSeconds = Imported.ImportedDurationSeconds;

		// Deliberately NOT marked as speaking this line's text. The dub's words are the dubbing
		// service's own translation, which nobody has verified against the text translation shown
		// as subtitles - so the line reports words-unverified, which is the honest state and the
		// prompt for a human to listen once.
		Line->SpokenTextHash.Reset();
		Line->AudioCheckedAt = FDateTime();

		// The standard conversion hash - source and resolved voice - because that is exactly what
		// the staleness check recomputes for any line with a SourceSound, and a hash in a private
		// format reads as stale forever. The language is not in it and need not be: a line lives
		// in one language's bank for life. The voice is spurious for a dub (the voice is the
		// source's own), so a re-cast marks a dub stale - a harmless false positive, where the
		// alternative was a permanent one.
		Line->ContentHash = FSpeechLine::ComputeConversionHash(SourceHash, Resolution);

		LocalizedBank->MarkPackageDirty();
		SpeechLocalizationPrivate::SaveAsset(LocalizedBank);

		const FString Message = FString::Printf(
			TEXT("'%s' dubbed into '%s': %s (%.2fs). Listen once - the dub's wording is the ")
			TEXT("service's own translation, not necessarily the subtitle's."),
			*LocalizedHandle.LineId.ToString(), *TargetLanguage,
			*Imported.Sound.ToString(), Imported.ImportedDurationSeconds);

		UE_LOG(LogSpeechForge, Log, TEXT("%s"), *Message);
		OnComplete(true, Message);
	});
}
