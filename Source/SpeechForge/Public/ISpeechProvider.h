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

	// ---------------------------------------------------------------------------------------------
	// Deliberately absent
	//
	// There is no CreateVoice and no DeleteVoice. Creating a voice is a human act performed once, and
	// a key that can create one can usually delete one - which loses every line that voice ever spoke
	// and cannot be undone. That is the same boundary as never writing a credential: the operations
	// where speed converts a small mistake into an irreversible one do not get an automated surface.
	// ---------------------------------------------------------------------------------------------
};
