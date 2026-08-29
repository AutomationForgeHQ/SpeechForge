// The two seams: where lines come from, and where voices come from.

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

/**
 * Anything that can decide which voice a line is spoken in.
 *
 * A plain C++ interface registered on the module rather than a UInterface, for the same reason
 * providers are: a resolver is a service belonging to a plugin, not a property of an asset, and it
 * has to be findable before any particular asset is loaded.
 *
 * Sources are consulted in descending priority and the first to answer wins. The built-in source
 * reads the line's own override and its container's defaults; an adapter plugin registers a higher
 * or lower priority one to map speakers onto voices from wherever it likes. SpeechForge never learns
 * what a source reads from, so deleting the plugin that registered one changes nothing but which
 * voices resolve.
 */
class SPEECHFORGE_API ISpeechVoiceSource
{
public:

	virtual ~ISpeechVoiceSource() = default;

	/** Stable identifier, e.g. "NP_VoiceOver". Registering the same id twice replaces the first. */
	virtual FName GetVoiceSourceId() const = 0;

	/**
	 * Higher is consulted first.
	 *
	 * The built-in source that reads a line's own override sits at 1000, so an adapter wanting to be
	 * overridable by hand should sit below it, and one that must win should sit above and say why.
	 */
	virtual int32 GetPriority() const = 0;

	/**
	 * Answer the query, or decline.
	 *
	 * @return false to pass the question to the next source. Returning true with an invalid
	 *         resolution is a bug - decline instead, so a later source still gets its turn.
	 *
	 * Fill in SourceDescription. It is what a human or an agent reads when a line comes out in the
	 * wrong voice, and "open four assets and guess" is not an acceptable answer to that question.
	 */
	virtual bool ResolveVoice(const FSpeechVoiceQuery& Query, FSpeechVoiceResolution& OutResolution) const = 0;
};
