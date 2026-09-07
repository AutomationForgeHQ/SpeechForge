// One line: what to say, how it was said, and what it became.

#pragma once

#include "CoreMinimal.h"
#include "SpeechForgeTypes.h"
#include "SpeechAlignment.h"
#include "SpeechLine.generated.h"

class USoundWave;
class USpeechVoiceProfile;

/** Where a take candidate came from. */
UENUM(BlueprintType)
enum class ESpeechTakeKind : uint8
{
	/** One detached synthesis - a generated candidate that touched nothing when it was made. */
	Generated	UMETA(DisplayName = "Generated"),

	/** A captured performance, with its media filed in the take library. */
	Recorded	UMETA(DisplayName = "Recorded"),
};

/**
 * One candidate for a line, on a ledger instead of overwriting it.
 *
 * Generation normally overwrites the line's sound in place - latest-wins, right for a solo
 * prototyper. Production wants candidates side by side: generate a line three times, record it
 * twice, and a director picks. A take carries everything needed to *become* the line later -
 * the imported sound, and for generated takes the exact facts generation would have written
 * (hash, resolution, request id, alignment) - so choosing a take leaves the line indistinguishable
 * from one generated or recorded directly, and staleness stays honest.
 */
/**
 * One audio track of a take: what it sounded like, in one voice.
 *
 * A take is a performance - a delivery, a face, a moment that happened once. Re-voicing it does not
 * produce a second performance, it produces the same performance in another voice, and listing that
 * beside the original as a sibling take is how a ledger stops meaning anything. So conversions live
 * here, under the take they came from, and the viewer plays the picture against whichever one you
 * pick.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechTakeAudio
{
	GENERATED_BODY()

	/** None on the take's own recording - the original is a variant with no id, not a stored row. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	FName VariantId;

	/** What the picker shows: the voice's name, not its provider id. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	FString Label;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	FString SoundPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	float DurationSeconds = 0.f;

	/** The voice it was re-cast into. Speech to speech keeps the delivery and changes only this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	FSpeechVoiceResolution Voice;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Variant")
	FDateTime CreatedAt = FDateTime();
};

USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineTake
{
	GENERATED_BODY()

	/** Unique within its line. The handle every choose and list addresses. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FName TakeId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	ESpeechTakeKind Kind = ESpeechTakeKind::Generated;

	/** Content path of the candidate's imported sound - auditionable before it is anything else. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString SoundPath;

	/** The capture-take folder for a recorded take (video, manifest). Empty for generated. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString TakeDir;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FDateTime CreatedAt = FDateTime();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	float DurationSeconds = 0.f;

	// Generated facts, applied verbatim on choose so the line stays hash-honest.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString ContentHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString ProviderRequestId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FSpeechVoiceResolution GeneratedWith;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FSpeechAlignment Alignment;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString ImportedAudioHash;

	/**
	 * The words this take actually speaks.
	 *
	 * Carried on the take rather than only on the line, so the ledger can say which candidates were
	 * performed against an older script. A take recorded on Tuesday and a rewrite on Wednesday are
	 * both legitimate; choosing the Tuesday take without being told is not.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FString SpokenTextHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	int32 BilledCharacters = 0;

	/**
	 * Re-voicings of this take, each the same performance in a different voice.
	 *
	 * Empty on almost every take, and deliberately not a take list: a conversion is not a candidate
	 * competing with this one, it is this one heard differently.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	TArray<FSpeechTakeAudio> Variants;

	/**
	 * Which of this take's voices wins if the take is chosen. None means its own recording.
	 *
	 * Kept on the take rather than passed to the choose call, so that picking a voice while
	 * reviewing and pressing Choose afterwards do the obvious thing - and so the decision survives
	 * closing the panel, which a parameter would not.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Take")
	FName ChosenVariantId;
};

/**
 * The unit of the pipeline.
 *
 * A plain struct rather than an asset, because there are two containers for it - a bank holding many
 * and a single-line asset holding one - and neither is special-cased anywhere downstream. See
 * ISpeechLineSource.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLine
{
	GENERATED_BODY()

	// ---------------------------------------------------------------------------------------------
	// Authoring - edit these, then generate
	// ---------------------------------------------------------------------------------------------

	/**
	 * Stable identifier within its container, and the handle every tool addresses.
	 *
	 * Generated from the text when first left empty, then never moved - because a line id that
	 * follows the text would break every reference the moment somebody fixes a typo.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line")
	FName LineId;

	/**
	 * What the player reads and what is spoken.
	 *
	 * Keep performance notes out of this. See Direction, and the reason it is a separate field.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line", meta = (MultiLine = true))
	FString Text;

	/**
	 * How the line should be performed.
	 *
	 * Separate from Text, and this is not a stylistic choice. Providers that take inline direction
	 * want it merged into the string they are sent - but that string is not the string shown to the
	 * player. Put "[sighs]" in Text and the subtitle reads "[sighs] ...damn it." The audio is right,
	 * the asset is right, the log is clean, and only somebody looking at the screen ever finds it.
	 *
	 * Providers that cannot use direction ignore this and say so once in the log, and because the
	 * content hash is taken over the request that is actually sent, editing it does not mark lines
	 * stale on those providers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line")
	FString Direction;

	/**
	 * Who is speaking.
	 *
	 * The join key an external voice source matches on - an NPC definition, a casting table - so it
	 * is worth setting even when a voice is assigned directly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line")
	FName SpeakerId;

	/** Leave unset to resolve through the speaker's sheet and the container's defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line|Advanced")
	TSoftObjectPtr<USpeechVoiceProfile> VoiceOverride;

	/** Empty means the voice's model, then the project default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line|Advanced")
	FString ModelOverride;

	/**
	 * Negative means none.
	 *
	 * Honoured only where the provider seeds, and on hosted speech models only approximately - the
	 * same seed and text come back subtly different. Check bSeedIsBestEffort before believing it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line|Advanced")
	int32 SeedOverride = -1;

	/**
	 * The recording this line's audio was converted from, when it was converted rather than spoken.
	 *
	 * Set means "this line is a re-voiced performance": the delivery is a person's, the identity is
	 * the cast voice's. Its hash - not the text - is what the line's staleness is computed over,
	 * because re-reading the script cannot change a recording, while replacing the source or
	 * re-casting the speaker changes everything about the result.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Line|Advanced")
	TSoftObjectPtr<USoundWave> SourceSound;

	/** Identity of the source audio at the moment it was converted. Empty when never converted. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Line|Advanced")
	FString SourceAudioHash;

	/**
	 * The line this one follows.
	 *
	 * Providers that support stitching are given the previous line's text and request id, so prosody
	 * carries across a conversation instead of every line sounding recorded in its own session.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line|Advanced")
	FName PreviousLineId;

	// ---------------------------------------------------------------------------------------------
	// State - written by the pipeline, read by everyone
	// ---------------------------------------------------------------------------------------------

	/** Where this line is in the pipeline. Note that "stale" is not one of these - it is computed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	ESpeechLineStatus Status = ESpeechLineStatus::Draft;

	/**
	 * Where the audio came from, which is a different axis from Status.
	 *
	 * Anything other than Generated means the pipeline no longer owns this audio and will not
	 * overwrite it without being told twice.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	ESpeechLineOrigin Origin = ESpeechLineOrigin::Generated;

	/**
	 * Hash of the request the current audio was generated from.
	 *
	 * Over the **resolved** request - the text as actually sent, the concrete voice, the model, the
	 * settings, the seed - never over the reference to a voice. Hashing the reference would mean that
	 * repointing a speaker in some other plugin changes no hash here, so every line still reports
	 * itself current while being voiced by the wrong character.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString ContentHash;

	/**
	 * The provider's handle to this exact generation.
	 *
	 * Where a provider keeps history, this re-fetches the identical audio for free, forever, and is
	 * therefore the only durable route back to a take that a seed cannot reproduce. Never cleared,
	 * never pruned.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString ProviderRequestId;

	/** Why the last operation failed. Cleared when one succeeds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString LastError;

	/** Characters actually billed for this line, as reported by the provider. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	int32 GeneratedCharacters = 0;

	/**
	 * On a line in a localised bank: SpokenTextHash of the source line's text at translation time.
	 *
	 * The translation staleness axis, same construction as every other hash here: when the source
	 * line's text no longer hashes to this, the source was rewritten after this translation was
	 * made, and the localisation pass re-translates the line. Empty on an authoring bank's lines.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FString TranslatedFromTextHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	FDateTime GeneratedAt = FDateTime();

	// ---------------------------------------------------------------------------------------------
	// Result
	// ---------------------------------------------------------------------------------------------

	/** What the game plays. May be a recorded take rather than a generated one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	TSoftObjectPtr<USoundWave> Sound;

	/**
	 * The generated take, retained permanently - including after replacement.
	 *
	 * Not sentiment. A recorded take can be rejected and fallen back from that afternoon, and the
	 * generated take is the reference an actor was directed against, which is what a production brief
	 * has to carry. Nothing in this plugin deletes it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	TSoftObjectPtr<USoundWave> GeneratedSound;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FSpeechAlignment Alignment;

	/**
	 * The voice this audio was actually made with, as opposed to the one it would resolve to now.
	 *
	 * Stored rather than recomputed, because that is what lets a staleness report say "Stability 0.5
	 * -> 0.35" instead of "the hash differs".
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FSpeechVoiceResolution GeneratedWith;

	/**
	 * Hash of the audio bytes as imported.
	 *
	 * Lets hand-editing be **detected** rather than declared. Nobody remembers to tell a tool they
	 * levelled a file in a DAW, and the failure that closes is the tool overwriting their work three
	 * weeks later without either party noticing.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString ImportedAudioHash;

	/**
	 * The words the current audio actually speaks.
	 *
	 * Separate from ContentHash because they answer separate questions, and conflating them is how a
	 * shipped game gets a subtitle that disagrees with its own voice line. ContentHash asks *would
	 * re-running the operation produce something different* - a question only a generated line can
	 * act on. This asks *does the audio still say what the line says*, which applies to every line
	 * that has audio at all, however it got there: synthesised, recorded by an actor, or re-voiced
	 * from someone else's performance.
	 *
	 * Stamped wherever audio is attached, and never by an edit to the script. So a rewrite moves the
	 * text away from this hash and the line reports the drift, whether or not any tool can fix it -
	 * and where none can, that is precisely the point.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString SpokenTextHash;

	/**
	 * When this line's audio asset was last checked for outside edits.
	 *
	 * The gate that makes detection affordable. Hashing every sound on every refresh means reading
	 * every WAV in the project; comparing the audio package's file time against this costs a stat,
	 * and a file that has not been written cannot have been edited.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FDateTime AudioCheckedAt = FDateTime();

	/**
	 * Which recording in the source language this line was dubbed from - that line's audio identity
	 * at the moment the dub was made.
	 *
	 * The one thing a dub needs that no other operation does. Everything else in a bank is derived
	 * from inputs held on the line itself, so recomputing a hash answers the staleness question. A
	 * dub is derived from *another line, in another bank*, which can be re-recorded afterwards
	 * without anything here moving: the dub's own source hash was taken at dub time and stays true
	 * to a recording that is no longer the one the scene plays. Holding the source's audio identity
	 * makes the question askable - and askable cheaply, because it is a string comparison against a
	 * field the source line already maintains, not a re-read of its audio.
	 *
	 * Empty on every line that is not a dub, and on dubs made before this existed - which read as
	 * current, because a check that cannot be performed must not report a fault.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString DubbedFromAudioHash;

	// ---------------------------------------------------------------------------------------------
	// Takes - candidates on a ledger
	// ---------------------------------------------------------------------------------------------

	/**
	 * Candidates for this line, kept side by side: detached generations and registered recorded
	 * performances. None of them is the line until one is chosen - see ApplyLineTake on the
	 * subsystem - and in the default solo flow this stays empty while generation keeps
	 * overwriting in place.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Takes")
	TArray<FSpeechLineTake> Takes;

	/** Which take became the line, when one did. Ledger bookkeeping; the line's own fields are truth. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Takes")
	FName ChosenTakeId;

	const FSpeechLineTake* FindTake(FName TakeId) const
	{
		return Takes.FindByPredicate([TakeId](const FSpeechLineTake& Take) { return Take.TakeId == TakeId; });
	}

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	bool HasAudio() const { return !Sound.IsNull(); }

	/** True when the pipeline still owns this audio and may overwrite it. */
	bool IsOwnedByPipeline() const { return Origin == ESpeechLineOrigin::Generated; }

	/** True when a request is in flight and a second submit would be a duplicate charge. */
	bool IsBusy() const { return Status == ESpeechLineStatus::Generating; }

	/** Apply authoring fields from a spec, leaving all pipeline state untouched. */
	void ApplySpec(const FSpeechLineSpec& Spec);

	/** Copy authoring fields out into a spec. */
	FSpeechLineSpec ToSpec() const;

	// ---------------------------------------------------------------------------------------------
	// Hashing
	// ---------------------------------------------------------------------------------------------

	/**
	 * Hash of a resolved request.
	 *
	 * @param RequestText The string that will actually be sent, direction already merged in or left
	 *        out according to what the provider can use. Hashing what is sent rather than what was
	 *        authored is what makes an ignored field correctly fail to mark anything stale.
	 * @param Voice The concrete resolution, never a reference to a voice asset.
	 * @param Seed Negative for none.
	 */
	static FString ComputeContentHash(
		const FString& RequestText,
		const FSpeechVoiceResolution& Voice,
		int32 Seed);

	/**
	 * Hash of a resolved *conversion*.
	 *
	 * Over the source audio's identity and the voice it became - never the text, which a conversion
	 * does not read. So a rewritten script leaves a converted line current, and replacing the
	 * source or re-casting the speaker marks it stale, which is exactly the truth in both cases.
	 */
	/**
	 * Hash of the words themselves - nothing else.
	 *
	 * No voice, no model, no direction. Direction is how a line is performed, not what it says, so
	 * changing it must not raise a subtitle alarm; that belongs to ContentHash, which a generated
	 * line can act on. This is only ever asked one question: do the words still match the audio.
	 */
	static FString ComputeSpokenTextHash(const FString& Text);

	static FString ComputeConversionHash(
		const FString& SourceAudioHash,
		const FSpeechVoiceResolution& Voice);

	/** Derive a stable id from text, for a line authored without one. Never applied to an existing id. */
	static FName MakeLineIdFromText(FName SpeakerId, const FString& InText);
};

/** What a container supplies to lines that do not name a voice or model themselves. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineDefaults
{
	GENERATED_BODY()

	/** Used by every line in this container whose speaker resolves no voice of their own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	TSoftObjectPtr<USpeechVoiceProfile> Voice;

	/** Empty falls back to the voice's model, then the project default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	FString ModelId;

	/** Applied to every line here that has no SpeakerId of its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	FName SpeakerId;
};
