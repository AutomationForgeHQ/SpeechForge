#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

class ISpeechProvider;
class ISpeechTranslationProvider;

/** Filter the Output Log on "LogSpeechForge" to follow resolve, price, synthesize and import. */
SPEECHFORGE_API DECLARE_LOG_CATEGORY_EXTERN(LogSpeechForge, Log, All);

/**
 * SpeechForge's module, and the provider registry add-on plugins join.
 *
 * The registry lives here rather than on the subsystem for one reason: module startup order. An
 * add-on loads and registers whenever its own loading phase says, which may be before or after the
 * editor builds the subsystem. Holding the list on the module means neither order loses a
 * registration - the subsystem reads whatever is present when it comes up, and hears about anything
 * that arrives later through the change delegate.
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

	/**
	 * The provider to use when nothing names one.
	 *
	 * The settings value when it is set; otherwise the sole registered provider, because a machine
	 * with exactly one provider plugin enabled has already made the choice. With several registered
	 * and none named this returns NAME_None and the caller reports rather than picking - providers
	 * bill different accounts, and a silent guess spends the wrong one.
	 *
	 * This exists because the setting used to default to a vendor by name, which meant the core
	 * could not be built without that vendor's plugin mattering. It defaults to None now, and this
	 * is the one place that decides what None means.
	 */
	FName ResolveDefaultProviderId() const;

	DECLARE_MULTICAST_DELEGATE(FOnProvidersChanged);
	FOnProvidersChanged OnProvidersChanged;

	// ---------------------------------------------------------------------------------------------
	// Translation providers - who can turn one language's text into another's
	//
	// A second registry rather than a flag on speech providers, because the two capabilities have
	// nothing in common but the word "provider": different vendors, different keys, different
	// billing. The core ships one keyless implementation ("Pseudo") so the localisation pipeline
	// is exercisable before anyone signs up for anything.
	// ---------------------------------------------------------------------------------------------

	void RegisterTranslationProvider(TSharedRef<ISpeechTranslationProvider> Provider);
	void UnregisterTranslationProvider(FName ProviderId);

	TSharedPtr<ISpeechTranslationProvider> FindTranslationProvider(FName ProviderId) const;
	TArray<FName> GetTranslationProviderIds() const;

	/**
	 * The translation provider to use when nothing names one.
	 *
	 * The sole registered *real* provider when there is exactly one - Pseudo does not count,
	 * because a machine with DeepL installed has made its choice and must not fall back to
	 * placeholder text by accident. Pseudo only when it is all there is. NAME_None when several
	 * real providers are registered and none was named, and the caller reports rather than picking.
	 */
	FName ResolveDefaultTranslationProviderId() const;

	// ---------------------------------------------------------------------------------------------
	// The bank class localisation creates.
	//
	// Core knows how to make a sibling bank per language and nothing about what such a bank should
	// do differently. A localisation add-on that knows more - which of its lines are dubs, how its
	// faces follow the source performance - registers a USpeechBank subclass here, and every bank
	// the Localize page creates from then on is one of those. Unregistered, localisation makes plain
	// banks, exactly as before.
	// ---------------------------------------------------------------------------------------------

	void RegisterLocalizedBankClass(UClass* BankClass);
	void UnregisterLocalizedBankClass(UClass* BankClass);

	/** USpeechBank when nothing is registered. */
	UClass* GetLocalizedBankClass() const;

private:

	TWeakObjectPtr<UClass> LocalizedBankClass;

	FDelegateHandle ToolMenusHandle;

	TMap<FName, TSharedPtr<ISpeechProvider>> Providers;
	TMap<FName, TSharedPtr<ISpeechTranslationProvider>> TranslationProviders;
};
