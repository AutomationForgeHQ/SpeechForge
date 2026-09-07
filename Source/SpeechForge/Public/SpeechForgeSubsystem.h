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
class USpeechSpeaker;
class USpeechVoiceProfile;

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
	 * Create or update a voice profile - the instrument: provider, preset, model, settings.
	 *
	 * Deliberately does not create anything on the provider. Making a voice exist is a human act
	 * performed once in the provider's interface, and a tool that could create voices could
	 * generally delete them. This records one that exists.
	 *
	 * @param AssetPath Content path. Empty creates under the configured voices folder as
	 *        VP_<DisplayName>.
	 * @return content path of the profile, or empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	FString CreateVoiceProfile(
		const FString& AssetPath,
		const FString& DisplayName,
		const FString& ProviderVoiceId,
		const FString& ProviderVoiceName,
		FName ProviderId,
		const FString& ModelId,
		ESpeechVoiceProvenance Provenance);

	/**
	 * Create or update a speaker's character sheet - the first step of any scene.
	 *
	 * Finds the existing sheet by SpeakerId wherever it lives; creates one under the configured
	 * speakers folder when there is none. Empty DisplayName/Description/VoiceProfilePath leave the
	 * existing values alone, so an adapter can seed identity without clobbering a cast voice.
	 *
	 * @return content path of the speaker asset, or empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	FString CreateOrUpdateSpeaker(
		FName SpeakerId,
		const FString& DisplayName,
		const FString& Description,
		const FString& VoiceProfilePath);

	/**
	 * Record on a speaker's sheet what they are in another system.
	 *
	 * The adapter seam: e.g. key "NarrativePro", path of an NPCDefinition. Core stores the path and
	 * attaches no meaning to the key. Creates the sheet if the speaker has none yet.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	bool SetSpeakerBinding(FName SpeakerId, FName BindingKey, const FString& ObjectPath);

	/** The speaker asset for an id, or empty. Registry lookup; loads nothing else. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	FString FindSpeakerAssetPath(FName SpeakerId) const;

	/** Every speaker sheet in the project. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	TArray<FString> FindSpeakerAssets() const;

	/** Every voice profile in the project. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	TArray<FString> FindVoiceProfiles() const;

	/** The profile recorded for this provider voice, or empty. Prevents duplicate profiles per preset. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	FString FindVoiceProfileByProviderVoice(FName ProviderId, const FString& ProviderVoiceId) const;

	/**
	 * Edit a line's authoring fields in place: text, direction, speaker.
	 *
	 * SpeakerId None leaves the speaker alone. Pipeline state is untouched - an edit that changes
	 * what is spoken shows up as stale, never as silently regenerated.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	bool UpdateLineAuthoring(
		const FSpeechLineHandle& Handle,
		const FString& Text,
		const FString& Direction,
		FName SpeakerId);

	/**
	 * Re-cast one line by hand: set or clear its voice override.
	 *
	 * The exception flow. Empty VoiceProfilePath clears the override, returning the line to its
	 * speaker's voice.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Casting")
	bool SetLineVoiceOverride(const FSpeechLineHandle& Handle, const FString& VoiceProfilePath);

	/**
	 * Stamp a bank with where its lines came from: the adapter's key and the source asset.
	 *
	 * Called by adapters at harvest. The stamp is what makes two-way sync offerable - the panel
	 * matches sync tools to banks by this key - and what lets an un-synced edit be warned about
	 * rather than silently overwritten by the next harvest.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	bool SetBankSource(
		const FString& BankPath,
		FName SourceAdapter,
		const FString& SourceAssetPath,
		const FString& SpeakerFilter = FString());

	/**
	 * Which bank is home for each of these line ids - the one-line-one-home contract's question.
	 *
	 * Identity is (LineId, LanguageCode): only banks of the given language are consulted, and the
	 * bank at IgnoreBankPath is skipped, because re-ingesting into a line's own home is the update
	 * path and not a duplicate. Answered from the LineIdIndex registry tag where a bank has been
	 * saved with one; older banks are loaded and read the slow way, once - their next save stamps
	 * the index.
	 *
	 * @return LineId -> content path of its home bank, holding only the ids homed elsewhere.
	 */
	TMap<FName, FString> FindLineHomes(
		const TArray<FName>& LineIds,
		const FString& LanguageCode,
		const FString& IgnoreBankPath) const;

	/**
	 * Remove lines from a bank, by id.
	 *
	 * The lines' generated audio assets stay in the project: a bank entry is a reference, and
	 * deleting content is a human's call in the Content Browser. Ids not present are ignored, so
	 * the call is idempotent.
	 *
	 * @return How many lines were removed. Zero with OutError set when the bank cannot be edited.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	int32 RemoveBankLines(const FString& BankPath, const TArray<FName>& LineIds, FString& OutError);

	/**
	 * Remove every line from a bank, keeping its defaults, language and source stamp.
	 *
	 * The clean-slate half of replacing a bank's source: clear, then ingest the new one. Audio
	 * assets stay, as RemoveBankLines leaves them.
	 *
	 * @return How many lines were removed.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Authoring")
	int32 ClearBankLines(const FString& BankPath, FString& OutError);

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
	 *   1. the line's own VoiceOverride - the by-hand exception
	 *   2. the speaker's sheet - USpeechSpeaker matched on SpeakerId, its VoiceProfile
	 *   3. the container's default profile
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
	 * Every bank whose source script has been written since the bank last read it.
	 *
	 * The one drift nothing else can see. A line edited in its own dialogue editor never reaches the
	 * bank, so the audio, the subtitle and the script quietly disagree until somebody re-harvests -
	 * and nobody re-harvests a bank they have no reason to suspect.
	 *
	 * Deliberately built to be cheap enough to run unprompted: the asset registry names every bank
	 * from its cache, banks with no adapter are dismissed without being loaded, and a file time
	 * decides whether the rest is worth opening. It reports suspicion, not proof - a resaved
	 * dialogue with no text change lands here too, and re-harvesting is how you find out which.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status", meta = (AFNode = "Report/SpeechDrift", AFShape = "map", AFGrain = "project", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	TArray<FSpeechSourceDrift> CheckSourceDrift() const;

	/**
	 * Bring the Speech Library up on one bank, and optionally one line.
	 *
	 * Navigation, which the pipeline had none of: a line reached from a recording session, a face
	 * clip or an agent could only be found again by remembering which bank it was in and searching
	 * for it by hand. Panels in other plugins call this by reflection, so nothing links to a widget.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Navigation")
	void OpenLibraryAt(const FString& BankPath, FName LineId);

	/** Raised by OpenLibraryAt. The Speech Library listens; nothing else needs to know it exists. */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLibraryFocusRequested, const FString& /*BankPath*/, FName /*LineId*/);
	FOnLibraryFocusRequested OnLibraryFocusRequested;

	/**
	 * Raised whenever a speech asset is written - a generation, a conversion, a chosen take, a
	 * recording applied from another plugin entirely.
	 *
	 * Panels showing lines cannot know when the thing they are describing changed, and a panel that
	 * caches what it drew keeps showing the previous voice until somebody navigates away and back.
	 * That is not a stale list, it is a wrong answer about which audio a line plays.
	 */
	DECLARE_MULTICAST_DELEGATE(FOnSpeechLibraryChanged);
	FOnSpeechLibraryChanged OnLibraryChanged;

	/**
	 * What a bank was harvested from: "<adapter>|<asset path>", or empty when it is its own source.
	 *
	 * Deliberately one string rather than a struct: it crosses a reflection boundary to panels in
	 * other plugins, and a struct cannot.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Navigation")
	FString GetBankSourceDescription(const FString& BankPath) const;

private:

	/** Runs the sweep once the asset registry has finished its first scan, and logs what it found. */
	void ReportSourceDriftOnStartup();

public:

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

	/**
	 * What re-voicing these lines would cost.
	 *
	 * The other meter. Conversion bills by duration of audio, so this measures sources rather than
	 * counting characters - and unlike generation it can only be exact once the audio exists. A
	 * line whose source cannot be measured is reported in `UnpricedCount` and priced at nothing,
	 * rather than being guessed at from its text: a conversion is charged for a performance, and
	 * the same sentence can be five seconds or fifteen.
	 *
	 * @param SourceAudio A sound asset path or an absolute WAV path applied to every handle. Leave
	 *        empty to measure whatever each line was converted from before, which is what pricing a
	 *        re-cast needs.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Status", meta = (AFNode = "Estimate/SpeechConversion", AFShape = "reduce", AFGrain = "dialogue", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	FSpeechCostEstimate EstimateConversionCost(
		const TArray<FSpeechLineHandle>& Handles,
		const FString& SourceAudio) const;

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

	/** Expand bank-level handles into per-line handles. A handle with LineId None means every line. */
	TArray<FSpeechLineHandle> ExpandHandles(const TArray<FSpeechLineHandle>& Handles) const;

	// ---------------------------------------------------------------------------------------------
	// Takes - candidates on a ledger
	// ---------------------------------------------------------------------------------------------

	using FOnTakeGenerated = TFunction<void(bool /*bSuccess*/, const FSpeechLineTake& /*Take*/, const FString& /*Error*/)>;

	/**
	 * Generate one detached candidate for a line: synthesized, imported as its own asset under
	 * Sounds/Takes, appended to the line's take ledger - and the line itself untouched. Spends
	 * money like any generation. Choosing (ApplyLineTake) is what makes a candidate the line.
	 */
	void GenerateLineTake(const FSpeechLineHandle& Handle, FOnTakeGenerated OnComplete);

	/**
	 * Put a recorded performance on the line's take ledger without applying it: the WAV is imported
	 * as its own asset, the capture-take folder is remembered for the video side.
	 *
	 * @return The new take's id, or empty with OutError set.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	FString RegisterRecordedTake(
		const FSpeechLineHandle& Handle,
		const FString& WavAbsolutePath,
		const FString& TakeDir,
		FString& OutError);

	/** The line's take ledger. Empty for lines in the default overwrite-in-place flow. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	TArray<FSpeechLineTake> GetLineTakes(const FSpeechLineHandle& Handle) const;

	/** Which take is marked chosen on the line, or None. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	FName GetChosenTakeId(const FSpeechLineHandle& Handle) const;

	/**
	 * Make a take the line. A generated take applies its stored facts exactly as generation would
	 * have written them; a recorded take goes through graduation (ApplyRecordedAudio). Either way
	 * the line ends indistinguishable from the direct flow, hashes included, and re-choosing a
	 * different take later is just another apply.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	bool ApplyLineTake(const FSpeechLineHandle& Handle, FName TakeId, FString& OutError);

	/**
	 * Bookkeeping only: record which take is the chosen one without applying anything. For the
	 * immediate-processing flow, where the finisher already applied the audio and the ledger
	 * should still say which take it was.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	bool MarkTakeChosen(const FSpeechLineHandle& Handle, FName TakeId);

	using FOnLineConverted = TFunction<void(bool /*bSuccess*/, const FString& /*Message*/)>;

	/**
	 * Re-voice a recording into the line's cast voice, and make it the line's audio.
	 *
	 * The line-level half of speech-to-speech: a performance keeps its delivery and changes its
	 * identity. The result graduates exactly as a recording does - `Origin` becomes `Recorded`, the
	 * generated take survives as the reference it was directed against - because a converted
	 * performance is still a performance, and the pipeline must never overwrite it.
	 *
	 * Staleness afterwards is computed over the **source audio and the voice**, never the text: a
	 * rewritten script cannot change a recording, while replacing the source or re-casting the
	 * speaker changes everything.
	 *
	 * **Spends money at the moment it is sent**, billed by duration rather than by character.
	 *
	 * @param SourceAudio A sound asset's content path, or the absolute path of a WAV on disk.
	 *        Empty re-converts from the line's existing SourceSound.
	 */
	/**
	 * Re-voice one take, as another voice for that same performance.
	 *
	 * Not a new take. A conversion keeps the delivery, the timing and the face that were captured
	 * once, and changes only who it sounds like - so it belongs under the take it came from, where a
	 * director can play the picture against either. Listing it as a sibling would mean a ledger of
	 * "candidates" containing two rows that are the same moment, which is how a ledger stops being
	 * read at all.
	 *
	 * The new voice is picked on arrival, so playing the take immediately plays the conversion.
	 */
	void ConvertTakeAudio(const FSpeechLineHandle& Handle, FName TakeId, FOnLineConverted OnComplete);

	/**
	 * Choose which of a take's voices wins if that take is chosen. None restores its own recording.
	 *
	 * Stored on the take rather than passed to the choose call, so picking a voice while reviewing
	 * and pressing Choose afterwards do the obvious thing, and the decision outlives the panel.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	bool SetTakeVariant(const FSpeechLineHandle& Handle, FName TakeId, FName VariantId, FString& OutError);

	/**
	 * Drop one re-voicing from a take. The sound asset is left where it is.
	 *
	 * For tidying a ledger that accumulated voices nobody wants - not for freeing space. Deleting
	 * the audio too would be a destructive act performed on a list-tidying gesture.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	bool RemoveTakeVariant(const FSpeechLineHandle& Handle, FName TakeId, FName VariantId, FString& OutError);

	/**
	 * Drop a whole take from a line's ledger.
	 *
	 * Refused while the line is playing that take's audio: removing the row would leave the line
	 * sounding like a take nothing remembers, which is the drift this pipeline exists to prevent.
	 * Choose another take first.
	 *
	 * The imported sound assets are left in the project. Whoever is clearing takes is freeing the
	 * capture library, and silently deleting content that other assets may reference is a different
	 * and much worse act than tidying a list.
	 */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Takes")
	bool RemoveTake(const FSpeechLineHandle& Handle, FName TakeId, FString& OutError);

	void ConvertLineAudio(
		const FSpeechLineHandle& Handle,
		const FString& SourceAudio,
		FOnLineConverted OnComplete);

	// ---------------------------------------------------------------------------------------------
	// Localisation - a sibling bank per language, translated line by line
	//
	// The layer model already decided the shape (see FaceForge/GRADUATION.md §4): a language swap
	// replaces exactly one thing - the words, and with them the audio and the mouth. Everything
	// else joins by line id, which a sibling bank preserves untouched. The face bank for a
	// language follows for free, because face banks key to a speech bank by path.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Translate one bank's lines into a sibling bank for a language, creating it when missing.
	 *
	 * SB_<Name>_<LANG> lands beside the source bank, carrying the same line ids, speakers,
	 * direction and voice overrides - only the text is translated. A line is re-translated when
	 * its source text has moved since (TranslatedFromTextHash), or when bForce says so; audio is
	 * deliberately untouched here. Generate the localised bank afterwards exactly like any other -
	 * same voices resolve through the same speakers, and the sounds land in a per-language folder.
	 *
	 * **Bills translation characters the moment it is sent**, through the named translation
	 * provider - or the resolved default: the sole real provider, else the built-in "Pseudo".
	 *
	 * @return An error string, or empty when the batch was dispatched. Completion arrives on the
	 *         callback with a per-line summary.
	 */
	FString LocalizeBank(
		const FString& BankPath,
		const FString& TargetLanguage,
		FName TranslationProviderId,
		bool bForce,
		TFunction<void(bool /*bSuccess*/, const FString& /*Summary*/)> OnComplete);

	/** Every localised sibling of a bank, counted against today's source text. Registry-cached. */
	UFUNCTION(BlueprintCallable, Category = "SpeechForge|Localisation")
	TArray<FSpeechLocalizationStatus> GetLocalizationStatus(const FString& BankPath) const;

	/**
	 * Give one localised line its audio by dubbing the source line's recording across languages.
	 *
	 * The performance-take path: where TTS re-reads the translated words, a dub carries the
	 * original actor's pacing, pauses and voice into the target language - the delivery survives
	 * by construction, which is the whole reason to record a human. Uses the speech provider's
	 * dubbing capability; refused with a reason where the provider has none. The source audio is
	 * the source line's recorded sound (its take's WAV where one exists).
	 *
	 * **Bills dubbing minutes the moment it is sent** - notably more than synthesis.
	 */
	void DubLineAudio(
		const FSpeechLineHandle& LocalizedHandle,
		FOnLineConverted OnComplete);

	/**
	 * Make a recorded performance the line's audio. The graduation operation.
	 *
	 * AudioSource is either the content path of a sound already in the project, or the absolute
	 * path of a WAV to import. Either way: the line's Sound repoints, its Origin becomes Recorded,
	 * and the generated take is *not* unlinked - GeneratedSound keeps it, because it is the
	 * reference the performance was directed against. ContentHash is left untouched on purpose:
	 * a later script edit then reads as stale-plus-Recorded, which is the pickup-session report,
	 * not a regeneration.
	 *
	 * @return The applied sound's content path, or empty with OutError set.
	 */
	FString ApplyRecordedAudio(const FString& AssetPath, FName LineId, const FString& AudioSource, FString& OutError);

private:

	/** Load a bank or single-line asset and return it as a line source. */
	UObject* LoadSourceAsset(const FString& AssetPath) const;
	ISpeechLineSource* AsLineSource(UObject* Asset) const;

	/** A handle with no LineId on a multi-line asset becomes one handle per line. */

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
