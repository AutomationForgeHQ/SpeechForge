// Audio on disk to a USoundWave in the project, with its timings attached.

#pragma once

#include "CoreMinimal.h"
#include "SpeechAlignment.h"

class USoundWave;

/** What to import, and where it should end up. */
struct FSpeechImportRequest
{
	/** The file the provider wrote. */
	FString AbsoluteAudioPath;

	/** Content path to create in, e.g. "/Game/_Generated/Speech/Sounds". */
	FString DestinationPackagePath;

	/** Asset name to create. Sanitised on the way in. */
	FString AssetName;

	/** Attached to the sound as user data, so consumers need not know SpeechForge exists. */
	FSpeechAlignment Alignment;

	FName LineId;
	FName SpeakerId;
	FString SourceAssetPath;

	/** Compare the alignment's last timestamp against the imported sound's own duration. */
	bool bVerifyDuration = true;

	/** How far the two may disagree before it is reported. */
	float DurationToleranceSeconds = 0.05f;
};

struct FSpeechImportResult
{
	bool bSuccess = false;

	TSoftObjectPtr<USoundWave> Sound;

	/** The engine's own view of how long the audio is. */
	float ImportedDurationSeconds = 0.f;

	/** ImportedDuration minus the alignment's last timestamp. The number the check is about. */
	float DurationDelta = 0.f;

	/** Hash of the bytes imported, so later hand-editing can be detected rather than declared. */
	FString AudioHash;

	FString Error;

	/** Non-fatal problems worth surfacing. */
	TArray<FString> Warnings;
};

/**
 * Turns a provider's audio file into a project asset.
 *
 * Small on purpose. The interesting part is not the import - the engine does that - but the check
 * afterwards: **the alignment's last timestamp must equal the imported sound's own duration.**
 *
 * Those two numbers come from sources that share no code. One is JSON the provider computed while
 * generating; the other is the engine's decoded asset. A disagreement between them catches a
 * resample, an unexpected container, and a misread alignment variant, all of which otherwise import
 * cleanly and produce a plausible result that is wrong in a way nobody notices for weeks.
 */
class SPEECHFORGE_API FSpeechImporter
{
public:

	static FSpeechImportResult Import(const FSpeechImportRequest& Request);

	/** SHA1 of a file's bytes, for detecting a hand-edited asset later. */
	static FString HashFile(const FString& AbsolutePath);

	/** Make a string safe to use as an asset name. */
	static FString SanitizeAssetName(const FString& In);
};
