#include "SpeechBank.h"

#include "SpeechForge.h"
#include "UObject/ObjectSaveContext.h"

void USpeechBank::PreSave(FObjectPreSaveContext SaveContext)
{
	// The home index rides every save, so the registry tag can never disagree with the asset for
	// longer than an unsaved edit. ";"-wrapped tokens, so lookups match whole ids only.
	FString Index = TEXT(";");
	for (const FSpeechLine& Line : Lines)
	{
		if (!Line.LineId.IsNone())
		{
			Index += Line.LineId.ToString() + TEXT(";");
		}
	}
	LineIdIndex = Lines.Num() > 0 ? Index : FString();

	Super::PreSave(SaveContext);
}

void USpeechBank::GetLineIds(TArray<FName>& OutLineIds) const
{
	OutLineIds.Reset(Lines.Num());
	for (const FSpeechLine& Line : Lines)
	{
		OutLineIds.Add(Line.LineId);
	}
}

const FSpeechLine* USpeechBank::FindLine(FName LineId) const
{
	return Lines.FindByPredicate([LineId](const FSpeechLine& Line)
	{
		return Line.LineId == LineId;
	});
}

FSpeechLine* USpeechBank::FindLineMutable(FName LineId)
{
	return Lines.FindByPredicate([LineId](const FSpeechLine& Line)
	{
		return Line.LineId == LineId;
	});
}

FName USpeechBank::AddOrUpdateLine(const FSpeechLineSpec& Spec)
{
	FSpeechLineSpec Resolved = Spec;

	if (Resolved.SpeakerId.IsNone())
	{
		Resolved.SpeakerId = Defaults.SpeakerId;
	}

	if (Resolved.LineId.IsNone())
	{
		Resolved.LineId = FSpeechLine::MakeLineIdFromText(Resolved.SpeakerId, Resolved.Text);
	}

	if (FSpeechLine* Existing = FindLineMutable(Resolved.LineId))
	{
		// Updated rather than duplicated. Re-running an authoring script must not produce a second
		// copy of every line, and an existing line keeps the audio and the request id it has already
		// been paid for.
		Existing->ApplySpec(Resolved);
		return Existing->LineId;
	}

	FSpeechLine NewLine;
	NewLine.ApplySpec(Resolved);
	Lines.Add(MoveTemp(NewLine));

	return Resolved.LineId;
}

bool USpeechBank::ValidateLineIds(TArray<FName>& OutDuplicates) const
{
	OutDuplicates.Reset();

	TSet<FName> Seen;
	for (const FSpeechLine& Line : Lines)
	{
		if (Line.LineId.IsNone())
		{
			continue;
		}

		if (Seen.Contains(Line.LineId))
		{
			OutDuplicates.AddUnique(Line.LineId);
		}
		else
		{
			Seen.Add(Line.LineId);
		}
	}

	return OutDuplicates.Num() == 0;
}

void USpeechBank::EnsureLineIds()
{
	for (FSpeechLine& Line : Lines)
	{
		if (Line.LineId.IsNone() && !Line.Text.IsEmpty())
		{
			const FName SpeakerForId = Line.SpeakerId.IsNone() ? Defaults.SpeakerId : Line.SpeakerId;
			Line.LineId = FSpeechLine::MakeLineIdFromText(SpeakerForId, Line.Text);
		}
	}
}

#if WITH_EDITOR

void USpeechBank::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Only fills in ids that are missing. An id that already exists is never regenerated from the
	// text, because a line id that follows its text would break every reference the moment somebody
	// fixed a typo - and the audio, the request id and the hash are all addressed by it.
	EnsureLineIds();

	TArray<FName> Duplicates;
	if (!ValidateLineIds(Duplicates))
	{
		// Reported rather than silently suffixed. A duplicate id means two lines share one handle, so
		// generating either writes over the other's result - and quietly renaming one of them would
		// orphan whichever reference happened to point at it.
		TArray<FString> Names;
		for (const FName Id : Duplicates)
		{
			Names.Add(Id.ToString());
		}

		UE_LOG(LogSpeechForge, Warning,
			TEXT("'%s' has duplicate line ids: %s. Two lines sharing an id share a result - rename one."),
			*GetName(), *FString::Join(Names, TEXT(", ")));
	}
}

#endif
