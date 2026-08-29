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

private:

	/** Give any line that arrived without an id a stable one derived from its text. */
	void EnsureLineIds();
};
