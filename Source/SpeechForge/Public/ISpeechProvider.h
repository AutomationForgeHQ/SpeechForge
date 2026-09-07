// What any speech generation service has to be able to do.

#pragma once

#include "CoreMinimal.h"
#include "SpeechForgeTypes.h"
#include "SpeechAlignment.h"

/** One synthesis request, fully resolved. Nothing here is a reference to be looked up later. */
struct FSpeechSynthesisRequest
{
	/**
	 * The string to speak, with direction already merged in or left out.
	 *
	 * Built by BuildRequestText, so that a provider which cannot use direction never sees it - which
	 * is also what makes editing direction correctly fail to mark anything stale on that provider.
	 */
	FString RequestText;

	/** What the player reads. Carried so alignment can be reported against it. */
	FString DisplayText;

	FSpeechVoiceResolution Voice;

	/** Negative for none. Providers that cannot seed ignore it and log rather than failing. */
	int32 Seed = -1;

	/** Stitching context. Ignored where bSupportsStitching is false. */
	FString PreviousText;
	FString NextText;
	FString PreviousRequestId;

	/** Where the provider should write its audio. Absolute path, extension already correct. */
	FString AbsoluteOutputPath;
};

/** What came back. */
struct FSpeechSynthesisResult
{
	bool bSuccess = false;

	/** Absolute path of the file written. */
	FString AbsoluteAudioPath;

	/** Container the audio arrived in, without the dot. Asserted against what was asked for. */
	FString AudioFormat;

	/**
	 * The rate the audio actually arrived at.
	 *
	 * Read off the file rather than assumed. Providers gate higher fidelity behind a subscription
	 * tier and do not error when it is unavailable - they return something else, which imports
	 * cleanly and is quietly worse.
	 */
	int32 SampleRate = 0;

	/** Empty where the provider does not do timings. */
	FSpeechAlignment Alignment;

	/**
	 * The provider's handle to this generation.
	 *
	 * Where history is supported this re-fetches the identical audio for free. Treat it as the only
	 * durable route back to a take that a seed cannot reproduce.
	 */
	FString RequestId;

	/** What this actually cost, as reported rather than as estimated. */
	int32 BilledCharacters = 0;

	FString Error;
};

/** One voice the provider holds. */
struct FSpeechRemoteVoice
{
	FString Id;
	FString Name;
	FString Description;

	/** Stock voices are worth flagging: a provider can retire them, taking your library with them. */
	bool bIsPremade = false;

	/**
	 * A short audio sample of the voice, where the provider offers one. Empty otherwise, and a UI
	 * shows no play button rather than a dead one.
	 */
	FString PreviewUrl;
};

/**
 * One conversion: an existing performance re-voiced, keeping the delivery and changing the voice.
 *
 * Deliberately not a synthesis request with a file glued on. Nothing here is text - no script, no
 * direction, no seed - because a conversion is not a reading of words; it is a recording wearing a
 * different identity. Providers that cannot do it decline, and say so through
 * FSpeechProviderCaps::bSupportsVoiceConversion.
 */
struct FSpeechConversionRequest
{
	/** Absolute path of the audio to convert - the source performance, whatever produced it. */
	FString AbsoluteSourcePath;

	/** The voice it becomes. Resolved before the call, never a reference. */
	FSpeechVoiceResolution Voice;

	/** Empty uses the provider's conversion default, which is rarely its synthesis model. */
	FString ModelId;

	/** Absolute path the converted audio is written to. */
	FString AbsoluteOutputPath;
};

/**
 * One dub: a recording carried into another language, delivery and voice intact.
 *
 * Like a conversion, this is not a synthesis request with a file glued on - no script and no seed,
 * because the words come out of the recording itself. The one text-shaped field is the language
 * pair, and the timing contract is the point: a dub fits the translated speech into the original's
 * pacing, which is what keeps every layer aligned downstream.
 */
struct FSpeechDubbingRequest
{
	/** Absolute path of the audio to dub - the source performance. */
	FString AbsoluteSourcePath;

	/** BCP-47-ish code of the source language ("en"). Empty lets the service detect it. */
	FString SourceLanguage;

	/** BCP-47-ish code of the target language ("de"). Never empty. */
	FString TargetLanguage;

	/** Absolute path the dubbed audio is written to. Extension decides the expected container. */
	FString AbsoluteOutputPath;
};

using FOnSpeechSynthesized     = TFunction<void(const FSpeechSynthesisResult&)>;
using FOnSpeechTestComplete    = TFunction<void(bool /*bSuccess*/, const FString& /*Message*/)>;
using FOnSpeechVoicesListed    =
	TFunction<void(bool /*bSuccess*/, const TArray<FSpeechRemoteVoice>& /*Voices*/, const FString& /*Error*/)>;

/**
 * A speech generation service.
 *
 * Deliberately narrow: synthesize, re-fetch, list voices, and enough metadata to drive a UI and an
 * estimate. Anything specific to one vendor - REST shapes, how a voice id looks, which model names
 * exist, whether direction is spelled as inline tags or a style vector - stays behind this line so
 * the pipeline above never learns about it.
 *
 * There is no submit-and-poll here, and that is a real difference from the motion pipeline rather
 * than an omission: hosted speech returns audio in the response. A provider that genuinely needs a
 * job can hide it inside Synthesize, which is asynchronous anyway.
 *
 * Every call completes on the game thread.
 */
class SPEECHFORGE_API ISpeechProvider
{
public:

	virtual ~ISpeechProvider() = default;

	/** Stable identifier used in settings and voice assets, e.g. "ElevenLabs". */
	virtual FName GetProviderId() const = 0;

	virtual FString GetDisplayName() const = 0;

	/**
	 * What this provider can do, so nothing above has to special-case it by name.
	 *
	 * Read rather than assumed. Every field on FSpeechProviderCaps is a difference already known to
	 * exist between candidate providers, and each would otherwise become an assumption baked in for
	 * whichever one happened to be implemented first.
	 */
	virtual FSpeechProviderCaps GetCaps() const = 0;

	/** Model used when nothing names one. */
	virtual FString GetDefaultModelId() const = 0;

	/**
	 * Longest text this model will accept, in characters.
	 *
	 * Per model rather than per provider, because they differ by an order of magnitude on the same
	 * account. A line over the limit has to be split and stitched, not truncated.
	 */
	virtual int32 GetCharacterLimit(const FString& ModelId) const = 0;

	/** Container this provider's audio arrives in, without the dot. */
	virtual FString GetAudioFormat() const { return TEXT("wav"); }

	/**
	 * Whether *this model* accepts surrounding lines as context.
	 *
	 * Separate from the provider-level capability because it is genuinely a per-model answer, and
	 * discovered the hard way: the expressive model that takes inline direction rejects stitching
	 * outright, while the model that stitches has no audio tags. One provider, two models, opposite
	 * answers - so asking the provider once and caching it would be wrong for half the project.
	 *
	 * The default defers to the provider-wide flag, which is right for anything uniform.
	 */
	virtual bool SupportsStitchingForModel(const FString& ModelId) const
	{
		return GetCaps().bSupportsStitching;
	}

	/**
	 * Merge direction into the text to be spoken.
	 *
	 * The default drops direction entirely, which is right for a provider that cannot use it.
	 * Overriding this is how a provider says "I take inline tags" - and because the content hash is
	 * taken over the result, a provider that ignores direction correctly reports lines as current
	 * when only the direction changed.
	 */
	virtual FString BuildRequestText(const FString& Text, const FString& Direction) const
	{
		return Text;
	}

	/**
	 * Which plugin this provider ships in, for the shared Keys page.
	 *
	 * Not SpeechForge, for anything that actually has a key: the registration lives in the core,
	 * because that is where providers announce themselves - so without this every key would be
	 * attributed to SpeechForge, which is a core and spends nothing. Somebody uninstalling the
	 * plugin that owns a key would go looking for the wrong one.
	 *
	 * Defaults to SpeechForge so a provider that forgets is merely unhelpful rather than wrong: a
	 * provider compiled into the core genuinely would belong to it.
	 */
	virtual FText GetOwningPluginName() const { return NSLOCTEXT("SpeechForge", "OwnerCore", "SpeechForge"); }

	/** Service name this provider's secret is stored under in the credential vault. */
	virtual FString GetCredentialServiceName() const = 0;

	/** True when a usable credential is available. */
	virtual bool HasCredential() const = 0;

	/**
	 * Where a person signs up for a key, shown beside the field that asks for one.
	 * Optional — a provider that needs no account leaves it empty.
	 */
	virtual FString GetCredentialHelpUrl() const { return FString(); }

	/** Generate one line. This is the call that costs money, and it costs it immediately. */
	virtual void Synthesize(const FSpeechSynthesisRequest& Request, FOnSpeechSynthesized OnComplete) = 0;

	/**
	 * Re-voice an existing recording. Spends money the moment it is sent, exactly like synthesis.
	 *
	 * The result returns through the synthesis callback on purpose: what a caller needs afterwards
	 * is identical - a file on disk and the billing that produced it - and two result types would
	 * only force every caller to learn both.
	 */
	virtual void ConvertSpeech(const FSpeechConversionRequest& Request, FOnSpeechSynthesized OnComplete)
	{
		FSpeechSynthesisResult Result;
		Result.Error = TEXT("This provider cannot re-voice a recording.");
		OnComplete(Result);
	}

	/**
	 * Dub a recording into another language, keeping the voice and the pacing.
	 *
	 * Spends money the moment it is sent, by the minute. Long-running on the provider's side -
	 * implementations hide their own polling inside this call, exactly the way a job-shaped
	 * Synthesize would, and complete on the game thread like everything else.
	 */
	virtual void DubSpeech(const FSpeechDubbingRequest& Request, FOnSpeechSynthesized OnComplete)
	{
		FSpeechSynthesisResult Result;
		Result.Error = TEXT("This provider cannot dub a recording into another language.");
		OnComplete(Result);
	}

	/**
	 * Fetch a past generation again by its request id, without regenerating.
	 *
	 * Free where supported, and the only way to recover audio from a provider that cannot reproduce
	 * a generation from a seed. Providers answering false to bSupportsRemoteHistory leave this alone.
	 */
	virtual void RefetchById(
		const FString& RequestId,
		const FString& AbsoluteOutputPath,
		FOnSpeechSynthesized OnComplete)
	{
		FSpeechSynthesisResult Result;
		Result.Error = TEXT("This provider cannot re-fetch past generations.");
		OnComplete(Result);
	}

	/** Cheapest possible authenticated call, for a connection test. Costs nothing, generates nothing. */
	virtual void TestConnection(FOnSpeechTestComplete OnComplete) = 0;

	/** Voices this account holds, so one can be paired without leaving the editor. */
	virtual void ListVoices(FOnSpeechVoicesListed OnComplete)
	{
		OnComplete(false, {}, TEXT("This provider cannot list voices."));
	}

	/**
	 * Where a human adds voices to this provider's account, when there is such a place.
	 *
	 * Optional, and contextual by construction: the guidance ships with the provider plugin that it
	 * is about, the panel renders whatever the active provider declares, and core names nobody.
	 * Empty means the browser shows no link.
	 */
	virtual FString GetVoiceLibraryUrl() const { return FString(); }

	/** One or two sentences telling a human how voices get onto this account, shown with the link. */
	virtual FText GetVoiceLibraryHint() const { return FText::GetEmpty(); }

	// ---------------------------------------------------------------------------------------------
	// Deliberately absent
	//
	// There is no CreateVoice and no DeleteVoice. Creating a voice is a human act performed once, and
	// a key that can create one can usually delete one - which loses every line that voice ever spoke
	// and cannot be undone. That is the same boundary as never writing a credential: the operations
	// where speed converts a small mistake into an irreversible one do not get an automated surface.
	// ---------------------------------------------------------------------------------------------
};
