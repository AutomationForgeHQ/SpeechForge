// Copyright Blackcode SA. All rights reserved.
//
// What makes these types appear under right-click > Automation Forge in the Content Browser.
//
// Without a factory a UDataAsset can still be created - through Miscellaneous > Data Asset, then
// picking the class out of a list of every data asset class in the project. That is the path a
// person finds after being told it exists, which is to say not at all. An asset definition decides
// what a double-click opens; a factory is what decides the thing can be made in the first place.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "SpeechForgeFactories.generated.h"

/** Creates a Speech Bank - lines are then harvested into it from a dialogue, or written by hand. */
UCLASS()
class USpeechBankFactory : public UFactory
{
	GENERATED_BODY()

public:

	USpeechBankFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};

/** Creates a Speaker - the casting sheet one character reads from. */
UCLASS()
class USpeechSpeakerFactory : public UFactory
{
	GENERATED_BODY()

public:

	USpeechSpeakerFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};

/** Creates a Voice Profile - a provider voice pinned under a name of ours. */
UCLASS()
class USpeechVoiceProfileFactory : public UFactory
{
	GENERATED_BODY()

public:

	USpeechVoiceProfileFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};

/** Creates a single Speech Line outside any bank, for the one-off that a bank would overdress. */
UCLASS()
class USpeechLineDefFactory : public UFactory
{
	GENERATED_BODY()

public:

	USpeechLineDefFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};
