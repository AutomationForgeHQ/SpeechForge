// One line: what to say, how it was said, and what it became.

#pragma once

#include "CoreMinimal.h"
#include "SpeechForgeTypes.h"
#include "SpeechAlignment.h"
#include "SpeechLine.generated.h"

class USoundWave;
class USpeechVoice;

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

	/** Leave unset to resolve through the speaker and the container's defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Line|Advanced")
	TSoftObjectPtr<USpeechVoice> VoiceOverride;

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

	/** Derive a stable id from text, for a line authored without one. Never applied to an existing id. */
	static FName MakeLineIdFromText(FName SpeakerId, const FString& InText);
};

/** What a container supplies to lines that do not name a voice or model themselves. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineDefaults
{
	GENERATED_BODY()

	/** Used by every line in this container that has no override and no external resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	TSoftObjectPtr<USpeechVoice> Voice;

	/** Empty falls back to the voice's model, then the project default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	FString ModelId;

	/** Applied to every line here that has no SpeakerId of its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Defaults")
	FName SpeakerId;
};
