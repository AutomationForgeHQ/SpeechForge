#include "SpeechVoice.h"

FSpeechVoiceResolution USpeechVoice::MakeResolution(
	FName FallbackProviderId,
	const FString& FallbackModelId,
	const FString& InSourceDescription) const
{
	FSpeechVoiceResolution Resolution;

	Resolution.ProviderId = ProviderId.IsNone() ? FallbackProviderId : ProviderId;
	Resolution.ProviderVoiceId = ProviderVoiceId;
	Resolution.ModelId = ModelId.IsEmpty() ? FallbackModelId : ModelId;
	Resolution.Settings = Settings;
	Resolution.SourceDescription = InSourceDescription;

	return Resolution;
}

FString USpeechVoice::GetSetupProblem() const
{
	if (ProviderVoiceId.IsEmpty())
	{
		return FString::Printf(
			TEXT("'%s' has no Provider Voice Id. Pick a voice on the provider and paste its id here; ")
			TEXT("nothing can generate until it is paired."),
			*GetName());
	}

	if (Provenance == ESpeechVoiceProvenance::Unknown)
	{
		// Not fatal, so this is the only advisory this function returns. It matters because a stock
		// voice can be retired by its provider, and a library built on one goes with it - so "which
		// kind is this" is a question worth being able to answer before three thousand lines exist.
		return FString::Printf(
			TEXT("'%s' does not record how its voice was made. Set Provenance, so it is possible to ")
			TEXT("tell later whether this voice can be recreated."),
			*GetName());
	}

	return FString();
}
