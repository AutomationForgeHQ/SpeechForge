#include "SpeechForgeSubsystem.h"

#include "SpeechForge.h"
#include "SpeechForgeSettings.h"
#include "SpeechCredentialStore.h"
#include "SpeechSources.h"
#include "SpeechBank.h"
#include "SpeechLineDef.h"
#include "SpeechSpeaker.h"
#include "SpeechVoiceProfile.h"
#include "SpeechImporter.h"

#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "Factories/SoundFactory.h"
#include "ISpeechProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Sound/SoundWave.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "Editor.h"

void USpeechForgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// No provider is registered here, and that is the point. Every provider - ElevenLabs included,
	// since its extraction into SpeechForgeElevenLabs - is a separate plugin that registers itself
	// at module startup, and this subsystem never learns any of their names.

	// The drift sweep waits for the registry's first scan to finish, because before that a query for
	// every bank in the project answers with whatever has been discovered so far - which on a cold
	// open is nothing, and reports a clean project by looking at none of it.
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	AssetRegistryModule.Get().OnFilesLoaded().AddUObject(this, &USpeechForgeSubsystem::ReportSourceDriftOnStartup);
}

void USpeechForgeSubsystem::ReportSourceDriftOnStartup()
{
	const TArray<FSpeechSourceDrift> Drifted = CheckSourceDrift();
	if (Drifted.Num() == 0)
	{
		return;
	}

	// Warning, not Log. This is the class of problem that ships: the subtitle, the voice and the
	// script disagreeing with nobody having touched the bank that connects them.
	UE_LOG(LogSpeechForge, Warning,
		TEXT("%d speech bank(s) were harvested from a script that has changed since. Re-harvest to ")
		TEXT("see what moved - lines with audio may now disagree with their subtitles."),
		Drifted.Num());

	for (const FSpeechSourceDrift& Report : Drifted)
	{
		if (Report.SourceWrittenAt == FDateTime())
		{
			UE_LOG(LogSpeechForge, Warning,
				TEXT("  %s: its source '%s' no longer exists, so it can never be re-harvested."),
				*Report.BankPath, *Report.SourceAssetPath);
			continue;
		}

		UE_LOG(LogSpeechForge, Warning,
			TEXT("  %s: '%s' written %s, harvested %s. %d line(s) carry audio."),
			*Report.BankPath,
			*Report.SourceAssetPath,
			*Report.SourceWrittenAt.ToString(),
			*Report.HarvestedAt.ToString(),
			Report.LinesWithAudio);
	}
}

void USpeechForgeSubsystem::Deinitialize()
{
	Batches.Empty();
	Super::Deinitialize();
}

USpeechForgeSubsystem* USpeechForgeSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<USpeechForgeSubsystem>() : nullptr;
}

TSharedPtr<ISpeechProvider> USpeechForgeSubsystem::FindProvider(FName ProviderId) const
{
	FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
	if (!Module)
	{
		return nullptr;
	}

	if (ProviderId.IsNone())
	{
		ProviderId = Module->ResolveDefaultProviderId();
	}

	return Module->FindProvider(ProviderId);
}

TArray<FName> USpeechForgeSubsystem::GetProviderIds() const
{
	FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
	return Module ? Module->GetProviderIds() : TArray<FName>();
}

FSpeechProviderCaps USpeechForgeSubsystem::GetProviderCaps(FName ProviderId) const
{
	if (TSharedPtr<ISpeechProvider> Provider = FindProvider(ProviderId))
	{
		return Provider->GetCaps();
	}

	// An empty ProviderId is how a caller tells "no such provider" from a real answer.
	return FSpeechProviderCaps();
}

FSpeechCredentialInfo USpeechForgeSubsystem::GetCredentialInfo(FName ProviderId) const
{
	FSpeechCredentialInfo Info;

	TSharedPtr<ISpeechProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		return Info;
	}

	const FString Service = Provider->GetCredentialServiceName();

	Info.ProviderId = Provider->GetProviderId();
	Info.bConfigured = Provider->HasCredential();
	Info.Source = FSpeechCredentialStore::DescribeSource(Service);
	Info.EnvironmentVariable = FSpeechCredentialStore::GetEnvironmentVariableName(Service);

	if (!Info.bConfigured)
	{
		Info.SetupHint = Provider->GetCaps().SetupHint;
	}

	return Info;
}

// -------------------------------------------------------------------------------------------------
// Asset plumbing
// -------------------------------------------------------------------------------------------------

UObject* USpeechForgeSubsystem::LoadSourceAsset(const FString& AssetPath) const
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}

	return LoadObject<UObject>(nullptr, *AssetPath);
}

ISpeechLineSource* USpeechForgeSubsystem::AsLineSource(UObject* Asset) const
{
	return Asset ? Cast<ISpeechLineSource>(Asset) : nullptr;
}

TArray<FSpeechLineHandle> USpeechForgeSubsystem::ExpandHandles(const TArray<FSpeechLineHandle>& Handles) const
{
	TArray<FSpeechLineHandle> Expanded;

	TArray<FSpeechLineHandle> Working = Handles;
	if (Working.Num() == 0)
	{
		for (const FString& Path : FindSpeechAssets())
		{
			Working.Add(FSpeechLineHandle(Path, NAME_None));
		}
	}

	for (const FSpeechLineHandle& Handle : Working)
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		if (!Source)
		{
			continue;
		}

		if (!Handle.LineId.IsNone())
		{
			Expanded.AddUnique(Handle);
			continue;
		}

		// **Always expand, whatever the line count.** This used to skip an asset holding one line, on
		// the grounds that a single-line asset resolves None to its only line - which is true of a
		// USpeechLineDef and false of a USpeechBank that happens to contain one. So a one-line bark
		// bank was neither expanded nor resolvable, and every caller reported it as **zero lines**:
		// no status, no character count, and a cost of nothing. Silently pricing a line at zero is
		// the worst answer this plugin can give, and it came from a guard written about how many
		// lines an asset holds when the fact it needed was what kind of asset it is.
		//
		// Expanding unconditionally costs a lookup that resolves to the same handle in the LineDef
		// case, and removes the special case rather than adding a second one.
		TArray<FName> Ids;
		Source->GetLineIds(Ids);
		for (const FName Id : Ids)
		{
			Expanded.AddUnique(FSpeechLineHandle(Handle.AssetPath, Id));
		}
	}

	return Expanded;
}

TArray<FString> USpeechForgeSubsystem::FindSpeechAssets() const
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(USpeechBank::StaticClass()->GetClassPathName(), Assets, true);
	AssetRegistry.Get().GetAssetsByClass(USpeechLineDef::StaticClass()->GetClassPathName(), Assets, true);

	TArray<FString> Paths;
	for (const FAssetData& Data : Assets)
	{
		Paths.Add(Data.GetSoftObjectPath().ToString());
	}

	Paths.Sort();
	return Paths;
}

FString USpeechForgeSubsystem::ApplyRecordedAudio(
	const FString& AssetPath, FName LineId, const FString& AudioSource, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	if (!Source)
	{
		OutError = FString::Printf(TEXT("No speech line source at '%s'."), *AssetPath);
		return FString();
	}

	FSpeechLine* Line = Source->FindLineMutable(LineId);
	if (!Line)
	{
		OutError = FString::Printf(TEXT("'%s' has no line '%s'."), *AssetPath, *LineId.ToString());
		return FString();
	}

	USoundWave* Applied = nullptr;
	FString ImportedFileHash;

	if (AudioSource.StartsWith(TEXT("/")))
	{
		Applied = LoadObject<USoundWave>(nullptr, *AudioSource);
		if (!Applied)
		{
			OutError = FString::Printf(TEXT("No sound wave at '%s'."), *AudioSource);
			return FString();
		}
	}
	else
	{
		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = AudioSource;
		Task->DestinationPath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
		Task->DestinationName = FSpeechImporter::SanitizeAssetName(
			FString::Printf(TEXT("SW_Recorded_%s"), *LineId.ToString()));
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;

		USoundFactory* Factory = NewObject<USoundFactory>();
		Factory->bAutoCreateCue = false;
		Task->Factory = Factory;

		AssetTools.Get().ImportAssetTasks({ Task });

		for (UObject* Object : Task->GetObjects())
		{
			if ((Applied = Cast<USoundWave>(Object)) != nullptr)
			{
				break;
			}
		}

		if (!Applied)
		{
			OutError = FString::Printf(TEXT("'%s' did not import as a sound wave."), *AudioSource);
			return FString();
		}

		ImportedFileHash = FSpeechImporter::HashFile(AudioSource);
	}

	// The graduation itself. Sound moves; GeneratedSound does not - a generated line's GeneratedSound
	// already holds the generated take, and where it is empty (a line that was only ever recorded)
	// there is nothing to preserve.
	Line->Sound = Applied;
	Line->Origin = ESpeechLineOrigin::Recorded;
	Line->ImportedAudioHash = ImportedFileHash;

	// The words the actor performed. Without this a line recorded before it was ever generated has
	// no baseline at all, so rewriting its subtitle afterwards is completely silent - which is the
	// worst version of this failure, because a recorded line is the one nothing can quietly fix.
	Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);
	Line->AudioCheckedAt = FDateTime::UtcNow();
	if (Line->Status == ESpeechLineStatus::Draft || Line->Status == ESpeechLineStatus::Failed)
	{
		Line->Status = ESpeechLineStatus::Generated;
	}
	Line->LastError.Reset();

	Asset->MarkPackageDirty();
	SaveAsset(Asset);

	UE_LOG(LogSpeechForge, Log, TEXT("Line '%s' now plays the recorded performance: %s (origin Recorded)."),
		*LineId.ToString(), *Applied->GetPathName());

	return Applied->GetPathName();
}

void USpeechForgeSubsystem::SaveAsset(UObject* Asset)
{
	if (!Asset)
	{
		return;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package || !Package->IsDirty())
	{
		return;
	}

	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);

	// Every mutation in this subsystem lands here, which makes it the one honest place to say that
	// something changed. Anything drawing these lines can then re-read rather than trust what it
	// drew last. Static, so the announcement goes through the live subsystem.
	if (USpeechForgeSubsystem* Live = USpeechForgeSubsystem::Get())
	{
		Live->OnLibraryChanged.Broadcast();
	}
}

// -------------------------------------------------------------------------------------------------
// Authoring
// -------------------------------------------------------------------------------------------------

FString USpeechForgeSubsystem::CreateOrUpdateBank(
	const FString& AssetPath,
	const FString& BankName,
	const TArray<FSpeechLineSpec>& Lines,
	FName DefaultSpeakerId)
{
	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return FString();
	}

	FString PackageName = AssetPath;
	FString ObjectName = FSpeechImporter::SanitizeAssetName(BankName.IsEmpty() ? TEXT("SB_Speech") : BankName);

	if (PackageName.IsEmpty())
	{
		PackageName = Settings->GetBanksPath() / ObjectName;
	}
	else
	{
		// Accept both "/Game/X/SB_Bank" and "/Game/X/SB_Bank.SB_Bank".
		PackageName.Split(TEXT("."), &PackageName, nullptr);
		ObjectName = FPackageName::GetShortName(PackageName);
	}

	USpeechBank* Bank = LoadObject<USpeechBank>(nullptr, *(PackageName + TEXT(".") + ObjectName));

	if (!Bank)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			UE_LOG(LogSpeechForge, Error, TEXT("Could not create package '%s'."), *PackageName);
			return FString();
		}

		Bank = NewObject<USpeechBank>(Package, *ObjectName, RF_Public | RF_Standalone);
		if (!Bank)
		{
			return FString();
		}

		FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry.Get().AssetCreated(Bank);
	}

	if (!DefaultSpeakerId.IsNone())
	{
		Bank->Defaults.SpeakerId = DefaultSpeakerId;
	}

	for (const FSpeechLineSpec& Spec : Lines)
	{
		Bank->AddOrUpdateLine(Spec);
	}

	Bank->MarkPackageDirty();
	SaveAsset(Bank);

	UE_LOG(LogSpeechForge, Log, TEXT("Bank '%s' now holds %d line(s)."), *PackageName, Bank->Lines.Num());

	return Bank->GetPathName();
}

FString USpeechForgeSubsystem::CreateVoiceProfile(
	const FString& AssetPath,
	const FString& DisplayName,
	const FString& ProviderVoiceId,
	const FString& ProviderVoiceName,
	FName ProviderId,
	const FString& ModelId,
	ESpeechVoiceProvenance Provenance)
{
	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return FString();
	}

	FString PackageName = AssetPath;
	FString ObjectName;

	if (PackageName.IsEmpty())
	{
		const FString BaseName = !DisplayName.IsEmpty() ? DisplayName
			: (!ProviderVoiceName.IsEmpty() ? ProviderVoiceName : ProviderVoiceId);
		ObjectName = FSpeechImporter::SanitizeAssetName(FString::Printf(TEXT("VP_%s"), *BaseName));
		PackageName = Settings->GetVoicesPath() / ObjectName;
	}
	else
	{
		PackageName.Split(TEXT("."), &PackageName, nullptr);
		ObjectName = FPackageName::GetShortName(PackageName);
	}

	USpeechVoiceProfile* Profile =
		LoadObject<USpeechVoiceProfile>(nullptr, *(PackageName + TEXT(".") + ObjectName));

	if (!Profile)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FString();
		}

		Profile = NewObject<USpeechVoiceProfile>(Package, *ObjectName, RF_Public | RF_Standalone);
		Profile->CreatedAt = FDateTime::UtcNow();

		FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry.Get().AssetCreated(Profile);
	}

	if (!DisplayName.IsEmpty())
	{
		Profile->DisplayName = DisplayName;
	}
	if (!ProviderVoiceName.IsEmpty())
	{
		Profile->ProviderVoiceName = ProviderVoiceName;
	}
	Profile->ProviderVoiceId = ProviderVoiceId;
	Profile->ProviderId = !ProviderId.IsNone() ? ProviderId
		: (FSpeechForgeModule::GetPtr() ? FSpeechForgeModule::GetPtr()->ResolveDefaultProviderId() : NAME_None);
	Profile->ModelId = ModelId;
	Profile->Provenance = Provenance;

	Profile->MarkPackageDirty();
	SaveAsset(Profile);

	if (Provenance == ESpeechVoiceProvenance::Premade)
	{
		// Said once, loudly, at the moment it can still be acted on cheaply. A stock voice can be
		// retired by its provider and takes every line generated against it when it goes.
		UE_LOG(LogSpeechForge, Warning,
			TEXT("'%s' records a stock provider voice. Those can be retired by the provider, ")
			TEXT("which would take every line generated against it. Design or clone a voice before ")
			TEXT("building a library on this one."),
			*ObjectName);
	}

	return Profile->GetPathName();
}

FString USpeechForgeSubsystem::CreateOrUpdateSpeaker(
	FName SpeakerId,
	const FString& DisplayName,
	const FString& Description,
	const FString& VoiceProfilePath)
{
	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings || SpeakerId.IsNone())
	{
		return FString();
	}

	USpeechSpeaker* Speaker = nullptr;

	// The sheet is found by who it is, never by where it lives - a speaker seeded into one folder
	// and later moved must still be the same speaker.
	const FString Existing = FindSpeakerAssetPath(SpeakerId);
	if (!Existing.IsEmpty())
	{
		Speaker = LoadObject<USpeechSpeaker>(nullptr, *Existing);
	}

	if (!Speaker)
	{
		const FString ObjectName =
			FSpeechImporter::SanitizeAssetName(FString::Printf(TEXT("SP_%s"), *SpeakerId.ToString()));
		const FString PackageName = Settings->GetSpeakersPath() / ObjectName;

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FString();
		}

		Speaker = NewObject<USpeechSpeaker>(Package, *ObjectName, RF_Public | RF_Standalone);
		Speaker->SpeakerId = SpeakerId;

		FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry.Get().AssetCreated(Speaker);
	}

	// Empty leaves alone, so a harvest that knows only identity never clobbers a cast voice.
	if (!DisplayName.IsEmpty())
	{
		Speaker->DisplayName = DisplayName;
	}
	if (!Description.IsEmpty())
	{
		Speaker->Description = Description;
	}
	if (!VoiceProfilePath.IsEmpty())
	{
		Speaker->VoiceProfile = TSoftObjectPtr<USpeechVoiceProfile>(FSoftObjectPath(VoiceProfilePath));
	}

	Speaker->MarkPackageDirty();
	SaveAsset(Speaker);

	return Speaker->GetPathName();
}

bool USpeechForgeSubsystem::SetSpeakerBinding(FName SpeakerId, FName BindingKey, const FString& ObjectPath)
{
	if (SpeakerId.IsNone() || BindingKey.IsNone())
	{
		return false;
	}

	FString SpeakerPath = FindSpeakerAssetPath(SpeakerId);
	if (SpeakerPath.IsEmpty())
	{
		SpeakerPath = CreateOrUpdateSpeaker(SpeakerId, FString(), FString(), FString());
	}

	USpeechSpeaker* Speaker = LoadObject<USpeechSpeaker>(nullptr, *SpeakerPath);
	if (!Speaker)
	{
		return false;
	}

	Speaker->ExternalBindings.Add(BindingKey, FSoftObjectPath(ObjectPath));
	Speaker->MarkPackageDirty();
	SaveAsset(Speaker);
	return true;
}

FString USpeechForgeSubsystem::FindSpeakerAssetPath(FName SpeakerId) const
{
	if (SpeakerId.IsNone())
	{
		return FString();
	}

	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(USpeechSpeaker::StaticClass()->GetClassPathName(), Assets, true);

	for (const FAssetData& Data : Assets)
	{
		FString Tagged;
		if (Data.GetTagValue(GET_MEMBER_NAME_CHECKED(USpeechSpeaker, SpeakerId), Tagged)
			&& FName(*Tagged) == SpeakerId)
		{
			return Data.GetObjectPathString();
		}
	}

	return FString();
}

TArray<FString> USpeechForgeSubsystem::FindSpeakerAssets() const
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(USpeechSpeaker::StaticClass()->GetClassPathName(), Assets, true);

	TArray<FString> Paths;
	for (const FAssetData& Data : Assets)
	{
		Paths.Add(Data.GetObjectPathString());
	}
	Paths.Sort();
	return Paths;
}

TArray<FString> USpeechForgeSubsystem::FindVoiceProfiles() const
{
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(USpeechVoiceProfile::StaticClass()->GetClassPathName(), Assets, true);

	TArray<FString> Paths;
	for (const FAssetData& Data : Assets)
	{
		Paths.Add(Data.GetObjectPathString());
	}
	Paths.Sort();
	return Paths;
}

FString USpeechForgeSubsystem::FindVoiceProfileByProviderVoice(
	FName ProviderId, const FString& ProviderVoiceId) const
{
	if (ProviderVoiceId.IsEmpty())
	{
		return FString();
	}

	// Loads the profiles to compare - acceptable because a project holds tens of profiles, not
	// thousands, and this runs on a click, not per frame.
	for (const FString& Path : FindVoiceProfiles())
	{
		const USpeechVoiceProfile* Profile = LoadObject<USpeechVoiceProfile>(nullptr, *Path);
		if (Profile
			&& Profile->ProviderVoiceId == ProviderVoiceId
			&& (ProviderId.IsNone() || Profile->ProviderId.IsNone() || Profile->ProviderId == ProviderId))
		{
			return Path;
		}
	}
	return FString();
}

namespace
{
	/** When a content asset's package was last written, or MinValue when there is no such file. */
	FDateTime PackageWriteTime(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return FDateTime::MinValue();
		}

		// "/Game/X/DLG_A.DLG_A" and "/Game/X/DLG_A" both have to work: callers hold whichever form
		// their adapter happened to store.
		FString PackageName = ObjectPath;
		int32 Dot = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), Dot))
		{
			PackageName.LeftInline(Dot);
		}

		FString Filename;
		if (!FPackageName::DoesPackageExist(PackageName, &Filename))
		{
			return FDateTime::MinValue();
		}

		return IFileManager::Get().GetTimeStamp(*Filename);
	}
}

bool USpeechForgeSubsystem::SetBankSource(
	const FString& BankPath, FName SourceAdapter, const FString& SourceAssetPath,
	const FString& SpeakerFilter)
{
	USpeechBank* Bank = LoadObject<USpeechBank>(nullptr, *BankPath);
	if (!Bank)
	{
		return false;
	}

	// The source's own file time, not the wall clock. The drift sweep compares this against that
	// same file later, and two clocks would make the comparison meaningless the first time a machine
	// with a skewed time joined the project.
	const FDateTime SourceTime = PackageWriteTime(SourceAssetPath);

	if (Bank->SourceAdapter == SourceAdapter &&
		Bank->SourceAssetPath == SourceAssetPath &&
		Bank->SourceSpeakerFilter == SpeakerFilter &&
		Bank->SourceHarvestedAt == SourceTime)
	{
		// Re-harvests land here every time; an unchanged stamp is not worth a dirty package.
		return true;
	}

	Bank->SourceAdapter = SourceAdapter;
	Bank->SourceAssetPath = SourceAssetPath;
	Bank->SourceSpeakerFilter = SpeakerFilter;
	Bank->SourceHarvestedAt = SourceTime;
	Bank->MarkPackageDirty();
	SaveAsset(Bank);
	return true;
}

TMap<FName, FString> USpeechForgeSubsystem::FindLineHomes(
	const TArray<FName>& LineIds, const FString& LanguageCode, const FString& IgnoreBankPath) const
{
	TMap<FName, FString> Homes;
	if (LineIds.Num() == 0)
	{
		return Homes;
	}

	FString IgnorePackage = IgnoreBankPath;
	IgnorePackage.Split(TEXT("."), &IgnorePackage, nullptr);

	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Banks;
	AssetRegistry.Get().GetAssetsByClass(USpeechBank::StaticClass()->GetClassPathName(), Banks, true);

	for (const FAssetData& Data : Banks)
	{
		if (!IgnorePackage.IsEmpty() && Data.PackageName.ToString() == IgnorePackage)
		{
			continue;
		}

		// Same language only: a localised sibling holding the same line id is a different line,
		// which is the whole localisation model, not a duplicate home.
		FString BankLanguage;
		Data.GetTagValue(TEXT("LanguageCode"), BankLanguage);
		if (BankLanguage != LanguageCode)
		{
			continue;
		}

		FString Index;
		if (Data.GetTagValue(TEXT("LineIdIndex"), Index) && !Index.IsEmpty())
		{
			for (const FName LineId : LineIds)
			{
				if (!Homes.Contains(LineId) &&
					Index.Contains(TEXT(";") + LineId.ToString() + TEXT(";")))
				{
					Homes.Add(LineId, Data.GetSoftObjectPath().ToString());
				}
			}
			continue;
		}

		// Saved before the index existed: answered the slow way, once. Its next save stamps it.
		if (const USpeechBank* Bank = Cast<USpeechBank>(Data.GetAsset()))
		{
			for (const FName LineId : LineIds)
			{
				if (!Homes.Contains(LineId) && Bank->FindLine(LineId))
				{
					Homes.Add(LineId, Bank->GetPathName());
				}
			}
		}
	}

	return Homes;
}

int32 USpeechForgeSubsystem::RemoveBankLines(
	const FString& BankPath, const TArray<FName>& LineIds, FString& OutError)
{
	// A bank only. A USpeechLineDef holds exactly one line - removing it leaves a husk that every
	// tool would still count, so the honest way to remove that line is deleting the asset.
	UObject* Asset = LoadSourceAsset(BankPath);
	USpeechBank* Bank = Cast<USpeechBank>(Asset);
	if (!Bank)
	{
		OutError = Asset
			? TEXT("That is a single-line asset - delete the asset itself in the Content Browser.")
			: FString::Printf(TEXT("No speech bank at '%s'."), *BankPath);
		return 0;
	}

	const int32 Before = Bank->Lines.Num();
	Bank->Lines.RemoveAll([&LineIds](const FSpeechLine& Line)
	{
		return LineIds.Contains(Line.LineId);
	});

	const int32 Removed = Before - Bank->Lines.Num();
	if (Removed == 0)
	{
		// Nothing matched, nothing dirtied. Not an error: removing the already-removed is the
		// idempotence a caller retrying a batch relies on.
		return 0;
	}

	Bank->MarkPackageDirty();
	SaveAsset(Bank);

	UE_LOG(LogSpeechForge, Log, TEXT("Removed %d line(s) from '%s'; %d remain. Their audio assets stay."),
		Removed, *BankPath, Bank->Lines.Num());

	return Removed;
}

int32 USpeechForgeSubsystem::ClearBankLines(const FString& BankPath, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(BankPath);
	USpeechBank* Bank = Cast<USpeechBank>(Asset);
	if (!Bank)
	{
		OutError = Asset
			? TEXT("That is a single-line asset - delete the asset itself in the Content Browser.")
			: FString::Printf(TEXT("No speech bank at '%s'."), *BankPath);
		return 0;
	}

	const int32 Removed = Bank->Lines.Num();
	if (Removed == 0)
	{
		return 0;
	}

	Bank->Lines.Empty();
	Bank->MarkPackageDirty();
	SaveAsset(Bank);

	UE_LOG(LogSpeechForge, Log, TEXT("Cleared %d line(s) from '%s'. Their audio assets stay."),
		Removed, *BankPath);

	return Removed;
}

bool USpeechForgeSubsystem::UpdateLineAuthoring(
	const FSpeechLineHandle& Handle,
	const FString& Text,
	const FString& Direction,
	FName SpeakerId)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		return false;
	}

	Line->Text = Text;
	Line->Direction = Direction;
	if (!SpeakerId.IsNone())
	{
		Line->SpeakerId = SpeakerId;
	}

	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

bool USpeechForgeSubsystem::SetLineVoiceOverride(
	const FSpeechLineHandle& Handle, const FString& VoiceProfilePath)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		return false;
	}

	Line->VoiceOverride = VoiceProfilePath.IsEmpty()
		? TSoftObjectPtr<USpeechVoiceProfile>()
		: TSoftObjectPtr<USpeechVoiceProfile>(FSoftObjectPath(VoiceProfilePath));

	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

// -------------------------------------------------------------------------------------------------
// Resolution
// -------------------------------------------------------------------------------------------------

FSpeechVoiceResolution USpeechForgeSubsystem::ResolveVoice(const FSpeechLineHandle& Handle) const
{
	FSpeechVoiceResolution Resolution;

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return Resolution;
	}

	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	if (!Source)
	{
		return Resolution;
	}

	const FSpeechLine* Line = Source->FindLine(Handle.LineId);
	if (!Line)
	{
		return Resolution;
	}

	const FSpeechLineDefaults Defaults = Source->GetLineDefaults();
	const FName SpeakerId = Line->SpeakerId.IsNone() ? Defaults.SpeakerId : Line->SpeakerId;

	const FString FallbackModel = !Line->ModelOverride.IsEmpty()
		? Line->ModelOverride
		: (!Defaults.ModelId.IsEmpty() ? Defaults.ModelId : Settings->DefaultModelId);

	// Resolved through the module rather than read off the settings: with the setting unset this is
	// the sole registered provider, and with several registered it is None - which every path below
	// carries into the resolution, where generation refuses it with a message rather than guessing.
	const FName FallbackProvider = FSpeechForgeModule::GetPtr()
		? FSpeechForgeModule::GetPtr()->ResolveDefaultProviderId()
		: NAME_None;

	// 1. The line's own override. Explicit beats everything, so a single awkward line can always be
	//    re-cast without disturbing anything else.
	if (!Line->VoiceOverride.IsNull())
	{
		if (USpeechVoiceProfile* Profile = Line->VoiceOverride.LoadSynchronous())
		{
			return Profile->MakeResolution(FallbackProvider, FallbackModel,
				FString::Printf(TEXT("line override -> %s"), *Profile->GetLabel()));
		}
	}

	// 2. The speaker's sheet. The cast list is the set of speaker assets - identity, voice and
	//    external links consolidated in one place - so this is where "who speaks" becomes "in what
	//    voice" for every line that has not been re-cast by hand.
	if (!SpeakerId.IsNone())
	{
		const FString SpeakerPath = FindSpeakerAssetPath(SpeakerId);
		if (!SpeakerPath.IsEmpty())
		{
			if (USpeechSpeaker* Speaker = LoadObject<USpeechSpeaker>(nullptr, *SpeakerPath))
			{
				if (USpeechVoiceProfile* Profile = Speaker->VoiceProfile.LoadSynchronous())
				{
					return Profile->MakeResolution(FallbackProvider, FallbackModel,
						FString::Printf(TEXT("speaker %s -> %s"),
							*Speaker->GetLabel(), *Profile->GetLabel()));
				}
			}
		}
	}

	// 3. The container's default.
	if (!Defaults.Voice.IsNull())
	{
		if (USpeechVoiceProfile* Profile = Defaults.Voice.LoadSynchronous())
		{
			return Profile->MakeResolution(FallbackProvider, FallbackModel,
				FString::Printf(TEXT("asset default -> %s"), *Profile->GetLabel()));
		}
	}

	// 4. The project default. Mostly useful while prototyping.
	if (!Settings->DefaultVoice.IsNull())
	{
		if (USpeechVoiceProfile* Profile = Settings->DefaultVoice.LoadSynchronous())
		{
			return Profile->MakeResolution(FallbackProvider, FallbackModel,
				FString::Printf(TEXT("project default -> %s"), *Profile->GetLabel()));
		}
	}

	return Resolution;
}

// -------------------------------------------------------------------------------------------------
// Requests and hashing
// -------------------------------------------------------------------------------------------------

bool USpeechForgeSubsystem::BuildRequest(
	const FSpeechLineHandle& Handle,
	FSpeechSynthesisRequest& OutRequest,
	FString& OutHash,
	FString& OutError) const
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	if (!Source)
	{
		OutError = FString::Printf(TEXT("'%s' is not a speech asset."), *Handle.AssetPath);
		return false;
	}

	const FSpeechLine* Line = Source->FindLine(Handle.LineId);
	if (!Line)
	{
		OutError = FString::Printf(TEXT("No line '%s' in '%s'."), *Handle.LineId.ToString(), *Handle.AssetPath);
		return false;
	}

	if (Line->Text.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Line '%s' has no text."), *Line->LineId.ToString());
		return false;
	}

	const FSpeechVoiceResolution Resolution = ResolveVoice(Handle);
	if (!Resolution.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Line '%s' resolves to no voice. Set one on the line, on the asset, or register a ")
			TEXT("voice source that knows speaker '%s'."),
			*Line->LineId.ToString(), *Line->SpeakerId.ToString());
		return false;
	}

	TSharedPtr<ISpeechProvider> Provider = FindProvider(Resolution.ProviderId);
	if (!Provider.IsValid())
	{
		OutError = FString::Printf(TEXT("No provider named '%s'."), *Resolution.ProviderId.ToString());
		return false;
	}

	OutRequest.Voice = Resolution;
	OutRequest.DisplayText = Line->Text;

	// The provider decides what direction becomes, and whether it survives at all. Hashing the result
	// rather than the authoring is what makes an ignored Direction correctly leave lines current.
	OutRequest.RequestText = Provider->BuildRequestText(Line->Text, Line->Direction);
	OutRequest.Seed = Line->SeedOverride;

	const int32 Limit = Provider->GetCharacterLimit(Resolution.ModelId);
	if (OutRequest.RequestText.Len() > Limit)
	{
		OutError = FString::Printf(
			TEXT("Line '%s' is %d characters and model '%s' takes %d. Split it rather than trimming - ")
			TEXT("stitching keeps the prosody continuous across the join."),
			*Line->LineId.ToString(), OutRequest.RequestText.Len(), *Resolution.ModelId, Limit);
		return false;
	}

	// Stitching context, where the line names a predecessor and **this model** can use it.
	//
	// Asked per model rather than per provider, because that is genuinely where the answer lives: on
	// ElevenLabs the expressive model rejects stitching outright and the model that stitches has no
	// audio tags. Dropped and logged rather than refused, so one bank stays usable across models -
	// and because stitching is not part of the content hash, dropping it marks nothing stale.
	if (!Line->PreviousLineId.IsNone())
	{
		if (Provider->SupportsStitchingForModel(Resolution.ModelId))
		{
			if (const FSpeechLine* Previous = Source->FindLine(Line->PreviousLineId))
			{
				OutRequest.PreviousText = Previous->Text;
				OutRequest.PreviousRequestId = Previous->ProviderRequestId;
			}
		}
		else
		{
			UE_LOG(LogSpeechForge, Verbose,
				TEXT("'%s' names a previous line, but model '%s' does not take stitching context - ")
				TEXT("generating it standalone. Switch the bank to a model that stitches if prosody ")
				TEXT("continuity matters more than inline direction."),
				*Line->LineId.ToString(), *Resolution.ModelId);
		}
	}

	OutHash = FSpeechLine::ComputeContentHash(OutRequest.RequestText, Resolution, OutRequest.Seed);
	return true;
}

// -------------------------------------------------------------------------------------------------
// Observation
// -------------------------------------------------------------------------------------------------

TArray<FSpeechLineStatus> USpeechForgeSubsystem::GetLineStatus(const TArray<FSpeechLineHandle>& Handles) const
{
	TArray<FSpeechLineStatus> Report;

	for (const FSpeechLineHandle& Handle : ExpandHandles(Handles))
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		if (!Source)
		{
			continue;
		}

		const FSpeechLine* Line = Source->FindLine(Handle.LineId);
		if (!Line)
		{
			continue;
		}

		FSpeechLineStatus Status;
		Status.Handle = Handle;
		Status.Status = Line->Status;
		Status.Origin = Line->Origin;
		Status.SpeakerId = Line->SpeakerId.IsNone() ? Source->GetLineDefaults().SpeakerId : Line->SpeakerId;
		Status.LastError = Line->LastError;
		Status.CharacterCount = Line->Text.Len();
		Status.DurationSeconds = Line->Alignment.DurationSeconds;
		Status.SoundPath = Line->Sound.IsNull() ? FString() : Line->Sound.ToString();
		Status.ResolvedVoice = ResolveVoice(Handle);

		// Measured from what the line plays, like everything else here: a re-voicing is a take's
		// variant, so the audio either is one or it is not. The legacy line-level conversion is
		// caught by its source hash.
		if (!Line->Sound.IsNull())
		{
			const FString LineSound = Line->Sound.ToString();
			Status.bRevoiced = !Line->SourceAudioHash.IsEmpty();

			for (const FSpeechLineTake& Take : Line->Takes)
			{
				for (const FSpeechTakeAudio& Variant : Take.Variants)
				{
					if (Variant.SoundPath == LineSound)
					{
						Status.bRevoiced = true;
					}
				}
			}
		}

		// Staleness is computed, never stored. Storing it would mean invalidating every line whenever
		// any voice asset anywhere changed, and getting that wrong is silent by construction.
		//
		// Two questions are asked here, and keeping them apart is the whole point. *Do the words
		// still match the audio* applies to every line that has audio, however it arrived, and is
		// the one that ships broken subtitles when it goes unasked. *Would re-running the operation
		// produce something different* applies only to audio a tool made, and is the one with a
		// button attached. They used to be a single hash, which is why a recorded line could have
		// its script rewritten in silence.
		TArray<FString> Reasons;

		if (!Line->Sound.IsNull())
		{
			if (Line->SpokenTextHash.IsEmpty())
			{
				// Audio from before this baseline existed - but only genuinely unverifiable where
				// nothing else can answer the question. A synthesised line still carries a content
				// hash taken over its text, so a rewrite there is caught anyway; flagging those too
				// would light up an entire existing library and teach everyone to ignore the flag.
				// What is left is exactly the audio no text hash ever covered: recorded takes, and
				// conversions, whose content hash deliberately holds no words at all.
				const bool bTextCoveredByContentHash =
					Line->Status == ESpeechLineStatus::Generated &&
					!Line->ContentHash.IsEmpty() &&
					Line->SourceAudioHash.IsEmpty();

				Status.bWordsUnverified = !bTextCoveredByContentHash;
			}
			else if (FSpeechLine::ComputeSpokenTextHash(Line->Text) != Line->SpokenTextHash)
			{
				Status.bWordsDrifted = true;
				Status.bStale = true;

				const bool bPerformance =
					Line->Origin == ESpeechLineOrigin::Recorded ||
					Line->Origin == ESpeechLineOrigin::Edited;

				Reasons.Add(bPerformance
					? TEXT("THE SCRIPT CHANGED AFTER THIS WAS PERFORMED - the subtitle and the audio "
					       "now say different things, and no regeneration can fix it: this needs a pickup")
					: TEXT("the script changed after this audio was made - the subtitle and the voice "
					       "now say different things"));
			}
		}

		// The third question, and the only one whose answer lives in another asset: *is my source
		// still the recording I was made from*. It is asked of dubs alone, because a dub is the one
		// operation derived from another bank's line - and that line can be re-recorded, or have a
		// different take chosen on it, long after this one was made. Nothing else on this line moves
		// when that happens: its own audio, its own hashes and its own text are all exactly as they
		// were, so without this it goes on playing a dub of a performance the scene has replaced.
		//
		// Cheap on purpose. The comparison is against a field the source line already maintains, so
		// it costs a bank load and a string compare rather than a re-read of anybody's audio.
		if (!Line->DubbedFromAudioHash.IsEmpty())
		{
			if (const USpeechBank* Bank = Cast<USpeechBank>(Asset))
			{
				if (!Bank->SourceBankPath.IsEmpty())
				{
					const USpeechBank* SourceBank =
						LoadObject<USpeechBank>(nullptr, *Bank->SourceBankPath);
					const FSpeechLine* SourceLine =
						SourceBank ? SourceBank->FindLine(Handle.LineId) : nullptr;

					// An empty hash on the source is not a mismatch - it is a source that has never
					// been asked the question. Reporting that as a fault would flag every dub whose
					// source predates audio hashing.
					if (SourceLine && !SourceLine->ImportedAudioHash.IsEmpty() &&
						SourceLine->ImportedAudioHash != Line->DubbedFromAudioHash)
					{
						Status.bStale = true;
						Reasons.Add(
							TEXT("the source language's recording changed after this was dubbed - "
							     "this line still speaks the old performance; dub it again"));
					}
				}
			}
		}

		if (Line->Status == ESpeechLineStatus::Generated && !Line->ContentHash.IsEmpty())
		{
			// A converted line is asked a different operation question. It never read the text, so
			// hashing the text against it would report every re-voiced performance as permanently
			// stale. What would make a *re-conversion* differ is the source or the voice - which is
			// unrelated to whether the words still match, asked above and asked of every line.
			if (!Line->SourceAudioHash.IsEmpty())
			{
				if (FSpeechLine::ComputeConversionHash(Line->SourceAudioHash, Status.ResolvedVoice)
					!= Line->ContentHash)
				{
					Status.bStale = true;

					const FString VoiceDiff = Status.ResolvedVoice.DescribeDifference(Line->GeneratedWith);
					Reasons.Add(VoiceDiff.IsEmpty()
						? TEXT("the source recording changed")
						: VoiceDiff + TEXT(" - re-voice the performance from its source"));
				}
			}
			else
			{
				FSpeechSynthesisRequest Request;
				FString Hash;
				FString Error;

				if (BuildRequest(Handle, Request, Hash, Error) && Hash != Line->ContentHash)
				{
					Status.bStale = true;

					const FString VoiceDiff = Status.ResolvedVoice.DescribeDifference(Line->GeneratedWith);
					if (!VoiceDiff.IsEmpty())
					{
						Reasons.Add(VoiceDiff);
					}
					else if (!Status.bWordsDrifted)
					{
						// Nothing about the voice moved and the words are intact, so it was the
						// direction - which changes the reading without changing what is said.
						Reasons.Add(TEXT("direction changed"));
					}

					if (Line->Origin == ESpeechLineOrigin::Recorded && !Status.bWordsDrifted)
					{
						// The report with money attached: a pickup session, scoped exactly.
						Reasons.Add(TEXT("line was RECORDED, so this needs a pickup, not a regeneration"));
					}
				}
			}
		}

		Status.StaleReason = FString::Join(Reasons, TEXT(" | "));

		Report.Add(MoveTemp(Status));
	}

	return Report;
}

void USpeechForgeSubsystem::OpenLibraryAt(const FString& BankPath, FName LineId)
{
	// The tab is invoked by name rather than by holding a widget: the panel may not exist yet, and
	// whether it does is not this subsystem's business.
	FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("SpeechLibrary")));

	// Broadcast after invoking, so a panel spawned by that call is already listening.
	OnLibraryFocusRequested.Broadcast(BankPath, LineId);
}

FString USpeechForgeSubsystem::GetBankSourceDescription(const FString& BankPath) const
{
	const USpeechBank* Bank = LoadObject<USpeechBank>(nullptr, *BankPath);
	if (!Bank || Bank->SourceAdapter.IsNone() || Bank->SourceAssetPath.IsEmpty())
	{
		return FString();
	}

	return FString::Printf(TEXT("%s|%s"), *Bank->SourceAdapter.ToString(), *Bank->SourceAssetPath);
}

TArray<FSpeechSourceDrift> USpeechForgeSubsystem::CheckSourceDrift() const
{
	TArray<FSpeechSourceDrift> Drifted;

	const FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> Banks;
	AssetRegistry.GetAssetsByClass(USpeechBank::StaticClass()->GetClassPathName(), Banks);

	for (const FAssetData& BankData : Banks)
	{
		// Read off the registry's cache first. A project's banks are mostly not adapter-sourced, and
		// those are dismissed here without ever being loaded - which is what keeps a project-wide
		// sweep cheap enough to run on startup without anybody noticing it happened.
		FString SourcePath;
		if (!BankData.GetTagValue(GET_MEMBER_NAME_CHECKED(USpeechBank, SourceAssetPath), SourcePath) ||
			SourcePath.IsEmpty())
		{
			continue;
		}

		const FDateTime SourceTime = PackageWriteTime(SourcePath);
		if (SourceTime == FDateTime::MinValue())
		{
			// The script this bank was harvested from is gone. Worth reporting rather than skipping:
			// a bank whose source vanished cannot be re-harvested, and somebody should know that
			// before they rely on the write-back button that is still on screen.
			const USpeechBank* MissingSourceBank = Cast<USpeechBank>(BankData.GetAsset());
			if (!MissingSourceBank)
			{
				continue;
			}

			FSpeechSourceDrift Report;
			Report.BankPath = BankData.GetSoftObjectPath().ToString();
			Report.SourceAssetPath = SourcePath;
			Report.SourceAdapter = MissingSourceBank->SourceAdapter;
			Report.HarvestedAt = MissingSourceBank->SourceHarvestedAt;
			Drifted.Add(MoveTemp(Report));
			continue;
		}

		const USpeechBank* Bank = Cast<USpeechBank>(BankData.GetAsset());
		if (!Bank || Bank->SourceHarvestedAt == FDateTime())
		{
			// Never stamped - harvested before this baseline existed. Nothing to compare against, and
			// inventing a comparison would either cry wolf over every bank or silence all of them.
			continue;
		}

		if (SourceTime <= Bank->SourceHarvestedAt)
		{
			continue;
		}

		FSpeechSourceDrift Report;
		Report.BankPath = BankData.GetSoftObjectPath().ToString();
		Report.SourceAssetPath = SourcePath;
		Report.SourceAdapter = Bank->SourceAdapter;
		Report.HarvestedAt = Bank->SourceHarvestedAt;
		Report.SourceWrittenAt = SourceTime;

		for (const FSpeechLine& Line : Bank->Lines)
		{
			if (!Line.Sound.IsNull())
			{
				++Report.LinesWithAudio;
			}
		}

		Drifted.Add(MoveTemp(Report));
	}

	return Drifted;
}

FSpeechCostEstimate USpeechForgeSubsystem::EstimateGenerationCost(
	const TArray<FSpeechLineHandle>& Handles, bool bIncludeCurrent) const
{
	FSpeechCostEstimate Estimate;

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return Estimate;
	}

	Estimate.Currency = Settings->Currency;

	for (const FSpeechLineHandle& Handle : ExpandHandles(Handles))
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		if (!Source)
		{
			continue;
		}

		const FSpeechLine* Line = Source->FindLine(Handle.LineId);
		if (!Line || Line->Text.IsEmpty())
		{
			continue;
		}

		FSpeechSynthesisRequest Request;
		FString Hash;
		FString Error;

		if (!BuildRequest(Handle, Request, Hash, Error))
		{
			continue;
		}

		const bool bWouldSkip =
			   Line->IsBusy()
			|| !Line->IsOwnedByPipeline()
			|| (Line->Status == ESpeechLineStatus::Generated && Hash == Line->ContentHash);

		if (bWouldSkip && !bIncludeCurrent)
		{
			++Estimate.SkippedCount;
			continue;
		}

		++Estimate.LineCount;

		// Characters of the text that will actually be sent, direction included where it survives.
		// This is what bills, which is why the estimate is exact rather than projected.
		Estimate.TotalCharacters += Request.RequestText.Len();

		TSharedPtr<ISpeechProvider> Provider = FindProvider(Request.Voice.ProviderId);
		if (Provider.IsValid() && Provider->GetCaps().bIsMetered)
		{
			Estimate.bAnyMetered = true;
			Estimate.BilledCharacters += Request.RequestText.Len();
		}
	}

	if (Estimate.bAnyMetered && Settings->CostPerThousandCharacters > 0.f)
	{
		Estimate.EstimatedCost =
			(static_cast<float>(Estimate.BilledCharacters) / 1000.f) * Settings->CostPerThousandCharacters;
	}

	return Estimate;
}

FSpeechCostEstimate USpeechForgeSubsystem::EstimateConversionCost(
	const TArray<FSpeechLineHandle>& Handles, const FString& SourceAudio) const
{
	FSpeechCostEstimate Estimate;
	Estimate.BillingUnit = ESpeechBillingUnit::Seconds;

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return Estimate;
	}

	Estimate.Currency = Settings->Currency;

	// One shared source measured once, rather than per line: converting a whole bank from a single
	// file is the batch case, and re-reading it for every handle would be the same answer at cost.
	float SharedSeconds = -1.f;
	if (!SourceAudio.IsEmpty())
	{
		if (FPaths::FileExists(SourceAudio))
		{
			SharedSeconds = FSpeechImporter::ReadWavDuration(SourceAudio);
		}
		else if (const USoundWave* Wave = LoadObject<USoundWave>(nullptr, *SourceAudio))
		{
			SharedSeconds = Wave->Duration;
		}
		else
		{
			SharedSeconds = 0.f;
		}
	}

	for (const FSpeechLineHandle& Handle : ExpandHandles(Handles))
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		if (!Source)
		{
			continue;
		}

		const FSpeechLine* Line = Source->FindLine(Handle.LineId);
		if (!Line)
		{
			continue;
		}

		++Estimate.LineCount;

		float Seconds = SharedSeconds;
		if (Seconds < 0.f)
		{
			// No source given, so the question is what a re-cast would send: whatever this line was
			// converted from last time.
			const USoundWave* Wave = Line->SourceSound.LoadSynchronous();
			Seconds = Wave ? Wave->Duration : 0.f;
		}

		if (Seconds <= 0.f)
		{
			++Estimate.UnpricedCount;
			continue;
		}

		Estimate.TotalSeconds += Seconds;

		const FSpeechVoiceResolution Resolution = ResolveVoice(Handle);
		TSharedPtr<ISpeechProvider> Provider = FindProvider(Resolution.ProviderId);

		// Metered *and* able to convert: a provider that cannot re-voice would never be sent this
		// audio, so counting it would price an operation that will not happen.
		if (Provider.IsValid() && Provider->GetCaps().bIsMetered && Provider->GetCaps().bSupportsVoiceConversion)
		{
			Estimate.bAnyMetered = true;
			Estimate.BilledSeconds += Seconds;
		}
	}

	if (Estimate.bAnyMetered && Settings->CostPerMinuteOfAudio > 0.f)
	{
		Estimate.EstimatedCost = (Estimate.BilledSeconds / 60.f) * Settings->CostPerMinuteOfAudio;
	}

	return Estimate;
}

FSpeechBatchStatus USpeechForgeSubsystem::GetBatchStatus(const FString& BatchId) const
{
	FSpeechBatchStatus Status;
	Status.BatchId = BatchId;

	const FSpeechBatch* Batch = Batches.Find(BatchId);
	if (!Batch)
	{
		// An unknown id and a finished batch look the same on purpose - a batch stops being tracked
		// once it settles, and the per-line status is the durable answer afterwards.
		return Status;
	}

	Status.bTracked = true;
	Status.Total = Batch->Total;
	Status.Succeeded = Batch->Succeeded;
	Status.Failed = Batch->Failed;
	Status.InFlight = Batch->InFlight;
	Status.BilledCharacters = Batch->BilledCharacters;
	Status.bFinished = (Batch->InFlight == 0 && Batch->Pending.Num() == 0);

	return Status;
}

// -------------------------------------------------------------------------------------------------
// Generation
// -------------------------------------------------------------------------------------------------

FString USpeechForgeSubsystem::MakeBatchId()
{
	// Readable rather than a GUID, because a human reads it in a log line.
	static int32 Counter = 0;
	return FString::Printf(TEXT("speech-%s-%03d"),
		*FDateTime::Now().ToString(TEXT("%H%M%S")), ++Counter);
}

FString USpeechForgeSubsystem::GenerateLines(const TArray<FSpeechLineHandle>& Handles, bool bForce)
{
	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	if (!Settings)
	{
		return FString();
	}

	FSpeechBatch Batch;
	Batch.BatchId = MakeBatchId();
	Batch.StartedAt = FPlatformTime::Seconds();
	Batch.MaxConcurrent = 1;

	for (const FSpeechLineHandle& Handle : ExpandHandles(Handles))
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		if (!Source)
		{
			continue;
		}

		FSpeechLine* Line = Source->FindLineMutable(Handle.LineId);
		if (!Line || Line->Text.IsEmpty())
		{
			continue;
		}

		// Already in flight. Skipping rather than resubmitting is what makes a retry after a timeout
		// safe, and money here is spent at submission.
		if (Line->IsBusy())
		{
			continue;
		}

		// Graduated. Force does not override this, deliberately: bForce means "I know it looks
		// current", not "throw away the take an actor recorded".
		if (!Line->IsOwnedByPipeline())
		{
			UE_LOG(LogSpeechForge, Verbose,
				TEXT("Skipping '%s' - its audio is %s and the pipeline no longer owns it."),
				*Line->LineId.ToString(),
				*UEnum::GetDisplayValueAsText(Line->Origin).ToString());
			continue;
		}

		FSpeechSynthesisRequest Request;
		FString Hash;
		FString Error;

		if (!BuildRequest(Handle, Request, Hash, Error))
		{
			Line->Status = ESpeechLineStatus::Failed;
			Line->LastError = Error;
			Asset->MarkPackageDirty();
			Batch.DirtyAssets.Add(Handle.AssetPath);
			UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
			continue;
		}

		if (!bForce && Line->Status == ESpeechLineStatus::Generated && Hash == Line->ContentHash)
		{
			continue;
		}

		// Concurrency belongs to the provider, not to us. Exceeding it queues rather than erroring,
		// so a batch that ignored it would look like it worked and simply be slow.
		TSharedPtr<ISpeechProvider> Provider = FindProvider(Request.Voice.ProviderId);
		if (Provider.IsValid())
		{
			const int32 ProviderLimit = FMath::Max(1, Provider->GetCaps().MaxConcurrentRequests);
			const int32 Configured = Settings->MaxConcurrentRequestsOverride;

			// The override may only go below the provider's number, never above.
			Batch.MaxConcurrent = Configured > 0 ? FMath::Min(Configured, ProviderLimit) : ProviderLimit;
		}

		Batch.Pending.Add(Handle);
	}

	if (Batch.Pending.Num() == 0)
	{
		UE_LOG(LogSpeechForge, Log, TEXT("Nothing to generate - every line was current, busy or graduated."));
		return FString();
	}

	Batch.Total = Batch.Pending.Num();

	const FString BatchId = Batch.BatchId;
	Batches.Add(BatchId, MoveTemp(Batch));

	UE_LOG(LogSpeechForge, Log, TEXT("Batch %s: %d line(s), up to %d at a time."),
		*BatchId, Batches[BatchId].Total, Batches[BatchId].MaxConcurrent);

	PumpBatch(BatchId);
	return BatchId;
}

void USpeechForgeSubsystem::PumpBatch(const FString& BatchId)
{
	FSpeechBatch* Batch = Batches.Find(BatchId);
	if (!Batch)
	{
		return;
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

	while (!Batch->bCancelled && Batch->InFlight < Batch->MaxConcurrent && Batch->Pending.Num() > 0)
	{
		const FSpeechLineHandle Handle = Batch->Pending.Pop(EAllowShrinking::No);

		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
		if (!Line)
		{
			++Batch->Failed;
			continue;
		}

		FSpeechSynthesisRequest Request;
		FString Hash;
		FString Error;

		if (!BuildRequest(Handle, Request, Hash, Error))
		{
			Line->Status = ESpeechLineStatus::Failed;
			Line->LastError = Error;
			++Batch->Failed;
			continue;
		}

		TSharedPtr<ISpeechProvider> Provider = FindProvider(Request.Voice.ProviderId);
		if (!Provider.IsValid())
		{
			Line->Status = ESpeechLineStatus::Failed;
			Line->LastError = TEXT("Provider disappeared between planning and sending.");
			++Batch->Failed;
			continue;
		}

		// Raw responses are kept rather than deleted. Where a seed cannot reproduce a generation this
		// file and the provider's history are the only two copies, and only one survives the account.
		const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();
		Request.AbsoluteOutputPath = Staging /
			FString::Printf(TEXT("%s_%s.%s"),
				*FSpeechImporter::SanitizeAssetName(Handle.LineId.ToString()),
				*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")),
				*Provider->GetAudioFormat());

		Line->Status = ESpeechLineStatus::Generating;
		Line->LastError.Reset();
		Asset->MarkPackageDirty();
		Batch->DirtyAssets.Add(Handle.AssetPath);

		++Batch->InFlight;

		const FSpeechVoiceResolution Resolution = Request.Voice;

		Provider->Synthesize(Request,
			[this, BatchId, Handle, Hash, Resolution](const FSpeechSynthesisResult& Result)
		{
			OnLineSynthesized(BatchId, Handle, Result, Hash, Resolution);
		});
	}

	// Settled. Save once per asset rather than once per line - a bank of two hundred lines would
	// otherwise be written to disk two hundred times.
	if (Batch->InFlight == 0 && Batch->Pending.Num() == 0)
	{
		for (const FString& Path : Batch->DirtyAssets)
		{
			SaveAsset(LoadSourceAsset(Path));
		}

		UE_LOG(LogSpeechForge, Log,
			TEXT("Batch %s finished: %d succeeded, %d failed, %d characters billed, %.1fs."),
			*BatchId, Batch->Succeeded, Batch->Failed, Batch->BilledCharacters,
			FPlatformTime::Seconds() - Batch->StartedAt);

		Batches.Remove(BatchId);
	}
}

void USpeechForgeSubsystem::OnLineSynthesized(
	const FString& BatchId,
	const FSpeechLineHandle& Handle,
	const FSpeechSynthesisResult& Result,
	const FString& ExpectedHash,
	const FSpeechVoiceResolution& Resolution)
{
	FSpeechBatch* Batch = Batches.Find(BatchId);
	if (Batch)
	{
		--Batch->InFlight;
	}

	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	if (!Line)
	{
		if (Batch)
		{
			++Batch->Failed;
			PumpBatch(BatchId);
		}
		return;
	}

	if (!Result.bSuccess)
	{
		Line->Status = ESpeechLineStatus::Failed;
		Line->LastError = Result.Error;

		// Counted even on failure where the provider bills at submission - a failed generation on a
		// pay-per-character plan is still a paid one, and pretending otherwise understates the bill.
		if (Batch)
		{
			Batch->BilledCharacters += Result.BilledCharacters;
			++Batch->Failed;
		}

		UE_LOG(LogSpeechForge, Error, TEXT("'%s': %s"), *Handle.LineId.ToString(), *Result.Error);

		Asset->MarkPackageDirty();
		if (Batch)
		{
			PumpBatch(BatchId);
		}
		return;
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

	// A localised bank's sounds land in a per-language subfolder. Same asset names by design -
	// SW_<LineId> is the naming scheme - so without the folder split, generating German would
	// overwrite the English audio it was translated from.
	FString SoundsPath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
	if (const USpeechBank* OwningBank = Cast<USpeechBank>(Asset))
	{
		if (!OwningBank->LanguageCode.IsEmpty())
		{
			FString LanguageFolder = OwningBank->LanguageCode.ToUpper();
			LanguageFolder.ReplaceCharInline(TEXT('-'), TEXT('_'));
			SoundsPath = SoundsPath / LanguageFolder;
		}
	}

	FSpeechImportRequest Import;
	Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
	Import.DestinationPackagePath = SoundsPath;
	Import.AssetName = FString::Printf(TEXT("SW_%s"), *Handle.LineId.ToString());
	Import.Alignment = Result.Alignment;
	Import.LineId = Line->LineId;
	Import.SpeakerId = Line->SpeakerId;
	Import.SourceAssetPath = Handle.AssetPath;
	Import.bVerifyDuration = Settings ? Settings->bVerifyAlignmentAgainstDuration : true;
	Import.DurationToleranceSeconds = Settings ? Settings->AlignmentDurationToleranceSeconds : 0.05f;

	const FSpeechImportResult Imported = FSpeechImporter::Import(Import);

	if (!Imported.bSuccess)
	{
		Line->Status = ESpeechLineStatus::Failed;
		Line->LastError = Imported.Error;

		if (Batch)
		{
			Batch->BilledCharacters += Result.BilledCharacters;
			++Batch->Failed;
		}

		UE_LOG(LogSpeechForge, Error, TEXT("'%s' generated but did not import: %s"),
			*Handle.LineId.ToString(), *Imported.Error);
	}
	else
	{
		Line->Status = ESpeechLineStatus::Generated;
		Line->Origin = ESpeechLineOrigin::Generated;
		Line->ContentHash = ExpectedHash;
		Line->ProviderRequestId = Result.RequestId;
		Line->GeneratedWith = Resolution;
		Line->GeneratedCharacters = Result.BilledCharacters;
		Line->GeneratedAt = FDateTime::UtcNow();
		Line->Alignment = Result.Alignment;
		Line->Sound = Imported.Sound;
		Line->GeneratedSound = Imported.Sound;
		Line->ImportedAudioHash = Imported.AudioHash;
		Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);
		Line->AudioCheckedAt = FDateTime::UtcNow();
		Line->LastError = FString::Join(Imported.Warnings, TEXT(" "));

		if (Batch)
		{
			Batch->BilledCharacters += Result.BilledCharacters;
			++Batch->Succeeded;
		}

		UE_LOG(LogSpeechForge, Log,
			TEXT("'%s' -> %s  (%.3fs, %d Hz, %d chars, delta %.4fs)"),
			*Handle.LineId.ToString(),
			*Import.AssetName,
			Imported.ImportedDurationSeconds,
			Result.SampleRate,
			Result.BilledCharacters,
			Imported.DurationDelta);
	}

	Asset->MarkPackageDirty();

	if (Batch)
	{
		PumpBatch(BatchId);
	}
}

// -------------------------------------------------------------------------------------------------
// Takes - candidates on a ledger
// -------------------------------------------------------------------------------------------------

namespace
{
	/** A take id unique within its line: time-based, suffixed only on collision. */
	FName MakeTakeId(const FSpeechLine& Line)
	{
		const FString Base = FString::Printf(TEXT("T_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		FString Candidate = Base;
		int32 Suffix = 1;
		while (Line.FindTake(FName(*Candidate)) != nullptr)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, ++Suffix);
		}
		return FName(*Candidate);
	}
}

void USpeechForgeSubsystem::GenerateLineTake(const FSpeechLineHandle& Handle, FOnTakeGenerated OnComplete)
{
	const auto Fail = [&OnComplete](const FString& Error)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
		OnComplete(false, FSpeechLineTake(), Error);
	};

	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		return Fail(FString::Printf(TEXT("No line at '%s'."), *Handle.ToString()));
	}

	FSpeechSynthesisRequest Request;
	FString Hash;
	FString Error;
	if (!BuildRequest(Handle, Request, Hash, Error))
	{
		return Fail(Error);
	}

	TSharedPtr<ISpeechProvider> Provider = FindProvider(Request.Voice.ProviderId);
	if (!Provider.IsValid())
	{
		return Fail(TEXT("No provider for this line's resolved voice."));
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FName TakeId = MakeTakeId(*Line);

	const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();
	Request.AbsoluteOutputPath = Staging /
		FString::Printf(TEXT("%s_%s.%s"),
			*FSpeechImporter::SanitizeAssetName(Handle.LineId.ToString()),
			*TakeId.ToString(),
			*Provider->GetAudioFormat());

	const FSpeechVoiceResolution Resolution = Request.Voice;

	// Deliberately no Status = Generating on the line: a detached candidate is not the pipeline
	// touching the line, and the panel must not report a line busy that is not.
	Provider->Synthesize(Request,
		[this, Handle, Hash, Resolution, TakeId, OnComplete](const FSpeechSynthesisResult& Result)
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

		if (!Line)
		{
			OnComplete(false, FSpeechLineTake(), TEXT("The line disappeared while its take generated."));
			return;
		}

		if (!Result.bSuccess)
		{
			UE_LOG(LogSpeechForge, Error, TEXT("Take for '%s' failed: %s"),
				*Handle.LineId.ToString(), *Result.Error);
			OnComplete(false, FSpeechLineTake(), Result.Error);
			return;
		}

		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

		FSpeechImportRequest Import;
		Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
		Import.DestinationPackagePath =
			(Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds")) / TEXT("Takes");
		Import.AssetName = FSpeechImporter::SanitizeAssetName(
			FString::Printf(TEXT("SW_%s_%s"), *Handle.LineId.ToString(), *TakeId.ToString()));
		Import.Alignment = Result.Alignment;
		Import.LineId = Line->LineId;
		Import.SpeakerId = Line->SpeakerId;
		Import.SourceAssetPath = Handle.AssetPath;
		Import.bVerifyDuration = Settings ? Settings->bVerifyAlignmentAgainstDuration : true;
		Import.DurationToleranceSeconds = Settings ? Settings->AlignmentDurationToleranceSeconds : 0.05f;

		const FSpeechImportResult Imported = FSpeechImporter::Import(Import);
		if (!Imported.bSuccess)
		{
			OnComplete(false, FSpeechLineTake(), Imported.Error);
			return;
		}

		FSpeechLineTake Take;
		Take.TakeId = TakeId;
		Take.Kind = ESpeechTakeKind::Generated;
		Take.SoundPath = Imported.Sound.ToString();
		Take.CreatedAt = FDateTime::UtcNow();
		Take.DurationSeconds = Imported.ImportedDurationSeconds;
		Take.ContentHash = Hash;
		Take.ProviderRequestId = Result.RequestId;
		Take.GeneratedWith = Resolution;
		Take.Alignment = Result.Alignment;
		Take.ImportedAudioHash = Imported.AudioHash;
		Take.SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);
		Take.BilledCharacters = Result.BilledCharacters;

		Line->Takes.Add(Take);
		Asset->MarkPackageDirty();
		SaveAsset(Asset);

		UE_LOG(LogSpeechForge, Log, TEXT("Take %s for '%s' -> %s (%.2fs, %d chars billed). Line untouched."),
			*TakeId.ToString(), *Handle.LineId.ToString(), *Take.SoundPath,
			Take.DurationSeconds, Take.BilledCharacters);

		OnComplete(true, Take, FString());
	});
}

FString USpeechForgeSubsystem::RegisterRecordedTake(
	const FSpeechLineHandle& Handle,
	const FString& WavAbsolutePath,
	const FString& TakeDir,
	FString& OutError)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		OutError = FString::Printf(TEXT("No line at '%s'."), *Handle.ToString());
		return FString();
	}

	if (!FPaths::FileExists(WavAbsolutePath))
	{
		OutError = FString::Printf(TEXT("No audio file at '%s'."), *WavAbsolutePath);
		return FString();
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FName TakeId = MakeTakeId(*Line);

	FSpeechImportRequest Import;
	Import.AbsoluteAudioPath = WavAbsolutePath;
	Import.DestinationPackagePath =
		(Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds")) / TEXT("Takes");
	Import.AssetName = FSpeechImporter::SanitizeAssetName(
		FString::Printf(TEXT("SW_%s_%s"), *Handle.LineId.ToString(), *TakeId.ToString()));
	Import.LineId = Line->LineId;
	Import.SpeakerId = Line->SpeakerId;
	Import.SourceAssetPath = Handle.AssetPath;
	Import.bVerifyDuration = false;

	const FSpeechImportResult Imported = FSpeechImporter::Import(Import);
	if (!Imported.bSuccess)
	{
		OutError = Imported.Error;
		return FString();
	}

	FSpeechLineTake Take;
	Take.TakeId = TakeId;
	Take.Kind = ESpeechTakeKind::Recorded;
	Take.SoundPath = Imported.Sound.ToString();
	Take.TakeDir = TakeDir;
	Take.CreatedAt = FDateTime::UtcNow();
	Take.DurationSeconds = Imported.ImportedDurationSeconds;
	Take.ImportedAudioHash = Imported.AudioHash;

	// The script as it read when this was performed. A rewrite after the session leaves the take
	// exactly as valid as it was and exactly as wrong for the new words, and the ledger says so.
	Take.SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);

	Line->Takes.Add(Take);
	Asset->MarkPackageDirty();
	SaveAsset(Asset);

	UE_LOG(LogSpeechForge, Log, TEXT("Recorded take %s registered for '%s' (%.2fs). Line untouched until chosen."),
		*TakeId.ToString(), *Handle.LineId.ToString(), Take.DurationSeconds);

	return TakeId.ToString();
}

TArray<FSpeechLineTake> USpeechForgeSubsystem::GetLineTakes(const FSpeechLineHandle& Handle) const
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	const FSpeechLine* Line = Source ? Source->FindLine(Handle.LineId) : nullptr;
	return Line ? Line->Takes : TArray<FSpeechLineTake>();
}

FName USpeechForgeSubsystem::GetChosenTakeId(const FSpeechLineHandle& Handle) const
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	const FSpeechLine* Line = Source ? Source->FindLine(Handle.LineId) : nullptr;
	return Line ? Line->ChosenTakeId : NAME_None;
}

bool USpeechForgeSubsystem::ApplyLineTake(const FSpeechLineHandle& Handle, FName TakeId, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		OutError = FString::Printf(TEXT("No line at '%s'."), *Handle.ToString());
		return false;
	}

	const FSpeechLineTake* Take = Line->FindTake(TakeId);
	if (!Take)
	{
		OutError = FString::Printf(TEXT("No take '%s' on line '%s'."),
			*TakeId.ToString(), *Handle.LineId.ToString());
		return false;
	}

	if (Take->Kind == ESpeechTakeKind::Recorded)
	{
		// Graduation, exactly as the direct flow: Sound repoints, Origin flips to Recorded, the
		// generated reference survives, and a later script edit reads as stale-plus-Recorded.
		const FSpeechLineTake TakeCopy = *Take;

		// A picked re-voicing is what this take sounds like now, so it is what graduates. The
		// performance, the words and the provenance are the take's either way - only the voice
		// differs - which is why this is a substitution here rather than a separate path.
		FString AudioToApply = TakeCopy.SoundPath;
		const FSpeechTakeAudio* PickedVariant = TakeCopy.ChosenVariantId.IsNone()
			? nullptr
			: TakeCopy.Variants.FindByPredicate([&TakeCopy](const FSpeechTakeAudio& Candidate)
				{ return Candidate.VariantId == TakeCopy.ChosenVariantId; });

		if (PickedVariant)
		{
			AudioToApply = PickedVariant->SoundPath;
		}

		const FString Applied = ApplyRecordedAudio(Handle.AssetPath, Handle.LineId, AudioToApply, OutError);
		if (Applied.IsEmpty())
		{
			return false;
		}

		// ApplyRecordedAudio reloaded and saved the asset; re-find the line before bookkeeping.
		Asset = LoadSourceAsset(Handle.AssetPath);
		Source = AsLineSource(Asset);
		Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
		if (Line)
		{
			Line->ChosenTakeId = TakeId;

			// The take knows how long it is; the line otherwise reports nothing, because a recording
			// arrives with no alignment and nobody measured it on the way in.
			Line->Alignment.DurationSeconds = PickedVariant
				? PickedVariant->DurationSeconds : TakeCopy.DurationSeconds;

			if (PickedVariant)
			{
				Line->GeneratedWith = PickedVariant->Voice;
			}

			Asset->MarkPackageDirty();
			SaveAsset(Asset);
		}
		return true;
	}

	// A generated take applies its stored facts exactly as generation would have written them, so
	// the line is indistinguishable from one generated directly - staleness included.
	USoundWave* Wave = LoadObject<USoundWave>(nullptr, *Take->SoundPath);
	if (!Wave)
	{
		OutError = FString::Printf(TEXT("The take's sound '%s' no longer exists."), *Take->SoundPath);
		return false;
	}

	Line->Status = ESpeechLineStatus::Generated;
	Line->Origin = ESpeechLineOrigin::Generated;
	Line->ContentHash = Take->ContentHash;
	Line->ProviderRequestId = Take->ProviderRequestId;
	Line->GeneratedWith = Take->GeneratedWith;
	Line->GeneratedCharacters = Take->BilledCharacters;
	Line->GeneratedAt = Take->CreatedAt;
	Line->Alignment = Take->Alignment;
	Line->Sound = Wave;
	Line->GeneratedSound = Wave;
	Line->ImportedAudioHash = Take->ImportedAudioHash;

	// A picked re-voicing wins over the take's own recording. Same performance, same words, same
	// provenance - only the voice differs, so nothing else about the take's facts changes.
	if (!Take->ChosenVariantId.IsNone())
	{
		if (const FSpeechTakeAudio* Variant = Take->Variants.FindByPredicate(
			[Take](const FSpeechTakeAudio& Candidate) { return Candidate.VariantId == Take->ChosenVariantId; }))
		{
			if (USoundWave* VariantWave = LoadObject<USoundWave>(nullptr, *Variant->SoundPath))
			{
				Line->Sound = VariantWave;
				Line->GeneratedWith = Variant->Voice;
				Line->Alignment.DurationSeconds = Variant->DurationSeconds;
				Line->ImportedAudioHash.Reset();
			}
		}
	}

	// From the take, never from the line's text as it reads now. A take performed against last
	// week's script must arrive on the line still saying so.
	Line->SpokenTextHash = Take->SpokenTextHash;
	Line->AudioCheckedAt = FDateTime::UtcNow();
	Line->LastError.Reset();
	Line->ChosenTakeId = TakeId;

	Asset->MarkPackageDirty();
	SaveAsset(Asset);

	UE_LOG(LogSpeechForge, Log, TEXT("Take %s is now line '%s'."),
		*TakeId.ToString(), *Handle.LineId.ToString());
	return true;
}

bool USpeechForgeSubsystem::MarkTakeChosen(const FSpeechLineHandle& Handle, FName TakeId)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line || !Line->FindTake(TakeId))
	{
		return false;
	}

	Line->ChosenTakeId = TakeId;
	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

// -------------------------------------------------------------------------------------------------
// Conversion - a performance re-voiced
// -------------------------------------------------------------------------------------------------

void USpeechForgeSubsystem::ConvertLineAudio(
	const FSpeechLineHandle& Handle,
	const FString& SourceAudio,
	FOnLineConverted OnComplete)
{
	const auto Fail = [&OnComplete](const FString& Error)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
		OnComplete(false, Error);
	};

	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
	if (!Line)
	{
		return Fail(FString::Printf(TEXT("No line at '%s'."), *Handle.ToString()));
	}

	// The source is a file on disk, an asset already in the project, or - when neither is given -
	// whatever this line was converted from last time, which is what a re-cast needs.
	FString AbsoluteSource;
	USoundWave* SourceWave = nullptr;

	if (SourceAudio.IsEmpty())
	{
		SourceWave = Line->SourceSound.LoadSynchronous();
		if (!SourceWave)
		{
			return Fail(FString::Printf(
				TEXT("'%s' has no source recording to convert. Give one, or record a take for it."),
				*Handle.LineId.ToString()));
		}
	}
	else if (FPaths::FileExists(SourceAudio))
	{
		AbsoluteSource = SourceAudio;
	}
	else
	{
		SourceWave = LoadObject<USoundWave>(nullptr, *SourceAudio);
		if (!SourceWave)
		{
			return Fail(FString::Printf(TEXT("No audio at '%s' - not a file, not an asset."), *SourceAudio));
		}
	}

	// An imported wave's source file is what the provider needs; there is no sense re-exporting
	// audio the project imported from disk minutes ago.
	if (SourceWave && AbsoluteSource.IsEmpty())
	{
#if WITH_EDITORONLY_DATA
		if (SourceWave->AssetImportData)
		{
			AbsoluteSource = SourceWave->AssetImportData->GetFirstFilename();
		}
#endif
		if (AbsoluteSource.IsEmpty() || !FPaths::FileExists(AbsoluteSource))
		{
			return Fail(FString::Printf(
				TEXT("'%s' has no source file on disk any more, so there is nothing to send. Point at a WAV."),
				*SourceWave->GetName()));
		}
	}

	const FSpeechVoiceResolution Resolution = ResolveVoice(Handle);
	if (!Resolution.IsValid())
	{
		return Fail(FString::Printf(
			TEXT("'%s' resolves to no voice, so there is nothing to convert it into. Cast its speaker."),
			*Handle.LineId.ToString()));
	}

	TSharedPtr<ISpeechProvider> Provider = FindProvider(Resolution.ProviderId);
	if (!Provider.IsValid())
	{
		return Fail(FString::Printf(TEXT("Provider '%s' is not registered."), *Resolution.ProviderId.ToString()));
	}

	if (!Provider->GetCaps().bSupportsVoiceConversion)
	{
		return Fail(FString::Printf(
			TEXT("%s cannot re-voice a recording. Use the performance as-is, or cast a voice on a provider that can."),
			*Provider->GetDisplayName()));
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();

	FSpeechConversionRequest Request;
	Request.AbsoluteSourcePath = AbsoluteSource;
	Request.Voice = Resolution;
	Request.AbsoluteOutputPath = Staging /
		FString::Printf(TEXT("%s_converted_%s.%s"),
			*FSpeechImporter::SanitizeAssetName(Handle.LineId.ToString()),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")),
			*Provider->GetAudioFormat());

	// The identity of what is being converted, captured before the call so the line can be told
	// later whether its source has changed underneath it.
	const FString SourceHash = FSpeechImporter::HashFile(AbsoluteSource);
	const TWeakObjectPtr<USoundWave> SourceAsset = SourceWave;

	// A conversion inherits the provenance of what it converted, because that is what provenance is
	// for: knowing whether a person performed this. Re-voicing our own synthesis produces more
	// synthesis and nothing irreplaceable is at stake. Re-voicing anything else - a recorded take, a
	// WAV off disk - carries a performance that cannot be regenerated, and calling that Generated
	// would let the pipeline overwrite it. Unattributable audio counts as a recording, deliberately:
	// the cautious answer is the safe one here.
	const bool bSourceIsOwnSynthesis =
		SourceWave &&
		Line->Origin == ESpeechLineOrigin::Generated &&
		(SourceWave == Line->Sound.Get() || SourceWave == Line->GeneratedSound.Get());

	UE_LOG(LogSpeechForge, Log, TEXT("Converting '%s' into %s..."),
		*Handle.LineId.ToString(), *Resolution.ProviderVoiceId);

	Provider->ConvertSpeech(Request,
		[this, Handle, Resolution, SourceHash, SourceAsset, bSourceIsOwnSynthesis, OnComplete]
		(const FSpeechSynthesisResult& Result)
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;
		if (!Line)
		{
			OnComplete(false, TEXT("The line disappeared while its audio converted."));
			return;
		}

		if (!Result.bSuccess)
		{
			Line->LastError = Result.Error;
			Asset->MarkPackageDirty();
			UE_LOG(LogSpeechForge, Error, TEXT("Converting '%s' failed: %s"),
				*Handle.LineId.ToString(), *Result.Error);
			OnComplete(false, Result.Error);
			return;
		}

		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

		FSpeechImportRequest Import;
		Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
		Import.DestinationPackagePath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
		Import.AssetName = FSpeechImporter::SanitizeAssetName(
			FString::Printf(TEXT("SW_Converted_%s"), *Handle.LineId.ToString()));
		Import.LineId = Line->LineId;
		Import.SpeakerId = Line->SpeakerId;
		Import.SourceAssetPath = Handle.AssetPath;
		Import.bVerifyDuration = false;

		const FSpeechImportResult Imported = FSpeechImporter::Import(Import);
		if (!Imported.bSuccess)
		{
			Line->LastError = Imported.Error;
			Asset->MarkPackageDirty();
			OnComplete(false, Imported.Error);
			return;
		}

		// Graduation, exactly as a recording graduates: the performance is a person's, and the
		// pipeline must never overwrite it. The generated take stays as the reference it was
		// directed against.
		if (Line->GeneratedSound.IsNull() && !Line->Sound.IsNull() &&
			Line->Origin == ESpeechLineOrigin::Generated)
		{
			Line->GeneratedSound = Line->Sound;
		}

		Line->Sound = Imported.Sound;
		Line->Origin = bSourceIsOwnSynthesis ? ESpeechLineOrigin::Generated : ESpeechLineOrigin::Recorded;
		Line->Status = ESpeechLineStatus::Generated;
		Line->SourceSound = SourceAsset.Get();
		Line->SourceAudioHash = SourceHash;
		Line->ImportedAudioHash = Imported.AudioHash;
		Line->GeneratedWith = Resolution;
		Line->GeneratedAt = FDateTime::UtcNow();

		// The old timings described the synthesised reading, not this performance - same words,
		// different lengths. Wrong subtitle timings are worse than none, so they go rather than
		// quietly drifting out of step with the audio.
		if (Line->Alignment.DurationSeconds > 0.f)
		{
			UE_LOG(LogSpeechForge, Log,
				TEXT("'%s' kept no character timings: a conversion returns none, and the previous "
				     "reading's would describe different audio."),
				*Handle.LineId.ToString());
		}
		Line->Alignment = FSpeechAlignment();
		Line->Alignment.DurationSeconds = Imported.ImportedDurationSeconds;

		// A conversion keeps the source's delivery *and its words*. ContentHash deliberately ignores
		// the text, because re-converting cannot change what was said - but that is exactly why the
		// subtitle question has to be asked separately here, and answered loudly.
		Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);
		Line->AudioCheckedAt = FDateTime::UtcNow();
		Line->ProviderRequestId = Result.RequestId;
		Line->LastError.Reset();

		// Hashed over the source and the voice, never the text - see ComputeConversionHash.
		Line->ContentHash = FSpeechLine::ComputeConversionHash(SourceHash, Resolution);

		Asset->MarkPackageDirty();
		SaveAsset(Asset);

		const FString Message = FString::Printf(
			TEXT("'%s' re-voiced into %s: %s (%.2fs)."),
			*Handle.LineId.ToString(), *Resolution.ProviderVoiceId,
			*Imported.Sound.ToString(), Imported.ImportedDurationSeconds);

		UE_LOG(LogSpeechForge, Log, TEXT("%s"), *Message);
		OnComplete(true, Message);
	});
}

bool USpeechForgeSubsystem::SetTakeVariant(
	const FSpeechLineHandle& Handle, FName TakeId, FName VariantId, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	FSpeechLineTake* Take = Line
		? Line->Takes.FindByPredicate([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; })
		: nullptr;

	if (!Take)
	{
		OutError = FString::Printf(TEXT("'%s' has no take %s."), *Handle.LineId.ToString(), *TakeId.ToString());
		return false;
	}

	if (!VariantId.IsNone() && !Take->Variants.ContainsByPredicate(
		[VariantId](const FSpeechTakeAudio& C) { return C.VariantId == VariantId; }))
	{
		OutError = FString::Printf(TEXT("Take %s has no voice %s."), *TakeId.ToString(), *VariantId.ToString());
		return false;
	}

	Take->ChosenVariantId = VariantId;
	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

bool USpeechForgeSubsystem::RemoveTake(
	const FSpeechLineHandle& Handle, FName TakeId, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	const FSpeechLineTake* Take = Line
		? Line->Takes.FindByPredicate([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; })
		: nullptr;

	if (!Take)
	{
		OutError = FString::Printf(TEXT("'%s' has no take %s."), *Handle.LineId.ToString(), *TakeId.ToString());
		return false;
	}

	// In use is measured, not remembered: whether the line is actually playing this take's audio,
	// its own or one of its voices.
	const FString LineSound = Line->Sound.IsNull() ? FString() : Line->Sound.ToString();
	bool bInUse = !LineSound.IsEmpty() && Take->SoundPath == LineSound;
	for (const FSpeechTakeAudio& Variant : Take->Variants)
	{
		bInUse = bInUse || (!LineSound.IsEmpty() && Variant.SoundPath == LineSound);
	}

	if (bInUse)
	{
		OutError = FString::Printf(
			TEXT("Take %s is the audio '%s' currently plays. Use another take first - removing this "
			     "one would leave the line sounding like a take nothing remembers."),
			*TakeId.ToString(), *Handle.LineId.ToString());
		return false;
	}

	Line->Takes.RemoveAll([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; });

	if (Line->ChosenTakeId == TakeId)
	{
		Line->ChosenTakeId = NAME_None;
	}

	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

bool USpeechForgeSubsystem::RemoveTakeVariant(
	const FSpeechLineHandle& Handle, FName TakeId, FName VariantId, FString& OutError)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	FSpeechLineTake* Take = Line
		? Line->Takes.FindByPredicate([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; })
		: nullptr;

	if (!Take)
	{
		OutError = FString::Printf(TEXT("'%s' has no take %s."), *Handle.LineId.ToString(), *TakeId.ToString());
		return false;
	}

	const int32 Removed = Take->Variants.RemoveAll(
		[VariantId](const FSpeechTakeAudio& C) { return C.VariantId == VariantId; });

	if (Removed == 0)
	{
		OutError = FString::Printf(TEXT("Take %s has no voice %s."), *TakeId.ToString(), *VariantId.ToString());
		return false;
	}

	// The sound asset itself is left alone. Deleting content behind somebody's back is a different
	// and much worse mistake than leaving an unreferenced asset in a folder.
	if (Take->ChosenVariantId == VariantId)
	{
		Take->ChosenVariantId = NAME_None;
	}

	Asset->MarkPackageDirty();
	SaveAsset(Asset);
	return true;
}

void USpeechForgeSubsystem::ConvertTakeAudio(
	const FSpeechLineHandle& Handle, FName TakeId, FOnLineConverted OnComplete)
{
	const auto Fail = [&OnComplete](const FString& Error)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
		OnComplete(false, Error);
	};

	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	const FSpeechLineTake* Take = Line
		? Line->Takes.FindByPredicate([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; })
		: nullptr;

	if (!Take)
	{
		return Fail(FString::Printf(TEXT("'%s' has no take %s."), *Handle.LineId.ToString(), *TakeId.ToString()));
	}

	USoundWave* SourceWave = LoadObject<USoundWave>(nullptr, *Take->SoundPath);
	if (!SourceWave)
	{
		return Fail(FString::Printf(TEXT("Take %s has no audio to re-voice."), *TakeId.ToString()));
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
			TEXT("Take %s has no source file on disk any more, so there is nothing to send."),
			*TakeId.ToString()));
	}

	const FSpeechVoiceResolution Resolution = ResolveVoice(Handle);
	if (!Resolution.IsValid())
	{
		return Fail(FString::Printf(
			TEXT("'%s' resolves to no voice. Cast its speaker before re-voicing a take."),
			*Handle.LineId.ToString()));
	}

	// This take may already have been heard in this exact voice. Converting again would produce a
	// second, indistinguishable entry and charge for it - which is precisely what happened when
	// applying a choice re-entered this function: four identical "Sarah" rows, three of them paid
	// for by accident. A voice is made once per take.
	if (const FSpeechTakeAudio* Existing = Take->Variants.FindByPredicate(
		[&Resolution](const FSpeechTakeAudio& Candidate)
		{ return Candidate.Voice.ToHashString() == Resolution.ToHashString(); }))
	{
		const FName ExistingId = Existing->VariantId;
		const FString ExistingLabel = Existing->Label;

		FString SelectError;
		SetTakeVariant(Handle, TakeId, ExistingId, SelectError);

		const FString Message = FString::Printf(
			TEXT("Take %s already had this voice (%s), so nothing was re-voiced or charged for."),
			*TakeId.ToString(), *ExistingLabel);

		UE_LOG(LogSpeechForge, Log, TEXT("%s"), *Message);
		OnComplete(true, Message);
		return;
	}

	TSharedPtr<ISpeechProvider> Provider = FindProvider(Resolution.ProviderId);
	if (!Provider.IsValid() || !Provider->GetCaps().bSupportsVoiceConversion)
	{
		return Fail(FString::Printf(
			TEXT("%s cannot re-voice a recording."),
			Provider.IsValid() ? *Provider->GetDisplayName() : *Resolution.ProviderId.ToString()));
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();

	const FName VariantId(*FString::Printf(TEXT("V_%s"), *FDateTime::Now().ToString(TEXT("%H%M%S"))));

	FSpeechConversionRequest Request;
	Request.AbsoluteSourcePath = AbsoluteSource;
	Request.Voice = Resolution;
	Request.AbsoluteOutputPath = Staging /
		FString::Printf(TEXT("%s_%s.%s"),
			*FSpeechImporter::SanitizeAssetName(TakeId.ToString()),
			*VariantId.ToString(),
			*Provider->GetAudioFormat());

	// The label the picker shows. A resolution explains itself as "speaker X -> Sarah", and the half
	// after the arrow is the only part a director cares about while listening.
	FString Label = Resolution.SourceDescription;
	int32 Arrow = INDEX_NONE;
	if (Label.FindLastChar(TCHAR('>'), Arrow) && Arrow + 1 < Label.Len())
	{
		Label = Label.RightChop(Arrow + 1).TrimStartAndEnd();
	}
	if (Label.IsEmpty())
	{
		Label = Resolution.ProviderVoiceId;
	}

	UE_LOG(LogSpeechForge, Log, TEXT("Re-voicing take %s into %s..."), *TakeId.ToString(), *Label);

	Provider->ConvertSpeech(Request,
		[this, Handle, TakeId, VariantId, Resolution, Label, OnComplete](const FSpeechSynthesisResult& Result)
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

		FSpeechLineTake* Take = Line
			? Line->Takes.FindByPredicate([TakeId](const FSpeechLineTake& C) { return C.TakeId == TakeId; })
			: nullptr;

		if (!Take)
		{
			OnComplete(false, TEXT("The take disappeared while its audio converted."));
			return;
		}

		if (!Result.bSuccess)
		{
			UE_LOG(LogSpeechForge, Error, TEXT("Re-voicing take %s failed: %s"), *TakeId.ToString(), *Result.Error);
			OnComplete(false, Result.Error);
			return;
		}

		const USpeechForgeSettings* LocalSettings = USpeechForgeSettings::Get();

		FSpeechImportRequest Import;
		Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
		Import.DestinationPackagePath = LocalSettings
			? LocalSettings->GetSoundsPath() / TEXT("Takes")
			: TEXT("/Game/_Generated/Speech/Sounds/Takes");
		Import.AssetName = FSpeechImporter::SanitizeAssetName(
			FString::Printf(TEXT("SW_%s_%s"), *TakeId.ToString(), *VariantId.ToString()));
		Import.LineId = Line->LineId;
		Import.SpeakerId = Line->SpeakerId;
		Import.SourceAssetPath = Handle.AssetPath;
		Import.bVerifyDuration = false;

		const FSpeechImportResult Imported = FSpeechImporter::Import(Import);
		if (!Imported.bSuccess)
		{
			OnComplete(false, Imported.Error);
			return;
		}

		FSpeechTakeAudio Variant;
		Variant.VariantId = VariantId;
		Variant.Label = Label;
		Variant.SoundPath = Imported.Sound.ToString();
		Variant.DurationSeconds = Imported.ImportedDurationSeconds;
		Variant.Voice = Resolution;
		Variant.CreatedAt = FDateTime::UtcNow();

		Take->Variants.Add(MoveTemp(Variant));

		// Picked on arrival, because the only reason to make one is to hear it.
		Take->ChosenVariantId = VariantId;

		Asset->MarkPackageDirty();
		SaveAsset(Asset);

		const FString Message = FString::Printf(
			TEXT("Take %s can now be heard as %s (%.2fs). The take itself is unchanged."),
			*TakeId.ToString(), *Label, Imported.ImportedDurationSeconds);

		UE_LOG(LogSpeechForge, Log, TEXT("%s"), *Message);
		OnComplete(true, Message);
	});
}

// -------------------------------------------------------------------------------------------------
// Graduation
// -------------------------------------------------------------------------------------------------

bool USpeechForgeSubsystem::AcceptCurrentAudio(const FSpeechLineHandle& Handle)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	if (!Line || Line->Status != ESpeechLineStatus::Generated)
	{
		return false;
	}

	// Accepting re-baselines the line, so it has to bank the same hash the status check will later
	// recompute. For a conversion that is the source-and-voice hash; banking a text hash here would
	// hand a blessed line straight back as permanently stale.
	if (!Line->SourceAudioHash.IsEmpty())
	{
		const FSpeechVoiceResolution Resolution = ResolveVoice(Handle);

		Line->ContentHash = FSpeechLine::ComputeConversionHash(Line->SourceAudioHash, Resolution);
		Line->GeneratedWith = Resolution;
		Line->Origin = ESpeechLineOrigin::Accepted;
		Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);

		Asset->MarkPackageDirty();
		SaveAsset(Asset);

		UE_LOG(LogSpeechForge, Log, TEXT("Accepted the converted audio on '%s'."), *Handle.LineId.ToString());
		return true;
	}

	FSpeechSynthesisRequest Request;
	FString Hash;
	FString Error;

	if (!BuildRequest(Handle, Request, Hash, Error))
	{
		UE_LOG(LogSpeechForge, Error, TEXT("%s"), *Error);
		return false;
	}

	Line->ContentHash = Hash;
	Line->GeneratedWith = Request.Voice;
	Line->Origin = ESpeechLineOrigin::Accepted;

	// Accepting is a person saying "these words, this audio, good enough". That settles the subtitle
	// question as much as the regeneration one, so both baselines move together.
	Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);

	Asset->MarkPackageDirty();
	SaveAsset(Asset);

	UE_LOG(LogSpeechForge, Log,
		TEXT("'%s' accepted as-is against the current authoring. Recorded as Accepted rather than ")
		TEXT("Generated, so it stays possible to tell which lines match what they claim."),
		*Handle.LineId.ToString());

	return true;
}

bool USpeechForgeSubsystem::MarkRecorded(const FSpeechLineHandle& Handle, const FString& RecordedSoundPath)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	if (!Line)
	{
		return false;
	}

	USoundWave* Recorded = LoadObject<USoundWave>(nullptr, *RecordedSoundPath);
	if (!Recorded)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("No sound at '%s'."), *RecordedSoundPath);
		return false;
	}

	// Sound moves; GeneratedSound does not. The generated take is the reference the performance was
	// directed against and the thing a production brief carries, and a recorded take can be rejected
	// the same afternoon it lands.
	Line->Sound = Recorded;
	Line->Origin = ESpeechLineOrigin::Recorded;
	Line->ImportedAudioHash.Reset();
	Line->SpokenTextHash = FSpeechLine::ComputeSpokenTextHash(Line->Text);
	Line->AudioCheckedAt = FDateTime::UtcNow();

	Asset->MarkPackageDirty();
	SaveAsset(Asset);

	UE_LOG(LogSpeechForge, Log,
		TEXT("'%s' now plays a recorded take. The generated one is kept as the reference read."),
		*Handle.LineId.ToString());

	return true;
}

int32 USpeechForgeSubsystem::DetectEditedAudio(const TArray<FSpeechLineHandle>& Handles)
{
	int32 Changed = 0;
	TSet<UObject*> Dirty;

	for (const FSpeechLineHandle& Handle : ExpandHandles(Handles))
	{
		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

		if (!Line || Line->Origin != ESpeechLineOrigin::Generated || Line->ImportedAudioHash.IsEmpty())
		{
			continue;
		}

		// The gate that makes this affordable enough to run unprompted. Hashing every sound in a bank
		// means pulling every WAV payload off disk; asking the filesystem when the package was last
		// written costs a stat, and audio inside a file nobody has rewritten cannot have been edited.
		// So the expensive question is only asked where the cheap one already said something moved.
		FString AudioFilename;
		if (FPackageName::DoesPackageExist(Line->Sound.GetLongPackageName(), &AudioFilename))
		{
			const FDateTime Written = IFileManager::Get().GetTimeStamp(*AudioFilename);
			if (Written != FDateTime::MinValue() && Written <= Line->AudioCheckedAt)
			{
				continue;
			}
		}

		USoundWave* Sound = Line->Sound.LoadSynchronous();
		if (!Sound)
		{
			continue;
		}

		// The sound's own imported source data, not the staging file - the staging file is ours and
		// nobody edits it. What can be edited is the asset in the project.
		TArray<uint8> Bytes;
		const uint8* Data = nullptr;
		int32 Size = 0;

#if WITH_EDITORONLY_DATA
		if (Sound->RawData.HasPayloadData())
		{
			const FSharedBuffer Payload = Sound->RawData.GetPayload().Get();
			Data = static_cast<const uint8*>(Payload.GetData());
			Size = static_cast<int32>(Payload.GetSize());
		}
#endif

		if (!Data || Size <= 0)
		{
			continue;
		}

		FSHA1 Sha;
		Sha.Update(Data, Size);
		Sha.Final();

		uint8 Digest[FSHA1::DigestSize];
		Sha.GetHash(Digest);
		const FString Current = BytesToHex(Digest, FSHA1::DigestSize);

		// The stored hash is of the file we imported; this is of what the asset holds now. They will
		// differ the first time this runs against a line imported before this check existed, so a
		// mismatch is only trusted when a hash was recorded in the same shape.
		// Whatever the answer, the file has now been examined at this version, so the next sweep
		// skips it at the stat.
		Line->AudioCheckedAt = FDateTime::UtcNow();
		Dirty.Add(Asset);

		if (Line->ImportedAudioHash.Len() == Current.Len() && Line->ImportedAudioHash != Current)
		{
			Line->Origin = ESpeechLineOrigin::Edited;
			++Changed;

			UE_LOG(LogSpeechForge, Log,
				TEXT("'%s' has been edited since it was generated. Marked as Edited - the pipeline ")
				TEXT("will not overwrite it."),
				*Handle.LineId.ToString());
		}
	}

	for (UObject* Asset : Dirty)
	{
		Asset->MarkPackageDirty();
		SaveAsset(Asset);
	}

	return Changed;
}

bool USpeechForgeSubsystem::RefetchLine(const FSpeechLineHandle& Handle)
{
	UObject* Asset = LoadSourceAsset(Handle.AssetPath);
	ISpeechLineSource* Source = AsLineSource(Asset);
	FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

	if (!Line || Line->ProviderRequestId.IsEmpty())
	{
		UE_LOG(LogSpeechForge, Error,
			TEXT("'%s' has no provider request id, so there is nothing to re-fetch."),
			*Handle.LineId.ToString());
		return false;
	}

	TSharedPtr<ISpeechProvider> Provider = FindProvider(Line->GeneratedWith.ProviderId);
	if (!Provider.IsValid() || !Provider->GetCaps().bSupportsRemoteHistory)
	{
		UE_LOG(LogSpeechForge, Error, TEXT("That provider cannot re-fetch past generations."));
		return false;
	}

	const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
	const FString Staging = Settings ? Settings->GetAbsoluteStagingDirectory() : FPaths::ProjectSavedDir();
	const FString OutputPath = Staging /
		FString::Printf(TEXT("%s_refetch.%s"),
			*FSpeechImporter::SanitizeAssetName(Handle.LineId.ToString()),
			*Provider->GetAudioFormat());

	const FSpeechAlignment KeptAlignment = Line->Alignment;
	const FName LineId = Line->LineId;
	const FName SpeakerId = Line->SpeakerId;

	Provider->RefetchById(Line->ProviderRequestId, OutputPath,
		[this, Handle, KeptAlignment, LineId, SpeakerId](const FSpeechSynthesisResult& Result)
	{
		if (!Result.bSuccess)
		{
			UE_LOG(LogSpeechForge, Error, TEXT("Re-fetch failed: %s"), *Result.Error);
			return;
		}

		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();

		FSpeechImportRequest Import;
		Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
		Import.DestinationPackagePath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
		Import.AssetName = FString::Printf(TEXT("SW_%s"), *LineId.ToString());

		// History returns audio without timings, so the line keeps the alignment it already stored.
		// That is exactly why alignment is stored on the line rather than recomputed on demand.
		Import.Alignment = KeptAlignment;
		Import.LineId = LineId;
		Import.SpeakerId = SpeakerId;
		Import.SourceAssetPath = Handle.AssetPath;

		const FSpeechImportResult Imported = FSpeechImporter::Import(Import);

		UObject* Asset = LoadSourceAsset(Handle.AssetPath);
		ISpeechLineSource* Source = AsLineSource(Asset);
		FSpeechLine* Line = Source ? Source->FindLineMutable(Handle.LineId) : nullptr;

		if (Line && Imported.bSuccess)
		{
			Line->Sound = Imported.Sound;
			Line->GeneratedSound = Imported.Sound;
			Line->ImportedAudioHash = Imported.AudioHash;
			Line->Status = ESpeechLineStatus::Generated;
			Asset->MarkPackageDirty();
			SaveAsset(Asset);

			UE_LOG(LogSpeechForge, Log, TEXT("Re-fetched '%s' from history. Free, and identical."),
				*LineId.ToString());
		}
	});

	return true;
}

bool USpeechForgeSubsystem::CancelBatch(const FString& BatchId)
{
	if (FSpeechBatch* Batch = Batches.Find(BatchId))
	{
		// Requests already sent keep running and are still paid for. Only the queue stops.
		Batch->bCancelled = true;
		Batch->Pending.Empty();
		return true;
	}

	return false;
}

void USpeechForgeSubsystem::TestConnectionAsync(
	FName ProviderId, TFunction<void(bool, const FString&)> OnComplete)
{
	TSharedPtr<ISpeechProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, FString::Printf(TEXT("No provider named '%s'."), *ProviderId.ToString()));
		return;
	}

	Provider->TestConnection(MoveTemp(OnComplete));
}

void USpeechForgeSubsystem::ListProviderVoices(
	FName ProviderId,
	TFunction<void(bool, const TArray<FSpeechRemoteVoiceInfo>&, const FString&)> OnComplete)
{
	TSharedPtr<ISpeechProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, {}, FString::Printf(TEXT("No provider named '%s'."), *ProviderId.ToString()));
		return;
	}

	Provider->ListVoices(
		[OnComplete](bool bSuccess, const TArray<FSpeechRemoteVoice>& Voices, const FString& Error)
	{
		TArray<FSpeechRemoteVoiceInfo> Converted;
		Converted.Reserve(Voices.Num());

		for (const FSpeechRemoteVoice& Voice : Voices)
		{
			FSpeechRemoteVoiceInfo Info;
			Info.Id = Voice.Id;
			Info.Name = Voice.Name;
			Info.Description = Voice.Description;
			Info.bIsPremade = Voice.bIsPremade;
			Converted.Add(MoveTemp(Info));
		}

		OnComplete(bSuccess, Converted, Error);
	});
}

void USpeechForgeSubsystem::TestConnection(FName ProviderId)
{
	TSharedPtr<ISpeechProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		UE_LOG(LogSpeechForge, Error, TEXT("No provider named '%s'."), *ProviderId.ToString());
		return;
	}

	const FName Resolved = Provider->GetProviderId();
	Provider->TestConnection([Resolved](bool bSuccess, const FString& Message)
	{
		if (bSuccess)
		{
			UE_LOG(LogSpeechForge, Log, TEXT("%s: %s"), *Resolved.ToString(), *Message);
		}
		else
		{
			UE_LOG(LogSpeechForge, Error, TEXT("%s: %s"), *Resolved.ToString(), *Message);
		}
	});
}
