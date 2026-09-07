// A voice as an instrument: a provider's preset, plus everything that makes it repeatable.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeechForgeTypes.h"
#include "SpeechVoiceProfile.generated.h"

/**
 * A voice profile: one voice, ready to be cast.
 *
 * The instrument, not the character. A profile names a provider, that provider's voice preset, the
 * model and the settings - everything that makes a performance repeatable - and deliberately says
 * nothing about who speaks in it. Speakers reference profiles (see USpeechSpeaker); two characters
 * can share one, and re-casting a character never edits a profile.
 *
 * Because the profile owns the provider, a project mixes providers freely - the question "which
 * vendor" is answered per voice, never per bank.
 *
 * Changing anything here marks every line that resolves through it stale, because the audio
 * genuinely no longer matches what the project says it should be. That is correct and it is
 * expensive; see the staleness report before regenerating a cast.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Voice Profile"))
class SPEECHFORGE_API USpeechVoiceProfile : public UDataAsset
{
	GENERATED_BODY()

public:

	/**
	 * What this voice is called in this project. Falls back to the provider's name for it, then the
	 * asset name. This is the string every panel shows, so name the sound, not a character -
	 * "Warm Male Narrator", not "Player".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice")
	FString DisplayName;

	/** The provider's own display name for the preset ("Sarah"), cached when the voice was browsed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice")
	FString ProviderVoiceName;

	/** Free text for whoever inherits this. What the voice sounds like, what it was chosen for. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice", meta = (MultiLine = true))
	FString Description;

	/** Which provider holds the voice. Falls back to the project default when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provider")
	FName ProviderId;

	/**
	 * The provider's own identifier for this voice.
	 *
	 * Opaque, and the one field that cannot be reconstructed from anything else. Losing it means
	 * re-casting every speaker that uses this profile.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provider")
	FString ProviderVoiceId;

	/** Empty falls back to the project default model. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provider")
	FString ModelId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provider")
	FSpeechVoiceSettings Settings;

	// ---------------------------------------------------------------------------------------------
	// Provenance
	// ---------------------------------------------------------------------------------------------

	/**
	 * How this voice came to exist.
	 *
	 * Not bookkeeping. A stock voice can be retired by its provider, which takes every line generated
	 * against it with it, so the field exists to make that case visibly a liability rather than a
	 * default. A designed or cloned voice is recreatable only if what made it is recorded below.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provenance")
	ESpeechVoiceProvenance Provenance = ESpeechVoiceProvenance::Unknown;

	/** The description this voice was designed from, when it was designed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provenance", meta = (MultiLine = true))
	FString DesignPrompt;

	/** Where the samples came from, when it was cloned. Enough to do it again. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provenance", meta = (MultiLine = true))
	FString CloneSourceNotes;

	/** When the profile was made, for tracing a voice that has since changed on the provider's side. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	FDateTime CreatedAt = FDateTime();

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	bool IsPaired() const { return !ProviderVoiceId.IsEmpty(); }

	/** The name a human should read: DisplayName, else the provider's name, else the asset name. */
	FString GetLabel() const;

	/**
	 * Build a resolution from this profile.
	 *
	 * @param FallbackProviderId Used when ProviderId is unset.
	 * @param FallbackModelId Used when ModelId is empty.
	 * @param SourceDescription What to record about where this answer came from.
	 */
	FSpeechVoiceResolution MakeResolution(
		FName FallbackProviderId,
		const FString& FallbackModelId,
		const FString& InSourceDescription) const;

	/** Why this profile cannot be used yet, or empty when it can. */
	FString GetSetupProblem() const;
};
