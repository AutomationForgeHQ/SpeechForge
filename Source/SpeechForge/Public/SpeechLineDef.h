// One line in its own asset. The simple case.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeechLine.h"
#include "SpeechSources.h"
#include "SpeechLineDef.generated.h"

/**
 * A single speech line as its own asset.
 *
 * Inferior to a bank for a script, and genuinely better for a one-off - a UI confirmation, a system
 * announcement, a test - where a bank is ceremony around one string, and where being able to
 * reference the line directly from another asset is worth more than being able to batch it.
 *
 * Implements the same interface as a bank, so every tool, estimate and batch treats the two
 * identically. A handle to a line here carries no LineId.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Line"))
class SPEECHFORGE_API USpeechLineDef : public UDataAsset, public ISpeechLineSource
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Line", meta = (ShowOnlyInnerProperties))
	FSpeechLine Line;

	/** Voice and model, when the line does not name its own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Line")
	FSpeechLineDefaults Defaults;

	// ---------------------------------------------------------------------------------------------
	// ISpeechLineSource
	//
	// A handle to this asset normally carries no LineId, so None resolves to the only line there is.
	// An explicit id still matches, so a caller that happens to know it is not punished for saying so.
	// ---------------------------------------------------------------------------------------------

	virtual void GetLineIds(TArray<FName>& OutLineIds) const override;
	virtual const FSpeechLine* FindLine(FName LineId) const override;
	virtual FSpeechLine* FindLineMutable(FName LineId) override;
	virtual FSpeechLineDefaults GetLineDefaults() const override { return Defaults; }
	virtual int32 GetLineCount() const override { return 1; }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
