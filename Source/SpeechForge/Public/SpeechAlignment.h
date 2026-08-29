// Character-level timing, and how it travels with the sound rather than with the pipeline.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "SpeechAlignment.generated.h"

/** One word, and when it is spoken. Derived from the character timings, never sent by a provider. */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechWordTiming
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	FString Word;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	float StartSeconds = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	float EndSeconds = 0.f;

	/** Index into FSpeechAlignment::Text where this word begins. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	int32 CharacterIndex = 0;
};

/**
 * When each character of a line is spoken.
 *
 * The cheapest valuable thing a speech provider returns and the reason the timestamped endpoint is
 * always the one to call: it costs the same as plain synthesis and pays for subtitle timing, exact
 * line durations, word boundaries for an audio-driven facial pass, and notify or shot-cut placement
 * on a real word rather than a guessed fraction.
 *
 * **Text is stored alongside the timings, and that is not redundant.** Providers commonly return two
 * alignments - one against the text as written and one against the text after normalisation, where
 * "3" has become "three". They have different lengths, but only for lines containing numbers or
 * abbreviations, so picking the wrong one is correct in every test that avoided digits and silently
 * wrong afterwards. Keeping the string the timings actually index makes the mismatch impossible to
 * express rather than merely unlikely.
 */
USTRUCT(BlueprintType)
struct SPEECHFORGE_API FSpeechAlignment
{
	GENERATED_BODY()

	/** The exact string these timings index into, character for character. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	FString Text;

	/** One per character of Text. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	TArray<float> CharacterStartSeconds;

	/** One per character of Text. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	TArray<float> CharacterEndSeconds;

	/** Derived on import, for consumers that work in words rather than characters. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	TArray<FSpeechWordTiming> Words;

	/**
	 * Where the last character stops, which is the spoken length of the line.
	 *
	 * Cross-check this against the imported sound's own duration. The two come from sources that
	 * share no code - the provider's JSON and the engine's decoded asset - and a disagreement means
	 * the audio was resampled, arrived in an unexpected format, or the wrong alignment variant was
	 * read. It is the first test worth writing and it catches three bugs at once.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	float DurationSeconds = 0.f;

	/**
	 * True when these timings index normalised text rather than the text as written.
	 *
	 * Recorded rather than inferred, so a consumer that needs to map back onto the authored string
	 * can tell whether it is allowed to.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
	bool bFromNormalizedText = false;

	bool IsEmpty() const { return Text.IsEmpty() || CharacterStartSeconds.Num() == 0; }

	/**
	 * Check the arrays agree with the text and with each other.
	 *
	 * Worth calling on every import rather than trusting the provider: a length mismatch here is the
	 * signature of having read the wrong alignment variant, and it produces subtitles that are
	 * perfect until the first line containing a number.
	 */
	bool Validate(FString& OutError) const;

	/** Fill Words and DurationSeconds from the character arrays. Called once on import. */
	void DeriveWordsAndDuration();

	/** Seconds at which the word containing character Index begins, or -1. */
	float FindWordStart(int32 CharacterIndex) const;
};

/**
 * Alignment attached to the sound itself, so consumers need not know SpeechForge exists.
 *
 * A facial animation pass and a shot-authoring tool both want word timings for a sound, and neither
 * should have to find the bank that produced it in order to get them. UAssetUserData is the engine's
 * own mechanism for attaching data to an asset you do not own, and this is exactly what it is for.
 *
 * Two independent consumers is the point at which this stopped being a guess.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Alignment"))
class SPEECHFORGE_API USpeechAlignmentUserData : public UAssetUserData
{
	GENERATED_BODY()

public:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Speech")
	FSpeechAlignment Alignment;

	/** Which line produced this sound, so it can be traced back. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Speech")
	FName LineId;

	/** Content path of the bank or single-line asset it came from. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Speech")
	FString SourceAssetPath;

	/** Who spoke it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Speech")
	FName SpeakerId;
};
