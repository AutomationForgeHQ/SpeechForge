// The whole pipeline, as a scriptable API.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SpeechForgeTypes.h"
#include "SpeechLine.h"
#include "SpeechForgeSubsystem.generated.h"

class ISpeechProvider;
class ISpeechLineSource;
class USpeechBank;
class USpeechVoice;

/** A group of lines moving through the pipeline together. */
struct FSpeechBatch
{
	FString BatchId;

	/** Not yet started. Drained as slots free up. */
	TArray<FSpeechLineHandle> Pending;

	int32 InFlight = 0;
	int32 MaxConcurrent = 1;

	int32 Succeeded = 0;
	int32 Failed = 0;
	int32 Total = 0;

	int32 BilledCharacters = 0;

	double StartedAt = 0.0;
	bool bCancelled = false;

	/** Assets touched, so each is saved once at the end rather than after every line. */
	TSet<FString> DirtyAssets;
};

/** How far a batch got. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechBatchStatus
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	FString BatchId;

	/** False when the id is unknown, which is also what a finished batch looks like. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	bool bTracked = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	bool bFinished = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	int32 Total = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	int32 Succeeded = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	int32 Failed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	int32 InFlight = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Batch")
	int32 BilledCharacters = 0;
};

/**
 * Everything SpeechForge can do, callable from C++, Blueprint, Python and therefore MCP.
 *
 * The editor UI, when there is one, is a thin layer over this and holds no logic of its own. If a
 * capability only exists behind a button, an agent cannot use it, and a pipeline that needs a human
 * to click things is not a pipeline.
 *
 * Four rules every function here keeps:
 *
 *   Idempotent      Re-running an operation already underway does nothing. Money is spent at
 *                   submission, so a retry after a timeout must not be able to pay twice.
 *   Non-blocking    Generation returns a batch id immediately and is polled. The editor never stalls.
 *   Structured      Status and errors come back as USTRUCTs, not log lines.
 *   Costed first    EstimateGenerationCost is exact arithmetic, needs no network call, and can be
 *                   asked before anything exists - which is the only moment the decision to spend
 *                   can still be made.
 */
UCLASS()
class SPEECHFORGE_API USpeechForgeSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static USpeechForgeSubsystem* Get();

	// ---------------------------------------------------------------------------------------------
	// Authoring
	// ---------------------------------------------------------------------------------------------

	/**
	 * Create a speech bank, or add to one that already exists.
	 *
	 * @param AssetPath Content path. Empty creates under the configured output path using BankName.
	 * @return content path of the bank, or empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring", meta = (AFNode = "Prepare/Speech", AFShape = "map", AFGrain = "dialogue", AFIdempotent))
	FString CreateOrUpdateBank(
		const FString& AssetPath,
		const FString& BankName,
		const TArray<FSpeechLineSpec>& Lines,
		FName DefaultSpeakerId);

	/**
	 * Create a voice asset pairing a speaker with a provider voice.
	 *
	 * Deliberately does not create anything on the provider. Casting a voice is a human act performed
	 * once, and a tool that could create voices could generally delete them.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	FString CreateVoice(
		const FString& AssetPath,
		FName SpeakerId,
		const FString& ProviderVoiceId,
		FName ProviderId,
		const FString& ModelId,
		ESpeechVoiceProvenance Provenance);

	/** Speech banks and single-line assets in the project. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	TArray<FString> FindSpeechAssets() const;

	// ---------------------------------------------------------------------------------------------
	// Resolution
	// ---------------------------------------------------------------------------------------------

	/**
	 * Work out which voice a line is actually spoken in.
	 *
	 * Consulted in this order, first answer wins:
	 *
	 *   1. the line's own VoiceOverride
	 *   2. registered external sources, highest priority first - an NPC definition, a casting table
	 *   3. the container's default voice
	 *   4. the project default
	 *
	 * The result is concrete. Nothing downstream ever works on the reference, because a reference
	 * resolved somewhere else can change without anything here noticing.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Resolution", meta = (AFNode = "Prepare/Speech", AFShape = "map", AFGrain = "take", AFIdempotent))
	FSpeechVoiceResolution ResolveVoice(const FSpeechLineHandle& Handle) const;

	// ---------------------------------------------------------------------------------------------
	// Observation
	// ---------------------------------------------------------------------------------------------

	/**
	 * Status, origin and staleness for lines.
	 *
	 * A handle with no LineId means every line in that asset. An empty array means every line in the
	 * project.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status", meta = (AFNode = "Report/Speech", AFShape = "map", AFGrain = "take", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	TArray<FSpeechLineStatus> GetLineStatus(const TArray<FSpeechLineHandle>& Handles) const;

	/**
	 * Where a batch has got to.
	 *
	 * An id nobody is tracking reports finished rather than failing, which is right - a batch the editor
	 * has forgotten across a restart is not still running - but it does mean a poll asking about an
	 * empty id says *done* straight away.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status", meta = (AFNode = "Report/Speech", AFShape = "map", AFGrain = "take", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	FSpeechBatchStatus GetBatchStatus(const FString& BatchId) const;

	/**
	 * What generating these lines would cost.
	 *
	 * Exact rather than projected, and computed with no network call: billing is per character of
	 * input, so the number is knowable before anything is submitted. Lines that are current,
	 * graduated or already in flight are counted as skipped rather than priced.
	 *
	 * @param bIncludeCurrent Price everything, including lines that would not actually regenerate.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status", meta = (AFNode = "Estimate/Speech", AFShape = "reduce", AFGrain = "dialogue", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	FSpeechCostEstimate EstimateGenerationCost(
		const TArray<FSpeechLineHandle>& Handles,
		bool bIncludeCurrent) const;

	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status")
	FSpeechCredentialInfo GetCredentialInfo(FName ProviderId) const;

	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status")
	FSpeechProviderCaps GetProviderCaps(FName ProviderId) const;

	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status")
	TArray<FName> GetProviderIds() const;

	// ---------------------------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------------------------

	/**
	 * Generate audio for lines that need it.
	 *
	 * Silently skips anything current, already in flight, or whose audio the pipeline no longer owns.
	 * That single rule does three jobs: no double charge on a retry, no drift across a bank from
	 * regenerating lines that did not change, and no overwriting of hand-authored or recorded work.
	 *
	 * **Starts work; does not finish it.** The provider takes minutes for a bank of any size, so this
	 * returns as soon as the batch is submitted. A pipeline step using it emits the batch id and waits
	 * on Get Batch Status - anything reading the audio before then reads a line that has none yet.
	 *
	 * @param bForce Regenerate even lines that are current. Does **not** override graduation.
	 * @return batch id, or empty when nothing was eligible.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline",
		meta = (AFNode = "Generate/Speech", AFShape = "map", AFGrain = "take",
			AFProviderDependent, AFRetryable, AFCostPerItem = "0.0116", AFAsync,
			AFStatusNode = "SpeechForgeSubsystem:GetBatchStatus",
			AFDoneWhen = "bFinished=True"))
	FString GenerateLines(const TArray<FSpeechLineHandle>& Handles, bool bForce);

	/**
	 * Fetch a line's existing audio again from the provider, without regenerating it.
	 *
	 * Free where the provider keeps history, and the only way to recover a take that a seed cannot
	 * reproduce. Use this rather than regenerating when the asset was lost but the line still knows
	 * its request id.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline")
	bool RefetchLine(const FSpeechLineHandle& Handle);

	/**
	 * Accept the audio a line already has across an authoring change, without regenerating.
	 *
	 * The pressure valve for a voice tweak that marks a whole cast stale and is not worth the money
	 * or the re-rolled performances. Records the line as Accepted rather than quietly re-stamping the
	 * hash, so it stays possible to tell later which lines were generated from what they claim.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline")
	bool AcceptCurrentAudio(const FSpeechLineHandle& Handle);

	/**
	 * Point a line at an authored take - an actor's recording, or any better source.
	 *
	 * The generated audio is kept, not replaced. It is the reference the performance was directed
	 * against and the thing a production brief has to carry, and a recorded take can be rejected the
	 * same afternoon it arrives.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline")
	bool MarkRecorded(const FSpeechLineHandle& Handle, const FString& RecordedSoundPath);

	/**
	 * Find lines whose audio has been hand-edited since it was imported, and mark them.
	 *
	 * Rule one says a generated asset that gets touched has graduated. Nobody remembers to say so, so
	 * this detects it instead - and the failure it closes is a levelled or de-essed file being
	 * overwritten weeks later without either party noticing.
	 *
	 * @return how many lines changed origin.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline")
	int32 DetectEditedAudio(const TArray<FSpeechLineHandle>& Handles);

	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Pipeline")
	bool CancelBatch(const FString& BatchId);

	/** Make one cheap authenticated call and log what happened. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status")
	void TestConnection(FName ProviderId);

	/** Cheapest authenticated call, reported rather than logged. */
	void TestConnectionAsync(FName ProviderId, TFunction<void(bool, const FString&)> OnComplete);

	/** Voices the account holds, so one can be paired without leaving the editor. */
	void ListProviderVoices(
		FName ProviderId,
		TFunction<void(bool, const TArray<FSpeechRemoteVoiceInfo>&, const FString&)> OnComplete);

	TSharedPtr<ISpeechProvider> FindProvider(FName ProviderId) const;

private:

	/** Load a bank or single-line asset and return it as a line source. */
	UObject* LoadSourceAsset(const FString& AssetPath) const;
	ISpeechLineSource* AsLineSource(UObject* Asset) const;

	/** A handle with no LineId on a multi-line asset becomes one handle per line. */
	TArray<FSpeechLineHandle> ExpandHandles(const TArray<FSpeechLineHandle>& Handles) const;

	/** Build the request a line would send right now, and its hash. */
	bool BuildRequest(
		const FSpeechLineHandle& Handle,
		struct FSpeechSynthesisRequest& OutRequest,
		FString& OutHash,
		FString& OutError) const;

	/** Start as many pending lines as the concurrency limit allows. */
	void PumpBatch(const FString& BatchId);

	void OnLineSynthesized(
		const FString& BatchId,
		const FSpeechLineHandle& Handle,
		const struct FSpeechSynthesisResult& Result,
		const FString& ExpectedHash,
		const FSpeechVoiceResolution& Resolution);

	static void SaveAsset(UObject* Asset);
	static FString MakeBatchId();

	TMap<FString, FSpeechBatch> Batches;
};
