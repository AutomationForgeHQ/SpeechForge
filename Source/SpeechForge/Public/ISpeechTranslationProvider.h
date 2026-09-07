// What any text translation service has to be able to do.

#pragma once

#include "CoreMinimal.h"

/**
 * One batch of texts to translate, fully resolved.
 *
 * A batch rather than a string, because the callers are banks: a localisation pass translates a
 * scene's worth of lines in one call, and per-line round trips would turn a two-second job into a
 * minute of rate limiting.
 */
struct FSpeechTranslationRequest
{
	/** The strings to translate, in an order the result must preserve. */
	TArray<FString> Texts;

	/** BCP-47-ish code of the source ("en"). Empty lets the service detect it. */
	FString SourceLanguage;

	/** BCP-47-ish code of the target ("de", "pt-BR"). Never empty. */
	FString TargetLanguage;

	/**
	 * Untranslated context that may steer word choice - what the scene is, who is speaking.
	 * Services that cannot use it ignore it; none of it is billed as translated text.
	 */
	FString Context;
};

/** What came back. Translations align with the request's Texts by index. */
struct FSpeechTranslationResult
{
	bool bSuccess = false;

	TArray<FString> Translations;

	/** What the service decided the source language was, when it had to guess. */
	FString DetectedSourceLanguage;

	/** What this actually cost, in the service's billing unit (characters), as reported. */
	int32 BilledCharacters = 0;

	FString Error;
};

using FOnSpeechTranslated = TFunction<void(const FSpeechTranslationResult&)>;

/**
 * A text translation service.
 *
 * The seam localisation goes through, mirroring ISpeechProvider: anything vendor-specific - REST
 * shapes, language code spellings, formality flags - stays behind this line. Registered on
 * FSpeechForgeModule exactly the way speech providers are, and for the same module-ordering reason.
 *
 * Every call completes on the game thread.
 */
class SPEECHFORGE_API ISpeechTranslationProvider
{
public:

	virtual ~ISpeechTranslationProvider() = default;

	/** Stable identifier used in settings and tool calls, e.g. "DeepL". */
	virtual FName GetProviderId() const = 0;

	virtual FString GetDisplayName() const = 0;

	/** Service name this provider's secret is stored under in the credential vault. Empty = keyless. */
	virtual FString GetCredentialServiceName() const = 0;

	/** True when a usable credential is available, or none is needed. */
	virtual bool HasCredential() const = 0;

	/** Where a person signs up for a key, shown beside the field that asks for one. */
	virtual FString GetCredentialHelpUrl() const { return FString(); }

	/**
	 * The target language codes this translator accepts, in the pipeline's own dialect ("de",
	 * "pt-BR"). A surface offers exactly these; a tool call is still free to pass any code.
	 * Empty means the provider does not publish a list, and a surface falls back to free entry.
	 */
	virtual TArray<FString> GetTargetLanguages() const { return {}; }

	/** Translate one batch. Billed per character the moment it is sent. */
	virtual void Translate(const FSpeechTranslationRequest& Request, FOnSpeechTranslated OnComplete) = 0;
};
