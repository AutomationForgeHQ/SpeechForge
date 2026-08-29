#include "SpeechAlignment.h"

#include "SpeechForge.h"

bool FSpeechAlignment::Validate(FString& OutError) const
{
	if (Text.IsEmpty())
	{
		OutError = TEXT("Alignment has no text, so its timings index nothing.");
		return false;
	}

	if (CharacterStartSeconds.Num() != CharacterEndSeconds.Num())
	{
		OutError = FString::Printf(
			TEXT("Alignment start and end arrays disagree (%d vs %d)."),
			CharacterStartSeconds.Num(), CharacterEndSeconds.Num());
		return false;
	}

	if (CharacterStartSeconds.Num() != Text.Len())
	{
		// This is the signature of having read the wrong alignment variant. Providers return timings
		// against both the text as written and the text after normalisation - where "3" has become
		// "three" - and the two differ in length only for lines containing numbers or abbreviations.
		// Read the wrong one and subtitles are perfect until the first line that says "airlock 3".
		//
		// It can also mean the text carries characters outside the basic plane, where the provider
		// counts code points and FString counts UTF-16 units. Same symptom, different cause, and both
		// are worth stopping for rather than importing.
		OutError = FString::Printf(
			TEXT("Alignment has %d timings for %d characters of text. Either the wrong alignment ")
			TEXT("variant was read (normalised vs original), or the text contains characters the ")
			TEXT("provider and the engine count differently."),
			CharacterStartSeconds.Num(), Text.Len());
		return false;
	}

	for (int32 Index = 0; Index < CharacterStartSeconds.Num(); ++Index)
	{
		if (CharacterEndSeconds[Index] < CharacterStartSeconds[Index])
		{
			OutError = FString::Printf(
				TEXT("Alignment character %d ends (%.3f) before it starts (%.3f)."),
				Index, CharacterEndSeconds[Index], CharacterStartSeconds[Index]);
			return false;
		}
	}

	OutError.Reset();
	return true;
}

void FSpeechAlignment::DeriveWordsAndDuration()
{
	Words.Reset();
	DurationSeconds = 0.f;

	if (CharacterEndSeconds.Num() == 0 || CharacterEndSeconds.Num() != Text.Len())
	{
		return;
	}

	// The last character's end is the spoken length. Deliberately not the maximum over the array:
	// if timings ever come back out of order that is a fault worth surfacing through the duration
	// cross-check rather than papering over here.
	DurationSeconds = CharacterEndSeconds.Last();

	int32 WordStartIndex = INDEX_NONE;

	auto FlushWord = [this, &WordStartIndex](int32 EndIndexExclusive)
	{
		if (WordStartIndex == INDEX_NONE)
		{
			return;
		}

		FSpeechWordTiming Timing;
		Timing.CharacterIndex = WordStartIndex;
		Timing.Word = Text.Mid(WordStartIndex, EndIndexExclusive - WordStartIndex);
		Timing.StartSeconds = CharacterStartSeconds[WordStartIndex];
		Timing.EndSeconds = CharacterEndSeconds[EndIndexExclusive - 1];

		Words.Add(MoveTemp(Timing));
		WordStartIndex = INDEX_NONE;
	};

	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const bool bIsSpace = FChar::IsWhitespace(Text[Index]);

		if (bIsSpace)
		{
			FlushWord(Index);
		}
		else if (WordStartIndex == INDEX_NONE)
		{
			WordStartIndex = Index;
		}
	}

	FlushWord(Text.Len());
}

float FSpeechAlignment::FindWordStart(int32 CharacterIndex) const
{
	for (const FSpeechWordTiming& Timing : Words)
	{
		const int32 End = Timing.CharacterIndex + Timing.Word.Len();
		if (CharacterIndex >= Timing.CharacterIndex && CharacterIndex < End)
		{
			return Timing.StartSeconds;
		}
	}

	return -1.f;
}
