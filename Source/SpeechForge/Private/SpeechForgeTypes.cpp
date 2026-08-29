#include "SpeechForgeTypes.h"

namespace SpeechForgeHash
{
	/**
	 * Fixed precision, so a float that round-trips through a details panel does not change a hash.
	 *
	 * Four decimals is well beyond anything audible and well within what a UPROPERTY slider produces,
	 * which is the point: the hash has to be stable against edits that are not edits.
	 */
	static FString Num(float Value)
	{
		return FString::Printf(TEXT("%.4f"), Value);
	}
}

FString FSpeechVoiceSettings::ToHashString() const
{
	TArray<FName> Keys;
	ProviderOverrides.GetKeys(Keys);

	// Sorted, because a TMap's iteration order is not stable across runs and an unsorted hash would
	// mark lines stale at random.
	Keys.Sort(FNameLexicalLess());

	FString Overrides;
	for (const FName Key : Keys)
	{
		Overrides += FString::Printf(TEXT("%s=%s;"), *Key.ToString(), *ProviderOverrides[Key]);
	}

	return FString::Printf(TEXT("st=%s|si=%s|sy=%s|sp=%s|ov=%s"),
		*SpeechForgeHash::Num(Stability),
		*SpeechForgeHash::Num(Similarity),
		*SpeechForgeHash::Num(StyleIntensity),
		*SpeechForgeHash::Num(Speed),
		*Overrides);
}

FString FSpeechVoiceResolution::ToHashString() const
{
	// SourceDescription is deliberately absent. It records *why* a voice resolved the way it did,
	// which is diagnostic - including it would mark every line stale the moment an adapter reworded
	// its own explanation of itself.
	return FString::Printf(TEXT("p=%s|v=%s|m=%s|%s"),
		*ProviderId.ToString(),
		*ProviderVoiceId,
		*ModelId,
		*Settings.ToHashString());
}

FString FSpeechVoiceResolution::DescribeDifference(const FSpeechVoiceResolution& Other) const
{
	TArray<FString> Differences;

	if (ProviderId != Other.ProviderId)
	{
		Differences.Add(FString::Printf(TEXT("provider %s -> %s"),
			*Other.ProviderId.ToString(), *ProviderId.ToString()));
	}

	if (ProviderVoiceId != Other.ProviderVoiceId)
	{
		// Worth naming loudly. A changed voice id means the line is spoken by somebody else, which is
		// a different kind of problem from a tweaked setting.
		Differences.Add(FString::Printf(TEXT("voice %s -> %s"),
			*Other.ProviderVoiceId, *ProviderVoiceId));
	}

	if (ModelId != Other.ModelId)
	{
		Differences.Add(FString::Printf(TEXT("model %s -> %s"), *Other.ModelId, *ModelId));
	}

	auto CompareFloat = [&Differences](const TCHAR* Name, float Old, float New)
	{
		if (!FMath::IsNearlyEqual(Old, New, KINDA_SMALL_NUMBER))
		{
			Differences.Add(FString::Printf(TEXT("%s %.2f -> %.2f"), Name, Old, New));
		}
	};

	CompareFloat(TEXT("stability"), Other.Settings.Stability, Settings.Stability);
	CompareFloat(TEXT("similarity"), Other.Settings.Similarity, Settings.Similarity);
	CompareFloat(TEXT("style"), Other.Settings.StyleIntensity, Settings.StyleIntensity);
	CompareFloat(TEXT("speed"), Other.Settings.Speed, Settings.Speed);

	if (Settings.ProviderOverrides.OrderIndependentCompareEqual(Other.Settings.ProviderOverrides) == false)
	{
		Differences.Add(TEXT("provider overrides"));
	}

	return FString::Join(Differences, TEXT(", "));
}
