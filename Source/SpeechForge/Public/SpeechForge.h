#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

class ISpeechProvider;
class ISpeechVoiceSource;

/** Filter the Output Log on "LogSpeechForge" to follow resolve, price, synthesize and import. */
SPEECHFORGE_API DECLARE_LOG_CATEGORY_EXTERN(LogSpeechForge, Log, All);

/**
 * SpeechForge's module, and the two registries add-on plugins join.
 *
 * Both registries live here rather than on the subsystem for one reason: module startup order. An
 * add-on loads and registers whenever its own loading phase says, which may be before or after the
 * editor builds the subsystem. Holding the lists on the module means neither order loses a
 * registration - the subsystem reads whatever is present when it comes up, and hears about anything
 * that arrives later through the change delegates.
 *
 * Shared pointers rather than IModularFeature: the pipeline hands providers into callbacks that
 * outlive the call, and a raw pointer whose owning module unloaded mid-batch is a crash rather than
 * an error message.
 */
class SPEECHFORGE_API FSpeechForgeModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * SpeechForge's module, loading it if it has not started yet.
	 *
	 * **Use this from an add-on's StartupModule.** Two plugins in the same loading phase start in an
	 * order nobody controls, and a `.uplugin` dependency guarantees only that SpeechForge is
	 * *enabled* - not that its module ran first. Merely looking the module up therefore works on some
	 * runs and returns null on others, and the failure is a provider that silently never registers.
	 * Loading on demand removes the ordering question entirely.
	 */
	static FSpeechForgeModule* GetPtr();

	/**
	 * The module if it is already up, never loading it.
	 *
	 * For shutdown paths, where loading a module in order to tell it something is being torn down
	 * would be worse than doing nothing.
	 */
	static FSpeechForgeModule* GetPtrIfLoaded();

	// ---------------------------------------------------------------------------------------------
	// Providers - who can turn text into audio
	// ---------------------------------------------------------------------------------------------

	/**
	 * Make a provider available to the pipeline.
	 *
	 * Call from an add-on module's StartupModule. Registering the same id twice replaces the first,
	 * which makes hot reload survivable.
	 */
	void RegisterProvider(TSharedRef<ISpeechProvider> Provider);

	/** Take a provider back out again. Call from ShutdownModule, or in-flight work will outlive it. */
	void UnregisterProvider(FName ProviderId);

	TSharedPtr<ISpeechProvider> FindProvider(FName ProviderId) const;
	TArray<FName> GetProviderIds() const;

	DECLARE_MULTICAST_DELEGATE(FOnProvidersChanged);
	FOnProvidersChanged OnProvidersChanged;

	// ---------------------------------------------------------------------------------------------
	// Voice sources - who decides which voice a line is spoken in
	// ---------------------------------------------------------------------------------------------

	/**
	 * Make an external voice resolver available.
	 *
	 * This is how a line gets a voice from somewhere SpeechForge knows nothing about - an NPC
	 * definition, a casting table, a localisation sheet. Sources are consulted in descending priority
	 * and the first to answer wins; see FSpeechVoiceResolution and the resolution order documented on
	 * the subsystem.
	 *
	 * SpeechForge never learns what a source is or where it reads from, so deleting the plugin that
	 * registered one changes nothing except which voices resolve.
	 */
	void RegisterVoiceSource(TSharedRef<ISpeechVoiceSource> Source);

	/** Take a voice source back out again. */
	void UnregisterVoiceSource(FName SourceId);

	/** Every registered source, already sorted highest priority first. */
	TArray<TSharedPtr<ISpeechVoiceSource>> GetVoiceSources() const;

	DECLARE_MULTICAST_DELEGATE(FOnVoiceSourcesChanged);
	FOnVoiceSourcesChanged OnVoiceSourcesChanged;

private:

	/** Re-sort VoiceSources by descending priority. Called whenever the set changes. */
	void SortVoiceSources();

	TMap<FName, TSharedPtr<ISpeechProvider>> Providers;

	/** Kept as an array rather than a map because resolution order is the whole point. */
	TArray<TSharedPtr<ISpeechVoiceSource>> VoiceSources;
};
