// A speaker's character sheet: who they are, what they sound like, and what they link to.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeechSpeaker.generated.h"

class USpeechVoiceProfile;

/**
 * One speaker, defined before any scene they appear in.
 *
 * The screenwriter's order is the design: first the cast is defined - identity, sound, links -
 * then lines are written and assigned to speakers by id, then production runs on top. This asset
 * is the first step, and it is the one place a speaker's facts consolidate. The set of these
 * assets *is* the project's cast list; there is no separate mapping table.
 *
 * A line resolves its voice through here: the line carries only a SpeakerId, this sheet names the
 * voice profile, and the profile names the provider. Recasting a character is one edit on one
 * asset, and every line the character speaks follows.
 *
 * ExternalBindings is the universal seam. SpeechForge names no game framework: an adapter that
 * knows what a speaker is elsewhere - a Narrative NPC definition, a cloud-app character - records
 * the link under its own key, and core never learns what the key means. Deleting the adapter
 * plugin leaves a dormant path, never a broken build.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Speech Speaker"))
class SPEECHFORGE_API USpeechSpeaker : public UDataAsset
{
	GENERATED_BODY()

public:

	/**
	 * Who this is, in project terms - the exact id lines carry in their SpeakerId field.
	 *
	 * Searchable in the asset registry, so resolution finds the sheet without loading every
	 * speaker in the project.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Identity")
	FName SpeakerId;

	/** The name panels show. Falls back to SpeakerId when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity")
	FString DisplayName;

	/** The character sheet proper: who they are, how they carry themselves, casting notes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity", meta = (MultiLine = true))
	FString Description;

	/**
	 * What this speaker sounds like. A voice profile - the instrument - which owns the provider,
	 * the preset, the model and the settings. Unset means uncast: lines fall through to the bank
	 * default, then the project default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voice")
	TSoftObjectPtr<USpeechVoiceProfile> VoiceProfile;

	/**
	 * What this speaker's face animation drives: a skeleton, a skeletal mesh, a face archetype -
	 * whatever names the rig. Manually settable, and that is the point: a filmmaker in vanilla
	 * Unreal makes a speaker, casts a voice, sets this to a MetaHuman face archetype, and the
	 * whole write-voice-face pipeline works with no game framework installed.
	 *
	 * Voice and text never need it. Face targeting and performance capture do, and a speaker
	 * without one gets a "no rig" badge rather than an error. Adapters that can derive it (a
	 * Narrative NPC's appearance, say) fill the blank as a suggestion; this field, once set,
	 * wins.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rig")
	FSoftObjectPath RigTarget;

	/**
	 * This speaker's actual head: the face mesh their lines are previewed and corrected on.
	 *
	 * Optional, and set the same way the rig is - by hand in vanilla Unreal, or by an adapter that
	 * knows the character. Unset means "the rig's default head": the face bank's Target Face Mesh,
	 * then the skeleton's own preview mesh. Set, every face clip of this speaker resolves to it
	 * live, by speaker id - held here once rather than copied onto hundreds of lines, so changing
	 * the character's appearance changes every line with it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rig")
	TSoftObjectPtr<USkeletalMesh> FaceMesh;

	/**
	 * Links to what this speaker is in other systems, keyed by the adapter that owns the link.
	 *
	 * e.g. "NarrativePro" -> an NPCDefinition. Written by adapter plugins, read by whoever asks;
	 * core stores the paths and attaches no meaning to any key.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Links")
	TMap<FName, FSoftObjectPath> ExternalBindings;

	/** The name a human should read. */
	FString GetLabel() const
	{
		return !DisplayName.IsEmpty() ? DisplayName : SpeakerId.ToString();
	}
};
