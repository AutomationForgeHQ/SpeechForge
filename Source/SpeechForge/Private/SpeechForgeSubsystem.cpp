#include "SpeechForgeSubsystem.h"

#include "SpeechForge.h"
#include "SpeechForgeSettings.h"
#include "SpeechCredentialStore.h"
#include "SpeechSources.h"
#include "SpeechBank.h"
#include "SpeechLineDef.h"
#include "SpeechVoice.h"
#include "SpeechImporter.h"
#include "ISpeechProvider.h"
#include "Providers/ElevenLabsProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Sound/SoundWave.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "Editor.h"

void USpeechForgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The first provider is registered by the capability plugin itself, exactly as MotionForge does
	// with its own. Additional providers are separate plugins that register themselves and that this
	// module never learns the names of.
	if (FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr())
	{
		if (!Module->FindProvider(FElevenLabsProvider::ProviderId).IsValid())
		{
			Module->RegisterProvider(MakeShared<FElevenLabsProvider>());
		}
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
		const USpeechForgeSettings* Settings = USpeechForgeSettings::Get();
		ProviderId = Settings ? Settings->DefaultProviderId : NAME_None;
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

FString USpeechForgeSubsystem::CreateVoice(
	const FString& AssetPath,
	FName SpeakerId,
	const FString& ProviderVoiceId,
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
		ObjectName = FSpeechImporter::SanitizeAssetName(FString::Printf(TEXT("SV_%s"), *SpeakerId.ToString()));
		PackageName = Settings->GetVoicesPath() / ObjectName;
	}
	else
	{
		PackageName.Split(TEXT("."), &PackageName, nullptr);
		ObjectName = FPackageName::GetShortName(PackageName);
	}

	USpeechVoice* Voice = LoadObject<USpeechVoice>(nullptr, *(PackageName + TEXT(".") + ObjectName));

	if (!Voice)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FString();
		}

		Voice = NewObject<USpeechVoice>(Package, *ObjectName, RF_Public | RF_Standalone);

		FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistry.Get().AssetCreated(Voice);
	}

	Voice->SpeakerId = SpeakerId;
	Voice->ProviderVoiceId = ProviderVoiceId;
	Voice->ProviderId = ProviderId.IsNone() ? Settings->DefaultProviderId : ProviderId;
	Voice->ModelId = ModelId;
	Voice->Provenance = Provenance;
	Voice->PairedAt = FDateTime::UtcNow();

	Voice->MarkPackageDirty();
	SaveAsset(Voice);

	if (Provenance == ESpeechVoiceProvenance::Premade)
	{
		// Said once, loudly, at the moment it can still be acted on cheaply. A stock voice can be
		// retired by its provider and takes every line generated against it when it goes.
		UE_LOG(LogSpeechForge, Warning,
			TEXT("'%s' is paired with a stock provider voice. Those can be retired by the provider, ")
			TEXT("which would take every line generated against it. Design or clone a voice before ")
			TEXT("building a library on this one."),
			*ObjectName);
	}

	return Voice->GetPathName();
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

	// 1. The line's own override. Explicit beats everything, so a single awkward line can always be
	//    re-cast without disturbing anything else.
	if (!Line->VoiceOverride.IsNull())
	{
		if (USpeechVoice* Voice = Line->VoiceOverride.LoadSynchronous())
		{
			return Voice->MakeResolution(Settings->DefaultProviderId, FallbackModel,
				FString::Printf(TEXT("line override -> %s"), *Voice->GetName()));
		}
	}

	// 2. Registered external sources, highest priority first. This is where an adapter plugin maps a
	//    speaker onto a voice from wherever it likes, without SpeechForge knowing it exists.
	if (FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr())
	{
		FSpeechVoiceQuery Query;
		Query.SpeakerId = SpeakerId;
		Query.LineId = Line->LineId;
		Query.SourceAssetPath = Handle.AssetPath;

		for (const TSharedPtr<ISpeechVoiceSource>& VoiceSource : Module->GetVoiceSources())
		{
			if (!VoiceSource.IsValid())
			{
				continue;
			}

			FSpeechVoiceResolution FromSource;
			if (VoiceSource->ResolveVoice(Query, FromSource) && FromSource.IsValid())
			{
				if (FromSource.ModelId.IsEmpty())
				{
					FromSource.ModelId = FallbackModel;
				}
				if (FromSource.ProviderId.IsNone())
				{
					FromSource.ProviderId = Settings->DefaultProviderId;
				}
				return FromSource;
			}
		}
	}

	// 3. The container's default.
	if (!Defaults.Voice.IsNull())
	{
		if (USpeechVoice* Voice = Defaults.Voice.LoadSynchronous())
		{
			return Voice->MakeResolution(Settings->DefaultProviderId, FallbackModel,
				FString::Printf(TEXT("asset default -> %s"), *Voice->GetName()));
		}
	}

	// 4. The project default. Mostly useful while prototyping.
	if (!Settings->DefaultVoice.IsNull())
	{
		if (USpeechVoice* Voice = Settings->DefaultVoice.LoadSynchronous())
		{
			return Voice->MakeResolution(Settings->DefaultProviderId, FallbackModel,
				FString::Printf(TEXT("project default -> %s"), *Voice->GetName()));
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

		// Staleness is computed, never stored. Storing it would mean invalidating every line whenever
		// any voice asset anywhere changed, and getting that wrong is silent by construction.
		if (Line->Status == ESpeechLineStatus::Generated && !Line->ContentHash.IsEmpty())
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
					Status.StaleReason = VoiceDiff;
				}
				else
				{
					// Nothing about the voice moved, so it was the words. Worth distinguishing,
					// because one is a re-record and the other is a re-read.
					Status.StaleReason = TEXT("text or direction changed");
				}

				if (Line->Origin == ESpeechLineOrigin::Recorded)
				{
					// The report with money attached: a pickup session, scoped exactly.
					Status.StaleReason += TEXT(" - line was RECORDED, so this needs a pickup, not a regeneration");
				}
			}
		}

		Report.Add(MoveTemp(Status));
	}

	return Report;
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

	FSpeechImportRequest Import;
	Import.AbsoluteAudioPath = Result.AbsoluteAudioPath;
	Import.DestinationPackagePath = Settings ? Settings->GetSoundsPath() : TEXT("/Game/_Generated/Speech/Sounds");
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
		if (Line->ImportedAudioHash.Len() == Current.Len() && Line->ImportedAudioHash != Current)
		{
			Line->Origin = ESpeechLineOrigin::Edited;
			Dirty.Add(Asset);
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
