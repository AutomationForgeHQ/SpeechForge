// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"

#include "SpeechForgeAssetDefinitions.generated.h"

/**
 * Where SpeechForge's assets live in the Content Browser, and what opens them.
 *
 * Every data asset in the Automation Forge family gets one of these plus a factory - the family
 * convention, no exceptions. Without it the type is reachable only through
 * Miscellaneous > Data Asset and a class list, and a double-click opens a details panel with no
 * road back to the panel that actually operates on the thing.
 */
UCLASS()
class USpeechBankAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;

	/** Opens the Speech Library with this bank selected rather than a details panel. */
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};

/** A speaker keeps the default editor - it is a sheet of fields, and a details panel is that. */
UCLASS()
class USpeechSpeakerAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
};

/** A voice profile keeps the default editor for the same reason a speaker does. */
UCLASS()
class USpeechVoiceProfileAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
};

/** A single line outside any bank - same treatment as a speaker: category, colour, details. */
UCLASS()
class USpeechLineDefAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
};
