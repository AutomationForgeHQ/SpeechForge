// Copyright Blackcode SA. All rights reserved.

#include "SpeechForgeAssetDefinitions.h"

#include "SpeechBank.h"
#include "SpeechForgeSubsystem.h"
#include "SpeechLineDef.h"
#include "SpeechSpeaker.h"
#include "SpeechVoiceProfile.h"

#define LOCTEXT_NAMESPACE "SpeechForge"

namespace
{
	/** One category for the whole family, with a submenu per set, so the types sit together. */
	const TArray<FAssetCategoryPath>& ForgeCategories()
	{
		static const TArray<FAssetCategoryPath> Categories
		{
			FAssetCategoryPath(
				FAssetCategoryPath(LOCTEXT("AutomationForge", "Automation Forge")),
				LOCTEXT("SpeechForge", "SpeechForge"))
		};
		return Categories;
	}
}

// -------------------------------------------------------------------------------------------------

FText USpeechBankAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("SpeechBank", "Speech Bank");
}

FLinearColor USpeechBankAssetDefinition::GetAssetColor() const
{
	// Violet for the SpeechForge set, apart from MotionForge's blue and MeshForge's orange.
	return FLinearColor(0.58f, 0.36f, 0.86f);
}

TSoftClassPtr<UObject> USpeechBankAssetDefinition::GetAssetClass() const
{
	return USpeechBank::StaticClass();
}

TConstArrayView<FAssetCategoryPath> USpeechBankAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

EAssetCommandResult USpeechBankAssetDefinition::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	USpeechForgeSubsystem* Forge = USpeechForgeSubsystem::Get();
	if (!Forge)
	{
		// No subsystem, no panel - the details editor is better than a dead double-click.
		return Super::OpenAssets(OpenArgs);
	}

	for (USpeechBank* Bank : OpenArgs.LoadObjects<USpeechBank>())
	{
		Forge->OpenLibraryAt(Bank->GetPathName(), NAME_None);
	}

	return EAssetCommandResult::Handled;
}

// -------------------------------------------------------------------------------------------------

FText USpeechSpeakerAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("SpeechSpeaker", "Speaker");
}

FLinearColor USpeechSpeakerAssetDefinition::GetAssetColor() const
{
	return FLinearColor(0.72f, 0.52f, 0.92f);
}

TSoftClassPtr<UObject> USpeechSpeakerAssetDefinition::GetAssetClass() const
{
	return USpeechSpeaker::StaticClass();
}

TConstArrayView<FAssetCategoryPath> USpeechSpeakerAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

// -------------------------------------------------------------------------------------------------

FText USpeechVoiceProfileAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("SpeechVoiceProfile", "Voice Profile");
}

FLinearColor USpeechVoiceProfileAssetDefinition::GetAssetColor() const
{
	return FLinearColor(0.46f, 0.28f, 0.68f);
}

TSoftClassPtr<UObject> USpeechVoiceProfileAssetDefinition::GetAssetClass() const
{
	return USpeechVoiceProfile::StaticClass();
}

TConstArrayView<FAssetCategoryPath> USpeechVoiceProfileAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

// -------------------------------------------------------------------------------------------------

FText USpeechLineDefAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("SpeechLineDef", "Speech Line");
}

FLinearColor USpeechLineDefAssetDefinition::GetAssetColor() const
{
	return FLinearColor(0.58f, 0.36f, 0.86f);
}

TSoftClassPtr<UObject> USpeechLineDefAssetDefinition::GetAssetClass() const
{
	return USpeechLineDef::StaticClass();
}

TConstArrayView<FAssetCategoryPath> USpeechLineDefAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

#undef LOCTEXT_NAMESPACE
