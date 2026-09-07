// The seam lines come from.
//
// There used to be a second seam here - ISpeechVoiceSource, an external resolver consulted between
// a line's override and its bank default. It is gone on purpose: the speaker asset (USpeechSpeaker)
// is now the one place a speaker's voice consolidates, and adapters *seed and link* speaker sheets
// rather than competing with them at resolution time. One place to look, one place to edit.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SpeechForgeTypes.h"
#include "SpeechLine.h"
#include "SpeechSources.generated.h"

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class USpeechLineSource : public UInterface
{
	GENERATED_BODY()
};

/**
 * Anything that holds speech lines.
 *
 * Two assets implement this today - a bank holding many lines and a single-line asset holding one -
 * and nothing downstream special-cases either. That is the point: the subsystem, the cost estimator,
 * the batch runner and the tools all address a line as a handle and never learn what kind of asset
 * it came out of.
 *
 * **This is the expansion seam the whole shape was chosen for.** A third container - a dialogue asset
 * read directly by an adapter plugin, a CSV importer, a localisation table - joins by implementing
 * this interface, and none of the machinery above has to change or even be recompiled.
 */
class SPEECHFORGE_API ISpeechLineSource
{
	GENERATED_BODY()

public:

	/** Every line this container holds, in authoring order. */
	virtual void GetLineIds(TArray<FName>& OutLineIds) const = 0;

	/** The line with this id, or null. Pass None to mean "the only line" in a single-line asset. */
	virtual const FSpeechLine* FindLine(FName LineId) const = 0;

	virtual FSpeechLine* FindLineMutable(FName LineId) = 0;

	/** Voice and model for lines here that do not name their own. */
	virtual FSpeechLineDefaults GetLineDefaults() const = 0;

	/** How many lines this holds. */
	virtual int32 GetLineCount() const
	{
		TArray<FName> Ids;
		GetLineIds(Ids);
		return Ids.Num();
	}
};
