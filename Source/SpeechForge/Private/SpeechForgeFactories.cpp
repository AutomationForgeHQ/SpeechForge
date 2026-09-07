// Copyright Blackcode SA. All rights reserved.

#include "SpeechForgeFactories.h"

#include "SpeechBank.h"
#include "SpeechLineDef.h"
#include "SpeechSpeaker.h"
#include "SpeechVoiceProfile.h"

#define LOCTEXT_NAMESPACE "SpeechForge"

// -------------------------------------------------------------------------------------------------

USpeechBankFactory::USpeechBankFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = USpeechBank::StaticClass();
}

UObject* USpeechBankFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<USpeechBank>(InParent, Class, Name, Flags);
}

FText USpeechBankFactory::GetDisplayName() const
{
	return LOCTEXT("NewSpeechBank", "Speech Bank");
}

FString USpeechBankFactory::GetDefaultNewAssetName() const
{
	// The prefix the rest of the library uses, so a new one sorts with its neighbours rather than
	// under N for NewDataAsset.
	return TEXT("SB_NewBank");
}

// -------------------------------------------------------------------------------------------------

USpeechSpeakerFactory::USpeechSpeakerFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = USpeechSpeaker::StaticClass();
}

UObject* USpeechSpeakerFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<USpeechSpeaker>(InParent, Class, Name, Flags);
}

FText USpeechSpeakerFactory::GetDisplayName() const
{
	return LOCTEXT("NewSpeechSpeaker", "Speaker");
}

FString USpeechSpeakerFactory::GetDefaultNewAssetName() const
{
	return TEXT("SP_NewSpeaker");
}

// -------------------------------------------------------------------------------------------------

USpeechVoiceProfileFactory::USpeechVoiceProfileFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = USpeechVoiceProfile::StaticClass();
}

UObject* USpeechVoiceProfileFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<USpeechVoiceProfile>(InParent, Class, Name, Flags);
}

FText USpeechVoiceProfileFactory::GetDisplayName() const
{
	return LOCTEXT("NewSpeechVoiceProfile", "Voice Profile");
}

FString USpeechVoiceProfileFactory::GetDefaultNewAssetName() const
{
	return TEXT("VP_NewVoice");
}

// -------------------------------------------------------------------------------------------------

USpeechLineDefFactory::USpeechLineDefFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = USpeechLineDef::StaticClass();
}

UObject* USpeechLineDefFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<USpeechLineDef>(InParent, Class, Name, Flags);
}

FText USpeechLineDefFactory::GetDisplayName() const
{
	return LOCTEXT("NewSpeechLineDef", "Speech Line");
}

FString USpeechLineDefFactory::GetDefaultNewAssetName() const
{
	return TEXT("SL_NewLine");
}

#undef LOCTEXT_NAMESPACE
