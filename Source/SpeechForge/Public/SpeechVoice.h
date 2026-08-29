// A speaker, paired with a provider's voice.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeechForgeTypes.h"
#include "SpeechVoice.generated.h"

/**
 * The pairing between a character in this project and a voice on a provider.
 *
 * One asset per speaking role. Everything that makes a performance repeatable lives here - the
 * provider, its voice id, the model, and the settings - so that a line only has to say who is
 * talking.
 *
 * Changing anything on this asset marks every line that resolves to it stale, because the audio
 * genuinely no longer matches what the project says it should be. That is correct and it is
 * expensive; see the staleness report before regenerating a cast.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Voice"))
class SPEECHFORGE_API USpeechVoice : public UDataAsset
{
	GENERATED_BODY()

public:

	/**
	 * Who this is, in project terms.
	 *
	 * The key an external voice source matches a line's SpeakerId against, so it is worth keeping
	 * identical to whatever the rest of the project calls this character.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice")
	FName SpeakerId;

	/** Free text for whoever inherits this. Casting notes, accent, age, the read you were after. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice", meta = (MultiLine = true))
	FString Description;

	/** Which provider holds the voice. Falls back to the project default when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Provider")
	FName ProviderId;

	/**
	 * The provider's own identifier for this voice.
	 *
	 * Opaque, and the one field that cannot be reconstructed from anything else. Losing it means
	 * re-casting the character.
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

	/** When the pairing was made, for tracing a voice that has since changed on the provider's side. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	FDateTime PairedAt = FDateTime();

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	bool IsPaired() const { return !ProviderVoiceId.IsEmpty(); }

	/**
	 * Build a resolution from this asset.
	 *
	 * @param FallbackProviderId Used when ProviderId is unset.
	 * @param FallbackModelId Used when ModelId is empty.
	 * @param SourceDescription What to record about where this answer came from.
	 */
	FSpeechVoiceResolution MakeResolution(
		FName FallbackProviderId,
		const FString& FallbackModelId,
		const FString& InSourceDescription) const;

	/** Why this voice cannot be used yet, or empty when it can. */
	FString GetSetupProblem() const;
};
