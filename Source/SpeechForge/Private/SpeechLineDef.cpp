#include "SpeechLineDef.h"

void USpeechLineDef::GetLineIds(TArray<FName>& OutLineIds) const
{
	OutLineIds.Reset(1);
	OutLineIds.Add(Line.LineId);
}

const FSpeechLine* USpeechLineDef::FindLine(FName LineId) const
{
	// None means "the only line there is", which is how a handle to this asset normally arrives. An
	// explicit id still matches, so a caller that happens to know it is not punished for saying so.
	return (LineId.IsNone() || LineId == Line.LineId) ? &Line : nullptr;
}

FSpeechLine* USpeechLineDef::FindLineMutable(FName LineId)
{
	return (LineId.IsNone() || LineId == Line.LineId) ? &Line : nullptr;
}

#if WITH_EDITOR

void USpeechLineDef::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (Line.LineId.IsNone() && !Line.Text.IsEmpty())
	{
		const FName SpeakerForId = Line.SpeakerId.IsNone() ? Defaults.SpeakerId : Line.SpeakerId;
		Line.LineId = FSpeechLine::MakeLineIdFromText(SpeakerForId, Line.Text);
	}
}

#endif
