// Shared vocabulary for the speech pipeline.

#pragma once

#include "CoreMinimal.h"
#include "SpeechForgeTypes.generated.h"

class USpeechVoice;

/**
 * Where a line has got to in the pipeline.
 *
 * Deliberately does **not** include "stale". Staleness is not a stored state - it is the comparison
 * between the hash a line was generated from and the hash its current authoring resolves to, and
 * storing it would mean invalidating every line whenever any voice asset anywhere changed. Computing
 * it is trivially correct and cannot go out of date. See FSpeechLineStatus::bStale.
 */
UENUM(BlueprintType)
enum class ESpeechLineStatus : uint8
{
	/** Authored but never generated. */
	Draft		UMETA(DisplayName = "Draft"),

	/** A request is in flight. A second submit would be a duplicate charge. */
	Generating	UMETA(DisplayName = "Generating"),

	/** Audio exists in the project. The end of this plugin's job. */
	Generated	UMETA(DisplayName = "Generated"),

	/** Something went wrong - see LastError. Re-running is safe. */
	Failed		UMETA(DisplayName = "Failed")
};

/**
 * Where a line's audio came from, which is a different question from where it is in the pipeline.
 *
 * These are separate axes on purpose, and the intersection is the point. A `Recorded` line can also
 * be stale - meaning the script has been rewritten since an actor recorded it - and that is the one
 * report with real money attached, because it scopes a pickup session exactly. Fold graduation into
 * the status enum and that fact becomes inexpressible.
 */
UENUM(BlueprintType)
enum class ESpeechLineOrigin : uint8
{
	/** As produced by a provider. The tool owns it and may overwrite it. */
	Generated	UMETA(DisplayName = "Generated"),

	/**
	 * Generated, then blessed across an authoring change without regenerating.
	 *
	 * The pressure valve for a voice tweak that marks a whole cast stale and is not worth the money
	 * or the re-rolled performances. Recorded rather than quietly re-hashed, so it stays honest.
	 */
	Accepted	UMETA(DisplayName = "Accepted"),

	/**
	 * The generated file has been hand-touched.
	 *
	 * Detected rather than declared, by comparing the audio against the hash of what was imported.
	 * Nobody remembers to tell a tool they edited something, and the failure that closes is a
	 * levelled or de-essed file being silently overwritten weeks later.
	 */
	Edited		UMETA(DisplayName = "Edited"),

	/** Replaced by an authored take - an actor, or any better source. */
	Recorded	UMETA(DisplayName = "Recorded")
};

/** What a provider charges for, so an estimate reports the quantity that actually bills. */
UENUM(BlueprintType)
enum class ESpeechBillingUnit : uint8
{
	/** Free. A local or self-hosted model. */
	None		UMETA(DisplayName = "None"),

	/** Characters of input text, charged at request time. */
	Characters	UMETA(DisplayName = "Characters"),

	/** Seconds of audio produced, which cannot be known before generating. */
	Seconds		UMETA(DisplayName = "Seconds")
};

/** How a voice came to exist, which decides whether it can be recreated. */
UENUM(BlueprintType)
enum class ESpeechVoiceProvenance : uint8
{
	Unknown		UMETA(DisplayName = "Unknown"),

	/**
	 * A stock voice belonging to the provider.
	 *
	 * **Treat as a liability rather than a default.** A provider can retire its stock voices - and
	 * ElevenLabs has announced exactly that - which takes every line generated against one with it.
	 * A library that has to outlive a prototype wants Designed or Cloned.
	 */
	Premade		UMETA(DisplayName = "Premade"),

	/** Generated from a written description. Recreatable if the prompt is kept. */
	Designed	UMETA(DisplayName = "Designed"),

	/** Cloned from recorded samples. Recreatable only if the samples are kept. */
	Cloned		UMETA(DisplayName = "Cloned")
};

/**
 * The dimensions of a performance that every speech engine has in some form.
 *
 * Deliberately neutral rather than shaped to one vendor. Providers map what they can onto their own
 * parameters and **log what they ignore** rather than failing, so one voice asset stays usable across
 * providers that support different subsets. Anything genuinely vendor-specific goes in
 * ProviderOverrides instead of growing this struct.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechVoiceSettings
{
	GENERATED_BODY()

	/**
	 * Consistency against expressiveness.
	 *
	 * Low is more emotional and more willing to follow direction, and more prone to wandering. High
	 * is steady and comparatively deaf to performance notes. Direction (§ the line's Direction field)
	 * only lands if this leaves room for it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice", meta = (ClampMin = 0.0, ClampMax = 1.0))
	float Stability = 0.5f;

	/** How closely to adhere to the reference voice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice", meta = (ClampMin = 0.0, ClampMax = 1.0))
	float Similarity = 0.75f;

	/** How far the model will push a performance away from a neutral read. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice", meta = (ClampMin = 0.0, ClampMax = 1.0))
	float StyleIntensity = 0.f;

	/** Delivery rate, where 1 is the voice's natural pace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice", meta = (ClampMin = 0.25, ClampMax = 4.0))
	float Speed = 1.f;

	/**
	 * Provider-specific parameters, passed through untouched.
	 *
	 * The escape hatch that keeps the four fields above neutral. A provider reads the keys it knows
	 * and ignores the rest; unknown keys are logged once rather than silently dropped, because a
	 * misspelled override that does nothing looks exactly like one that did not help.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voice|Advanced")
	TMap<FName, FString> ProviderOverrides;

	/** Stable text form, for hashing. Order-independent over the override map. */
	FString ToHashString() const;
};

/**
 * A fully concrete answer to "which voice does this line use".
 *
 * The important word is *concrete*. A line does not name a voice, it resolves one - possibly through
 * an external source that SpeechForge knows nothing about - and everything downstream, the content
 * hash above all, works on the resolved answer rather than the reference to it. Hashing the reference
 * would mean that repointing a speaker in some other plugin changes no hash, so every line still
 * reports itself current while being voiced by the wrong character, with nothing saying so.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechVoiceResolution
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FName ProviderId;

	/** The provider's own identifier for this voice. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString ProviderVoiceId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString ModelId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FSpeechVoiceSettings Settings;

	/**
	 * Where this answer came from, in words.
	 *
	 * Not decoration. When a line comes out in the wrong voice the first question is *why did it
	 * resolve to that*, and both a human and an agent need an answer better than "open four assets
	 * and guess". Something like "NP_VoiceOver -> NPCDefinition 'Alexis'".
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString SourceDescription;

	bool IsValid() const { return !ProviderVoiceId.IsEmpty() && !ProviderId.IsNone(); }

	/** Stable text form, for hashing. Excludes SourceDescription, which is diagnostic only. */
	FString ToHashString() const;

	/** Human-readable one-liner naming what differs from Other. Empty when they match. */
	FString DescribeDifference(const FSpeechVoiceResolution& Other) const;
};

/** What a line is being resolved for, handed to every voice source in turn. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechVoiceQuery
{
	GENERATED_BODY()

	/** Who is speaking. The join key an external source matches on. */
	UPROPERTY(BlueprintReadWrite, Category = "Voice")
	FName SpeakerId;

	UPROPERTY(BlueprintReadWrite, Category = "Voice")
	FName LineId;

	/** Content path of the bank or single-line asset the line lives in. */
	UPROPERTY(BlueprintReadWrite, Category = "Voice")
	FString SourceAssetPath;
};

/**
 * What a provider can actually do, so nothing above has to special-case it by name.
 *
 * Deliberately not optional. Every field here is a difference we already know exists between
 * candidate providers, and each one was at some point an assumption that would have been baked into
 * the pipeline for whichever provider happened to be first.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechProviderCaps
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FName ProviderId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FString DisplayName;

	/** The rate this provider produces audio at. Not a global setting - it differs per provider. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	int32 NativeSampleRate = 44100;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	ESpeechBillingUnit BillingUnit = ESpeechBillingUnit::None;

	/** False for anything local or self-hosted, which must estimate zero rather than a lesser wrong number. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bIsMetered = false;

	/** Whether a seed can be supplied at all. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsSeed = false;

	/**
	 * Whether seeding only *approximately* reproduces a generation.
	 *
	 * True on every hosted speech model tested so far - the same seed and text come back subtly
	 * different. A provider that seeds approximately must not be able to claim reproducibility,
	 * because everything downstream would believe it and treat a regenerated line as identical.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSeedIsBestEffort = true;

	/** Whether character-level timings come back with the audio. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsAlignment = false;

	/**
	 * Whether *any* of this provider's models take surrounding lines as context.
	 *
	 * Provider-wide, and therefore not the whole answer - stitching is a per-model capability and
	 * `ISpeechProvider::SupportsStitchingForModel` is what the pipeline actually asks. Measured on
	 * ElevenLabs: the expressive model that takes inline direction **rejects** stitching, and the
	 * model that stitches has no audio tags. Same account, opposite answers.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsStitching = false;

	/** Whether performance direction in the text does anything, e.g. inline audio tags. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsDirection = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsVoiceListing = false;

	/**
	 * Whether a past generation can be fetched again from the provider, free, by its request id.
	 *
	 * This is what makes a non-reproducible provider survivable: where a seed cannot recreate a take,
	 * the request id is the only durable route back to it. Never prune one.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bSupportsRemoteHistory = false;

	/**
	 * How many requests may be in flight at once.
	 *
	 * On hosted providers this is a property of the subscription tier rather than of the code, and
	 * exceeding it usually does not error - it queues. So a batch that ignores this appears to work
	 * and is simply slow, with nothing in any log to say why.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	int32 MaxConcurrentRequests = 1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	bool bNeedsCredential = false;

	/**
	 * What a human has to do before this provider works at all. Empty when it is ready.
	 *
	 * The field to act on: it names something no agent can do for them.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provider")
	FString SetupHint;
};

/** One line, addressed. LineId is None for a single-line asset. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineHandle
{
	GENERATED_BODY()

	FSpeechLineHandle() = default;
	FSpeechLineHandle(const FString& InAssetPath, FName InLineId)
		: AssetPath(InAssetPath), LineId(InLineId) {}

	/** Content path of a speech bank or single-line asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech")
	FString AssetPath;

	/** Which line within it. Leave as None for an asset that holds only one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech")
	FName LineId;

	bool IsValid() const { return !AssetPath.IsEmpty(); }

	FString ToString() const
	{
		return LineId.IsNone() ? AssetPath : FString::Printf(TEXT("%s#%s"), *AssetPath, *LineId.ToString());
	}

	bool operator==(const FSpeechLineHandle& Other) const
	{
		return AssetPath == Other.AssetPath && LineId == Other.LineId;
	}
};

FORCEINLINE uint32 GetTypeHash(const FSpeechLineHandle& Handle)
{
	return HashCombine(GetTypeHash(Handle.AssetPath), GetTypeHash(Handle.LineId));
}

/**
 * What a set of lines would cost to generate.
 *
 * Unlike most generation pipelines this is arithmetic rather than a projection: where billing is per
 * character of input, the number is exact and knowable before anything is submitted, with no network
 * call. That is the only moment the decision to spend can still be made.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechCostEstimate
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	int32 LineCount = 0;

	/** Characters that would be sent, across every line counted. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	int32 TotalCharacters = 0;

	/** Of those, the ones that actually bill under the configured plan. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	int32 BilledCharacters = 0;

	/** Zero when nothing counted uses a metered provider, or when no rate is configured. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	float EstimatedCost = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	FString Currency;

	/** False when nothing in this estimate would cost anything. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	bool bAnyMetered = false;

	/** Lines skipped because they are current, graduated, or already in flight. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cost")
	int32 SkippedCount = 0;
};

/**
 * Where the pipeline writes, resolved.
 *
 * The root is the only thing configured; every folder below it is derived. The sort by kind is a
 * property of the pipeline rather than a preference, so pointing output somewhere else moves the
 * whole structure intact instead of letting it flatten into one folder on the way.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechOutputPaths
{
	GENERATED_BODY()

	/** The configured root. Everything below is this plus one folder name. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString Root;

	/** Speech banks - the lines, their state, and what has been generated for each. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString Banks;

	/** Speech voice assets: which provider voice a character speaks with. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString Voices;

	/** Imported sound waves. The audio itself. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString Sounds;

	/**
	 * Empty on success. Set when a root was refused, saying why.
	 *
	 * A refused change leaves every path above reporting what is still configured, so a caller that
	 * ignores this field reads the truth rather than the request it thought it had made.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString Problem;
};

/** One voice a provider holds, in a form tools and Blueprint can read. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechRemoteVoiceInfo
{
	GENERATED_BODY()

	/** What goes in a Speech Voice asset's Provider Voice Id. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString Id;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	FString Description;

	/**
	 * A stock voice belonging to the provider rather than to this account.
	 *
	 * Worth checking before building a library on one: a provider can retire its stock voices, and
	 * every line generated against one goes with it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voice")
	bool bIsPremade = false;
};

/** Whether a provider has a usable secret, without revealing it. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechCredentialInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Credential")
	FName ProviderId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Credential")
	bool bConfigured = false;

	/** Where it is read from, in words. Never the value. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Credential")
	FString Source;

	/** The environment variable consulted before the vault. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Credential")
	FString EnvironmentVariable;

	/** What a human has to do about it, when bConfigured is false. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Credential")
	FString SetupHint;
};

/**
 * Everything reported about one line.
 *
 * Three orthogonal facts, deliberately not collapsed into one enum: where it is in the pipeline,
 * where its audio came from, and whether the audio still matches what the line now says.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineStatus
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FSpeechLineHandle Handle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	ESpeechLineStatus Status = ESpeechLineStatus::Draft;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	ESpeechLineOrigin Origin = ESpeechLineOrigin::Generated;

	/**
	 * The audio no longer matches what the line resolves to now.
	 *
	 * Computed, never stored. Stale plus an Origin of Recorded is the report worth having: the script
	 * changed after an actor recorded it, which is a pickup session and not a regeneration.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	bool bStale = false;

	/** What changed, in words, when bStale is true. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FString StaleReason;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FName SpeakerId;

	/** How the voice resolved right now, whatever the audio was made with. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FSpeechVoiceResolution ResolvedVoice;

	/** Content path of the sound the game plays, empty when none exists yet. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FString SoundPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	float DurationSeconds = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	int32 CharacterCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Status")
	FString LastError;
};

/** Authoring fields for one line, for callers that create lines rather than edit them by hand. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechLineSpec
{
	GENERATED_BODY()

	/** Stable identifier within its asset. Generated from the text when left empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech")
	FName LineId;

	/** What the player reads and what is spoken. Keep performance notes out of it - see Direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech", meta = (MultiLine = true))
	FString Text;

	/**
	 * How it should be performed, kept apart from the text on purpose.
	 *
	 * Put direction in the text and the subtitle reads it out. The audio is correct, the asset is
	 * correct, the log is clean, and the screen says "[sighs] ...damn it."
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech")
	FString Direction;

	/** Who is speaking. The key an external voice source matches on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech")
	FName SpeakerId;

	/** Leave unset to resolve through the speaker and the asset's defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech|Advanced")
	TSoftObjectPtr<USpeechVoice> VoiceOverride;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech|Advanced")
	FString ModelOverride;

	/** Negative means none. Honoured only where the provider seeds, and only approximately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech|Advanced")
	int32 SeedOverride = -1;

	/** The line this one follows, so prosody carries across a conversation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech|Advanced")
	FName PreviousLineId;
};
