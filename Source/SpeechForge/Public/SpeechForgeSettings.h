// Project Settings > Plugins > SpeechForge.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SpeechForgeTypes.h"
#include "SpeechForgeSettings.generated.h"

class USpeechVoice;

/**
 * Everything the pipeline needs that is not per-line, and that the whole team shares.
 *
 * Signing in is not one of those things — a key is one person's, on one machine, so it lives in
 * Editor Preferences ▸ Automation Forge ▸ SpeechForge and in the OS credential vault.
 * See USpeechForgeEditorSettings.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "SpeechForge"))
class SPEECHFORGE_API USpeechForgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	USpeechForgeSettings();

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Automation Forge"); }

	static const USpeechForgeSettings* Get();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---------------------------------------------------------------------------------------------
	// Provider
	// ---------------------------------------------------------------------------------------------

	/** Which provider new voices use when they do not name one. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FName DefaultProviderId = TEXT("ElevenLabs");

	/**
	 * Model used when neither the line nor the voice names one.
	 *
	 * The expressive model is the default because inline direction is what makes the Direction field
	 * worth having. A faster, cheaper model is the right choice per bank for barks and system lines
	 * where nobody is listening for a performance.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FString DefaultModelId = TEXT("eleven_v3");

	/** Voice used when nothing else resolves one. Mostly useful while prototyping. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	TSoftObjectPtr<USpeechVoice> DefaultVoice;

	/**
	 * What one thousand billed characters costs, for turning estimates into money.
	 *
	 * Zero means estimates report characters only. Nothing can detect this; only the account knows,
	 * and the rate differs by model.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 0.0, DisplayName = "Cost Per 1000 Characters"))
	float CostPerThousandCharacters = 0.10f;

	/** Purely for display alongside an estimate. */
	UPROPERTY(config, EditAnywhere, Category = "Provider")
	FString Currency = TEXT("USD");

	/**
	 * Cap on requests in flight, when set above zero.
	 *
	 * Leave at zero to use whatever the provider reports, which is the right answer: on hosted
	 * services the real limit belongs to the subscription tier, and exceeding it does not error - it
	 * queues. A batch that ignores it appears to work and is simply slow, with nothing in any log to
	 * say why. Set this only to go *below* the provider's number, never above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 0, ClampMax = 64))
	int32 MaxConcurrentRequestsOverride = 0;

	/** Give up on a request after this long. */
	UPROPERTY(config, EditAnywhere, Category = "Provider", meta = (ClampMin = 5, Units = "Seconds"))
	int32 RequestTimeoutSeconds = 120;

	// ---------------------------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------------------------

	/** Content path new banks, voices and imported sounds are created under. */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	FString OutputContentPath = TEXT("/Game/_Generated/Speech");

	// Sorted by kind, at the point of generation.
	//
	// A tool that writes every asset it makes into one folder produces something nobody can read
	// after the second run - voices, banks and results in one list, with throwaway test fixtures
	// indistinguishable from real content. Tidying that up by hand does not hold, because the next
	// generation puts it all back; the sort has to live here, where the assets are created.
	FString GetBanksPath()  const { return OutputContentPath / TEXT("Banks"); }
	FString GetVoicesPath() const { return OutputContentPath / TEXT("Voices"); }
	FString GetSoundsPath() const { return OutputContentPath / TEXT("Sounds"); }

	/** Every path above, resolved, as one value a caller can read or report. */
	FSpeechOutputPaths GetOutputPaths() const;

	/**
	 * Point the pipeline at a different content root, and persist it.
	 *
	 * Refuses anything that is not a valid content path under a mounted root, because the failure it
	 * prevents is silent: an unmounted or malformed path produces packages that are created in memory,
	 * never saved, and reported as successes.
	 *
	 * Moves nothing. Banks and sounds already written stay where they are, and a bank keeps working
	 * from wherever it is - this decides where the *next* assets are created.
	 *
	 * @return The resolved structure. On refusal, `Problem` says why and the paths describe what is
	 *         still configured rather than what was asked for.
	 */
	static FSpeechOutputPaths SetOutputRoot(const FString& ContentPath);

	/**
	 * Where downloaded audio lands before import, relative to the project directory.
	 *
	 * Raw responses are kept rather than deleted. Where a provider cannot reproduce a generation from
	 * a seed, this file and the provider's own history are the only two copies that will ever exist,
	 * and only one of them survives the account being closed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	FString StagingDirectory = TEXT("Saved/SpeechForge");

	/**
	 * Ask providers for this sample rate.
	 *
	 * 48 kHz, which is both what game audio engines want and - measured 2026-08-11, not read off a
	 * pricing page - what a mid-tier ElevenLabs subscription actually allows. **44100 is gated to a
	 * higher tier there and 48000 is not**, which is not a rule anybody would guess and is worth
	 * knowing before paying to lift a limit that is not in the way.
	 *
	 * The rate is still read back off the returned audio and checked rather than assumed, because
	 * that costs nothing and no provider's gating rules are a promise.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline", meta = (ClampMin = 8000, ClampMax = 48000))
	int32 RequestedSampleRate = 48000;

	/**
	 * Refuse to import audio that came back at a different rate than was asked for.
	 *
	 * Leave on. A provider that refuses a gated format outright is easy to handle; one that quietly
	 * substitutes a different rate is not, and the two are indistinguishable from the request side.
	 * Checking the returned bytes is what tells them apart.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	bool bFailOnSampleRateMismatch = true;

	/**
	 * Cross-check the last alignment timestamp against the imported sound's own duration.
	 *
	 * Leave on. The two numbers come from sources that share no code - the provider's JSON and the
	 * engine's decoded asset - so a disagreement catches a resample, an unexpected container, and a
	 * misread alignment variant, all of which otherwise pass every check and produce a plausible
	 * result.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline")
	bool bVerifyAlignmentAgainstDuration = true;

	/** How far the two may disagree before it is reported, in seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Pipeline",
		meta = (ClampMin = 0.001, EditCondition = "bVerifyAlignmentAgainstDuration", Units = "Seconds"))
	float AlignmentDurationToleranceSeconds = 0.05f;

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	/** Absolute path of the staging directory, created on demand. */
	FString GetAbsoluteStagingDirectory() const;
};
