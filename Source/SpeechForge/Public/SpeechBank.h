// Many lines in one asset. The default shape.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeechLine.h"
#include "SpeechSources.h"
#include "SpeechBank.generated.h"

/**
 * A set of speech lines - a scene, a character's barks, a quest's dialogue.
 *
 * The default container, because voice-over is bulk in a way most generated content is not. A game
 * has thousands of lines, no review step worth stopping for, and no per-line iteration ritual, so an
 * asset per line would bury the content browser for no benefit.
 *
 * The single-line asset exists for the cases where a bank is ceremony around one string; see
 * USpeechLineDef.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Bank"))
class SPEECHFORGE_API USpeechBank : public UDataAsset, public ISpeechLineSource
{
	GENERATED_BODY()

public:

	/** What this bank is for, for whoever finds it later. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bank", meta = (MultiLine = true))
	FString Description;

	/** Voice, model and speaker for every line here that does not name its own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bank")
	FSpeechLineDefaults Defaults;

	/** The lines, in authoring order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Bank", meta = (TitleProperty = "LineId"))
	TArray<FSpeechLine> Lines;

	// ---------------------------------------------------------------------------------------------
	// Provenance - where these lines came from, when they came from anywhere
	// ---------------------------------------------------------------------------------------------

	/**
	 * The adapter that harvested this bank, when one did. None means the bank is its own source.
	 *
	 * This is the key the two-way-sync contract matches on: a toolset function tagged
	 * `SpeechLineSync` names the adapter it can write back through, the Speech Library offers it
	 * only on banks stamped with the same key, and core never learns what the key means. An edit
	 * made here and not written back is overwritten by the next harvest - the stamp is what lets
	 * the panel say so instead of letting it happen silently.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	FName SourceAdapter;

	/**
	 * Content path of the asset the lines were read from. Empty when the bank is its own source.
	 *
	 * Searchable so that the startup drift sweep can find every adapter-sourced bank in the project
	 * without loading a single one - the asset registry answers from its cache, and only the banks
	 * that actually have a source are ever opened.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Source")
	FString SourceAssetPath;

	/**
	 * Which speakers this bank harvests from its source, ";"-joined. Empty means all of them.
	 *
	 * The per-character division: one scene's dialogue can split across banks by actor, and this
	 * records which slice is this bank's. Re-harvest applies it, so pulling new script lines never
	 * drags another character's lines into a bank that was deliberately scoped.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	FString SourceSpeakerFilter;

	/**
	 * When the source asset was last read into this bank.
	 *
	 * The source's own file time at harvest, not the wall clock, so the comparison that follows is
	 * between two facts of the same kind. A dialogue written after this moment means somebody edited
	 * the script somewhere this bank cannot see, and every line here may now disagree with it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	FDateTime SourceHarvestedAt = FDateTime();

	// ---------------------------------------------------------------------------------------------
	// Localisation - one bank per language, joined by line id
	//
	// A sibling bank per language rather than per-line language maps, because everything downstream
	// - hashes, takes, face banks, dialogue assignment - already speaks "a bank and a line id", and
	// a per-language bank means none of it learns anything new. The face bank for a language comes
	// free the same way: face banks key themselves to a speech bank by path.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Language these lines are written and spoken in, e.g. "de" or "pt-BR".
	 *
	 * Empty on an authoring bank, which is what marks it as the source of truth. Searchable so
	 * every localised bank in a project is enumerable from the registry cache without loading one.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Localisation")
	FString LanguageCode;

	/**
	 * Content path of the bank this one was translated from. Empty on an authoring bank.
	 *
	 * Searchable for the reverse question - which localisations does this bank have - answered from
	 * the registry cache, exactly the way face banks find their speech bank.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Localisation")
	FString SourceBankPath;

	/**
	 * Every line id here, ";"-wrapped and ";"-joined (";BRK_Arrival;BRK_DepotLocked;"), rebuilt on
	 * save.
	 *
	 * The one-line-one-home contract's index: a line identity is (LineId, LanguageCode) and exactly
	 * one bank may hold it - its home. This tag lets "who is home for line X?" be answered from the
	 * registry cache without loading a single bank, which is what makes ingest able to refuse
	 * duplicates cheaply. Wrapped in delimiters so the match is by whole token - "BRK_Arrival" must
	 * not be found inside "BRK_Arrival_Alt1".
	 */
	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "Localisation")
	FString LineIdIndex;

	// ---------------------------------------------------------------------------------------------
	// ISpeechLineSource
	// ---------------------------------------------------------------------------------------------

	virtual void GetLineIds(TArray<FName>& OutLineIds) const override;
	virtual const FSpeechLine* FindLine(FName LineId) const override;
	virtual FSpeechLine* FindLineMutable(FName LineId) override;
	virtual FSpeechLineDefaults GetLineDefaults() const override { return Defaults; }
	virtual int32 GetLineCount() const override { return Lines.Num(); }

	// ---------------------------------------------------------------------------------------------
	// Authoring
	// ---------------------------------------------------------------------------------------------

	/**
	 * Add a line, or overwrite the authoring fields of one that already has this id.
	 *
	 * Updating rather than duplicating is deliberate: re-running an authoring script must not produce
	 * a second copy of every line, and an existing line keeps the audio and the request id it has
	 * already paid for.
	 *
	 * @return the id used, which is generated from the text when the spec left it empty.
	 */
	FName AddOrUpdateLine(const FSpeechLineSpec& Spec);

	/** Lines whose id would collide, reported rather than silently suffixed. */
	bool ValidateLineIds(TArray<FName>& OutDuplicates) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Rebuilds LineIdIndex, so the registry tag always says what the saved bank holds. */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;

private:

	/** Give any line that arrived without an id a stable one derived from its text. */
	void EnsureLineIds();
};
