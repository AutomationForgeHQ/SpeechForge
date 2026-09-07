// The keyless translation provider: pseudo-localisation, so the pipeline works before any vendor.

#pragma once

#include "ISpeechTranslationProvider.h"

/**
 * Pseudo-localisation, the industry's standard pipeline test: vowels gain accents, the target
 * language is stamped in front, and the text grows a little - so every place that mishandles
 * non-ASCII, clips at source length, or shows the wrong language is visible at a glance, without
 * an account anywhere.
 *
 * Registered by the core itself. Mock-the-provider is also how this pipeline is tested: the whole
 * localisation path - sibling bank, staleness, per-language audio, face re-solve - runs against
 * this before a real translator is ever billed.
 */
class FPseudoTranslationProvider : public ISpeechTranslationProvider
{
public:

	virtual FName GetProviderId() const override { return TEXT("Pseudo"); }
	virtual FString GetDisplayName() const override { return TEXT("Pseudo-localisation (built in)"); }
	virtual FString GetCredentialServiceName() const override { return FString(); }
	virtual bool HasCredential() const override { return true; }

	virtual void Translate(const FSpeechTranslationRequest& Request, FOnSpeechTranslated OnComplete) override
	{
		FSpeechTranslationResult Result;
		Result.bSuccess = true;
		Result.DetectedSourceLanguage = Request.SourceLanguage.IsEmpty()
			? TEXT("en") : Request.SourceLanguage;

		for (const FString& Text : Request.Texts)
		{
			FString Accented;
			Accented.Reserve(Text.Len() + 16);
			for (const TCHAR Char : Text)
			{
				Accented.AppendChar(Accent(Char));
			}

			Result.Translations.Add(FString::Printf(TEXT("[%s] %s"),
				*Request.TargetLanguage, *Accented));
			Result.BilledCharacters += Text.Len();
		}

		// Synchronous under the asynchronous contract: completes on the game thread, immediately.
		OnComplete(Result);
	}

private:

	static TCHAR Accent(TCHAR Char)
	{
		// Escapes rather than literal accented characters, so the file's own encoding can never
		// become the bug being tested for.
		switch (Char)
		{
		case TEXT('a'): return TCHAR(0x00E1); // a-acute
		case TEXT('e'): return TCHAR(0x00E9); // e-acute
		case TEXT('i'): return TCHAR(0x00ED); // i-acute
		case TEXT('o'): return TCHAR(0x00F6); // o-umlaut
		case TEXT('u'): return TCHAR(0x00FC); // u-umlaut
		case TEXT('A'): return TCHAR(0x00C4); // A-umlaut
		case TEXT('E'): return TCHAR(0x00C9); // E-acute
		case TEXT('O'): return TCHAR(0x00D6); // O-umlaut
		case TEXT('U'): return TCHAR(0x00DC); // U-umlaut
		default:        return Char;
		}
	}
};
