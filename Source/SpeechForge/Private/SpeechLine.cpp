#include "SpeechLine.h"

#include "SpeechForge.h"
#include "Misc/SecureHash.h"

void FSpeechLine::ApplySpec(const FSpeechLineSpec& Spec)
{
	// Authoring only. Status, origin, hash, request id, audio and alignment are pipeline state and
	// survive a re-author untouched - re-running a script that defines a library must not throw away
	// the takes it has already paid for.
	if (!Spec.LineId.IsNone())
	{
		LineId = Spec.LineId;
	}

	Text = Spec.Text;
	Direction = Spec.Direction;

	if (!Spec.SpeakerId.IsNone())
	{
		SpeakerId = Spec.SpeakerId;
	}

	VoiceOverride = Spec.VoiceOverride;
	ModelOverride = Spec.ModelOverride;
	SeedOverride = Spec.SeedOverride;
	PreviousLineId = Spec.PreviousLineId;

	if (LineId.IsNone())
	{
		LineId = MakeLineIdFromText(SpeakerId, Text);
	}
}

FSpeechLineSpec FSpeechLine::ToSpec() const
{
	FSpeechLineSpec Spec;
	Spec.LineId = LineId;
	Spec.Text = Text;
	Spec.Direction = Direction;
	Spec.SpeakerId = SpeakerId;
	Spec.VoiceOverride = VoiceOverride;
	Spec.ModelOverride = ModelOverride;
	Spec.SeedOverride = SeedOverride;
	Spec.PreviousLineId = PreviousLineId;
	return Spec;
}

FString FSpeechLine::ComputeContentHash(
	const FString& RequestText,
	const FSpeechVoiceResolution& Voice,
	int32 Seed)
{
	// Over the request that will actually be sent, never over the authoring that produced it.
	//
	// Two consequences, both wanted. A field the provider ignores - direction, on a provider with no
	// inline tags - never reaches this string, so editing it correctly leaves lines current. And a
	// voice resolved through some other plugin is hashed as the concrete voice it resolved to, so
	// repointing a speaker elsewhere marks lines stale rather than leaving them silently spoken by
	// the wrong character.
	const FString Combined = FString::Printf(TEXT("v1|%s|%s|seed=%d"),
		*RequestText,
		*Voice.ToHashString(),
		Seed);

	// Hash the UTF-8 bytes rather than the TCHARs, so the same line hashes identically regardless of
	// how the platform stores wide characters.
	const FTCHARToUTF8 Utf8(*Combined);

	FSHA1 Sha;
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();

	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);

	return BytesToHex(Digest, FSHA1::DigestSize);
}

FName FSpeechLine::MakeLineIdFromText(FName InSpeakerId, const FString& InText)
{
	// {Speaker}_{FirstFourWords}, matching the convention Narrative already uses for dialogue node
	// ids. Worth matching rather than inventing: when an adapter harvests lines out of a dialogue
	// graph the two schemes have to agree, and one of them is not ours to change.
	TArray<FString> Words;
	InText.ParseIntoArrayWS(Words);

	FString Body;
	const int32 Count = FMath::Min(Words.Num(), 4);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FString Word = Words[Index];

		// Strip anything that would make an awkward FName in a content path or a log line.
		Word.RemoveFromEnd(TEXT("."));
		Word = Word.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
		Word.TrimStartAndEndInline();

		if (Word.IsEmpty())
		{
			continue;
		}

		Word[0] = FChar::ToUpper(Word[0]);
		Body += Word;
	}

	if (Body.IsEmpty())
	{
		Body = TEXT("Line");
	}

	const FString Prefix = InSpeakerId.IsNone() ? FString() : InSpeakerId.ToString() + TEXT("_");
	return FName(*(Prefix + Body));
}
