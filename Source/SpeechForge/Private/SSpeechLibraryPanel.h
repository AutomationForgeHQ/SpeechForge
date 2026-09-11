// The Speech Library: cast the speakers, write the lines, produce the audio - in that order.

#pragma once

#include "CoreMinimal.h"
#include "ISpeechProvider.h"
#include "SpeechForgeTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class USpeechForgeSubsystem;
template <typename T> class SComboBox;
template <typename T> class SSegmentedControl;
class SEditableTextBox;
class SMultiLineEditableTextBox;

/** One line: pipeline facts plus the authoring fields and the voice as a human reads it. */
struct FSpeechLibraryRow
{
	FSpeechLineStatus Status;
	FString Text;
	FString Direction;

	/** The resolved voice's human label - a profile's display name, never an id. */
	FString VoiceLabel;

	/** Content path of the line's override profile, empty when the speaker's casting answers. */
	FString OverridePath;

	/**
	 * On a localised bank: this line's source has a recording, so it can be dubbed rather than read
	 * out again by a synthesiser.
	 *
	 * Which is the whole choice localisation offers per line. A generated source line is cheaper and
	 * better regenerated in the new language; a performed one is worth carrying over, because a dub
	 * keeps the actor's voice and pacing and therefore keeps the captured face underneath usable.
	 * False on a source-language bank, where there is no source to dub from.
	 */
	bool bSourceRecorded = false;

	/**
	 * What the Origin column shows when the bank has more to say than the line's own origin - a
	 * localised bank names the source performance behind a line, and whether its audio is a dub.
	 * Empty means the ordinary text.
	 */
	FString OriginLabel;
};

/** One speaker on the Cast page. AssetPath empty means the id appears in lines but has no sheet yet. */
struct FSpeechCastRow
{
	FName SpeakerId;
	FString DisplayName;
	FString Description;
	FString AssetPath;

	FString ProfilePath;
	FString ProfileLabel;

	/** "NarrativePro -> NPC_Renk" style one-liners, for the Links tooltip. */
	TArray<FString> BindingSummaries;

	/** Lines in the current bank. Zero for a speaker not in this scene. */
	int32 LineCount = 0;

	FString GetLabel() const
	{
		return !DisplayName.IsEmpty() ? DisplayName : SpeakerId.ToString();
	}
};

/** One voice profile - the instrument - as the Cast page lists it. */
struct FSpeechProfileRow
{
	FString AssetPath;
	FString Label;
	FName ProviderId;
	FString ProviderVoiceId;
	int32 UsedBySpeakers = 0;
};

/**
 * A panel action discovered from tool metadata rather than written here.
 *
 * Any registered toolset function tagged `meta = (SpeechLibraryAction = "Label")` becomes a button
 * in the Produce action bar - the same discovery philosophy as the pipeline node palette, so
 * installing a plugin adds buttons with no edit to SpeechForge, and a third party gets the same
 * seam we use. The function takes one FString: the selection context as JSON.
 */
struct FSpeechLibraryAction
{
	FString Label;
	FString Tooltip;
	TWeakObjectPtr<UClass> ToolClass;
	FName FunctionName;

	/**
	 * Which page hosts the button, from `meta = (SpeechLibraryPage = "Ingest")`. Empty means the
	 * Produce action bar, where actions have always lived; "Ingest" moves it beside the ingestion
	 * methods it belongs with - Re-harvest is the canonical case.
	 */
	FString Page;

	/**
	 * Which box on that page, from `meta = (SpeechLibraryGroup = "Dialogue")`. The Perform page holds
	 * three kinds of verb - build the faces, record a performance, hand the finished thing to the
	 * game - and an apply button filed under recording reads as part of the recording flow. Empty
	 * means the page's default box.
	 */
	FString Group;

	/**
	 * From `meta = (SpeechLibraryPerRig)`: the action is invoked once per RIG among the chosen
	 * lines' speakers, with `faceBankSuffix` and `targetSkeletonPath` added to its context - the
	 * one-face-bank-per-rig rule, executed. Create/Update Face Bank is the canonical case: a
	 * multi-rig speech bank must split into sibling face banks, never blend into one.
	 */
	bool bPerRig = false;
};

/**
 * A write-back an adapter ships: pushes a bank line back to whatever the bank was harvested from.
 *
 * Discovered from tool metadata - `meta = (SpeechLineSync = "Label", SpeechLineSyncAdapter = "Key")`
 * on a toolset function taking (BankPath, LineId) - and offered only on banks whose source stamp
 * carries the same key. That is the whole two-way-sync contract: harvest is the forward direction
 * and stamps the bank; this is the reverse, shipped by the same adapter; core matches keys and
 * never learns what either side of the sync is.
 */
struct FSpeechLineSyncAction
{
	FString Label;
	FString Tooltip;
	FName AdapterKey;
	TWeakObjectPtr<UClass> ToolClass;
	FName FunctionName;
};

/**
 * A way to pull lines into a bank from some other asset - a dialogue, a script - shipped by an
 * adapter, never written here.
 *
 * Discovered from tool metadata:
 * `meta = (SpeechIngest = "Label", SpeechIngestClass = "/Script/Module.Class")` on a toolset
 * function taking two strings - the source asset's content path, then the target bank's - and
 * returning what happened. The Ingest page exists exactly when at least one of these is installed:
 * core has no ingestion of its own, so with no adapters the page would be an empty apology.
 *
 * SpeechIngestClass names what the method reads. The page's asset picker offers assets of that
 * class, its subclasses, and Blueprints whose native parent is one - which is what a Narrative
 * dialogue is.
 */
struct FSpeechIngestAction
{
	FString Label;
	FString Tooltip;

	/** Top-level path of the class this method ingests, e.g. "/Script/NarrativeArsenal.Dialogue". */
	FString AssetClassPath;

	/**
	 * The source-stamp key this method writes, from `SpeechIngestAdapter` - the same key
	 * `SpeechLineSync` uses. It lets the picker show the bank's remembered source: the stamp on
	 * the bank says which adapter harvested it, and only the method with the matching key may
	 * claim that path as its own.
	 */
	FName AdapterKey;

	TWeakObjectPtr<UClass> ToolClass;
	FName FunctionName;

	/**
	 * Whether the page's shared picked source satisfies this method's class filter. One source,
	 * several methods: the Ingest page has a single picker, and each method's button is enabled
	 * only when the picked asset is something that method can read.
	 */
	bool bPickedMatches = false;
};

/** One face bank serving the open speech bank, found through the registry's link tag. */
struct FPerformFaceBankRow
{
	/** Object path of the face bank asset. */
	FString Path;

	/** The name a human reads. */
	FString Label;

	/** The skeleton this bank's bakes target - the fact assignment matches on. */
	FString SkeletonPath;
	FString SkeletonLabel;

	/** How many of the open speech bank's lines have a clip here. */
	int32 CoveredCount = 0;
};

/** One recording session that covers lines of the open speech bank. */
struct FPerformSessionRow
{
	/** Object path of the session asset. */
	FString Path;

	FString Label;

	/** How many of the session's lines belong to the open bank, and how many it has in total. */
	int32 BankLineCount = 0;
	int32 TotalLineCount = 0;
};

/** One language the translator offers, as the Localize page lists it. */
struct FLocalizeLanguageRow
{
	FString Code;
	bool bChecked = false;

	/** The sibling bank that already exists for it, if any - shown, and re-run updates it. */
	FString ExistingBankPath;
};

/** One localised sibling of the open bank, with its counts. */
struct FLocalizedBankRow
{
	FString LanguageCode;
	FString BankPath;
	FString Label;
	int32 LinesCurrent = 0;
	int32 LinesStale = 0;
	int32 LinesMissing = 0;
	int32 LinesWithAudio = 0;

	/** The face banks cloned for this language, by short name. */
	FString FaceBankLabels;
};

/**
 * The Speech Library: the human surface over speech banks, in pages that follow the workflow's
 * order.
 *
 *   Ingest   Pull lines into the bank from a source asset - a Narrative dialogue, a script - plus
 *            everything about staying in step with that source: the stamp, drift, re-harvest. The
 *            page exists only when an installed adapter ships an ingestion method; core has none
 *            of its own. See FSpeechIngestAction.
 *   Cast     Define who is in the scene and what they sound like: speaker sheets, voice profiles,
 *            and a live browser over whichever providers are installed.
 *   Write    The lines - text and direction editable in place, speaker assignable per line, the
 *            voice cell showing the profile that answers and offering the by-hand override.
 *   Produce  Selection, cost, Generate, and every discovered action - recording sessions, face
 *            banks, whatever installed plugins tagged in.
 *   Perform  Lines become bodies: the face banks serving this bank, each line's rig, face status
 *            and sessions, and the recording flow. Exists only when an installed plugin tags an
 *            action onto it (FaceForge's Create/Update Face Bank is the canonical one); the
 *            session half lights up when PerformanceForge is present.
 *
 * The panel is complete with SpeechForge alone. Everything else is discovered: action buttons come
 * from tool metadata, and the voice browser runs off whichever providers are registered. A missing
 * plugin is a missing button, never a build error. Every button calls the subsystem or a
 * discovered tool; nothing is reachable from here that an agent cannot reach.
 */
class SSpeechLibraryPanel : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SSpeechLibraryPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// --- Called by row widgets (the panel outlives its rows) ------------------------------------

	void SetLineSpeaker(const FSpeechLineHandle& Handle, FName SpeakerId);
	void SetLineOverride(const FSpeechLineHandle& Handle, const FString& ProfilePath);

	const TArray<TSharedPtr<FSpeechCastRow>>& GetCastRows() const { return CastRows; }
	const TArray<TSharedPtr<FSpeechProfileRow>>& GetProfileRows() const { return ProfileRows; }

	// --- Perform-page facts, cached by RefreshPerform and read by row widgets --------------------

	/** The rig the speaker's sheet names, or empty for "no rig". */
	FString GetRigLabel(FName SpeakerId) const { const FString* Found = SpeakerRigLabels.Find(SpeakerId); return Found ? *Found : FString(); }

	/** The line's face clip status in the linked face bank, or empty for "no clip". */
	FString GetFaceStatusLabel(FName LineId) const { const FString* Found = LineFaceStatus.Find(LineId); return Found ? *Found : FString(); }

	/** Short names of the sessions holding this line, or empty. */
	FString GetSessionChips(FName LineId) const { const FString* Found = LineSessionChips.Find(LineId); return Found ? *Found : FString(); }

	/** The linked face banks whose skeleton matches this speaker's rig - what Assign may offer. */
	TArray<TSharedPtr<FPerformFaceBankRow>> GetAssignableFaceBanks(FName SpeakerId) const;

	/** Put one line's clip into a chosen bank, or into a new bank for its rig. */
	FReply OnAssignLineToBank(FName LineId, FString FaceBankPath);
	FReply OnAssignLineToNewBank(FName LineId);

	/** Open the Face Bank panel AT this line's clip, in the bank that holds it. */
	FReply OnOpenLineFaceClip(FName LineId);

	void ShowCastPage() { ActivePage = 1; /* PageCast - Ingest holds 0, present or not */ }

private:

	USpeechForgeSubsystem* Subsystem() const;

	// --- Banks and pages -------------------------------------------------------------------------

	void RefreshBanks();
	TSharedRef<SWidget> MakeBankEntry(TSharedPtr<FString> Path);
	void OnBankChosen(TSharedPtr<FString> Path, ESelectInfo::Type);
	FText BankComboLabel() const;

	/** Everything, in dependency order: profiles, then rows, then the cast. */
	void RefreshAll();

	int32 ActivePage = 0;

	/** Held so the Ingest segment can be added after construction, when adapters warrant it. */
	TSharedPtr<SSegmentedControl<int32>> PageControl;

	// --- Lines (shared by Write and Produce) -----------------------------------------------------

	void RefreshRows();
	TSharedRef<ITableRow> MakeWriteRow(TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeProduceRow(TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakePerformRow(TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner);
	TArray<FSpeechLineHandle> SelectedHandles() const;

	TArray<TSharedPtr<FSpeechLibraryRow>> Rows;
	TSharedPtr<SListView<TSharedPtr<FSpeechLibraryRow>>> WriteList;
	TSharedPtr<SListView<TSharedPtr<FSpeechLibraryRow>>> ProduceList;

	// --- The line editor (Write page) ------------------------------------------------------------

	TSharedRef<SWidget> MakeLineEditor();
	void OnWriteSelectionChanged(TSharedPtr<FSpeechLibraryRow> Row, ESelectInfo::Type);

	/** Save the editor's text and direction onto the bank line. The bank-local half of an edit. */
	FReply OnApplyLineEdit();

	/** Remove the Write list's selected lines from the bank, after a confirmation. Audio stays. */
	FReply OnDeleteSelectedLines();

	/** Remove every line from the bank, after a confirmation. Cast, language and stamp stay. */
	FReply OnClearBankLines();

	/** Line ids selected in the Write list. */
	TArray<FName> SelectedWriteLineIds() const;

	/** Save onto the bank, then push through the adapter's write-back. The synced half. */
	FReply OnSyncLineEdit(TSharedPtr<FSpeechLineSyncAction> Action);

	TSharedPtr<SMultiLineEditableTextBox> EditTextBox;
	TSharedPtr<SEditableTextBox> EditDirectionBox;
	FSpeechLineHandle EditingHandle;
	FText LineEditorMessage;

	/** The row the editor is on, or null. Rows rebuild on refresh; the handle is the identity. */
	TSharedPtr<FSpeechLibraryRow> FindEditingRow() const;

	/** Discovered write-backs, all adapters. Shown per bank by matching the source stamp. */
	TArray<TSharedPtr<FSpeechLineSyncAction>> SyncActions;

	/** The current bank's source stamp. None/empty when the bank is its own source. */
	/** Show a bank, and a line within it, because something elsewhere in the pipeline asked. */
	void FocusOn(const FString& BankPath, FName LineId);

	/** Something wrote speech data; refresh on the next tick. */
	void MarkNeedsRefresh();

	FDelegateHandle FocusHandle;

	/** Speech data changed somewhere; re-read on the next tick rather than trusting what was drawn. */
	FDelegateHandle ChangedHandle;
	bool bPendingRefresh = false;

	virtual void Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime) override;

	FName BankSourceAdapter;
	FString BankSourceAssetPath;

	/** Whether the script this bank was harvested from has been written since. Refreshed with the rows. */
	bool BankSourceDrifted = false;
	FString BankSourceDriftDetail;

	// --- Cast page -------------------------------------------------------------------------------

	TSharedRef<SWidget> MakeCastPage();
	void RefreshProfiles();
	void RefreshCast();

	TSharedPtr<FSpeechCastRow> SelectedSpeaker() const;
	FReply OnAddSpeaker();
	FReply OnCreateSheet(TSharedPtr<FSpeechCastRow> Row);
	FReply OnAssignProfile(TSharedPtr<FSpeechProfileRow> Profile);
	FReply OnOpenAsset(FString AssetPath);

	TArray<TSharedPtr<FSpeechCastRow>> CastRows;
	TSharedPtr<SListView<TSharedPtr<FSpeechCastRow>>> CastList;
	TSharedPtr<SEditableTextBox> NewSpeakerBox;

	/** Off shows only the speakers with lines in this bank - the scene. On shows the ensemble. */
	bool bShowAllSpeakers = false;

	TArray<TSharedPtr<FSpeechProfileRow>> ProfileRows;
	TSharedPtr<SListView<TSharedPtr<FSpeechProfileRow>>> ProfileList;

	/** "Provider|ProviderVoiceId" -> profile label, and path -> label, for naming resolved voices. */
	TMap<FString, FString> ProfileLabelByKey;
	TMap<FString, FString> ProfileLabelByPath;

	// --- The provider browser (Cast page) --------------------------------------------------------

	TArray<TSharedPtr<FName>> ProviderOptions;
	TSharedPtr<FName> ChosenProvider;

	/** The provider the browser is pointed at, or null. */
	TSharedPtr<ISpeechProvider> GetChosenProvider() const;

	TArray<TSharedPtr<FSpeechRemoteVoice>> RemoteVoices;
	TSharedPtr<SListView<TSharedPtr<FSpeechRemoteVoice>>> RemoteVoiceList;

	FReply OnFetchVoices();
	FReply OnPreviewVoice(TSharedPtr<FSpeechRemoteVoice> Voice);
	FReply OnSaveProfile(TSharedPtr<FSpeechRemoteVoice> Voice);
	FReply OnCastVoice(TSharedPtr<FSpeechRemoteVoice> Voice);

	/** The profile for a browsed voice, created if the project has none yet. */
	FString EnsureProfileForRemoteVoice(TSharedPtr<FSpeechRemoteVoice> Voice);

	FText CastingMessage() const;
	FText CastingMessageText;

	// --- Produce page ----------------------------------------------------------------------------

	TSharedRef<SWidget> MakeProducePage();
	TSharedRef<SWidget> MakeActionBar();
	FText EstimateText() const;
	FText MessageText() const;
	bool CanGenerate() const;
	FReply OnGenerateClicked();

	/** Whole bank, idempotent: missing and stale lines generate, current ones cost nothing. */
	FReply OnGenerateAllClicked();

	/** Selection, forced: re-generates regardless of state, after a costed confirmation. */
	FReply OnRegenerateClicked();

	/** Only on a localised bank - a source-language bank has nothing to dub from. */
	EVisibility DubVisibility() const;

	/** ...and only for selected lines whose source is a performance. */
	bool CanDub() const;

	/** Selection, one line at a time: carry each source recording into this language. */
	FReply OnDubClicked();

	/** The sequential step: dub one line, then the next when it lands. */
	void DubNext(TArray<FSpeechLineHandle> Handles, int32 Index, TSharedRef<TArray<FString>> Results);

	/** Dubs still in flight, so a second press cannot start the same line twice. */
	int32 DubsRunning = 0;

	/** The open bank has a language and a source bank - computed with the rows, not per frame. */
	bool bBankIsLocalized = false;

	/** Buttons found by scanning tool metadata. See FSpeechLibraryAction. */
	TArray<TSharedPtr<FSpeechLibraryAction>> DiscoveredActions;
	void DiscoverActions();
	FReply OnActionClicked(TSharedPtr<FSpeechLibraryAction> Action);

	// --- Ingest page (exists only when an adapter ships an ingestion method) ---------------------

	TSharedRef<SWidget> MakeIngestPage();
	FReply OnIngestClicked(TSharedPtr<FSpeechIngestAction> Action);

	/** The path an ingest would read: this session's pick, else the bank's matching source stamp. */
	FString IngestSourcePath(const TSharedPtr<FSpeechIngestAction>& Action) const;

	/** Discovered ingestion methods, all adapters. Empty means no Ingest page. */
	TArray<TSharedPtr<FSpeechIngestAction>> IngestActions;

	/**
	 * The page's shared source pick - session state, cleared on bank change. Empty shows the
	 * bank's own saved source stamp instead, which is what survives an editor restart.
	 */
	FString IngestPickedAsset;

	FText IngestMessage;

	// --- Perform page (exists only when a plugin tags an action onto it) -------------------------

	TSharedRef<SWidget> MakePerformPage();

	/** Face banks, per-line facts and sessions for the open bank. Called from RefreshAll. */
	void RefreshPerform();

	FReply OnOpenFaceBank(FString FaceBankPath);
	FReply OnOpenSession(FString SessionPath);

	TArray<TSharedPtr<FPerformFaceBankRow>> PerformFaceBanks;
	TArray<TSharedPtr<FPerformSessionRow>> PerformSessions;
	TSharedPtr<SListView<TSharedPtr<FPerformFaceBankRow>>> PerformFaceBankList;
	TSharedPtr<SListView<TSharedPtr<FPerformSessionRow>>> PerformSessionList;
	TSharedPtr<SListView<TSharedPtr<FSpeechLibraryRow>>> PerformList;

	TMap<FName, FString> SpeakerRigLabels;
	TMap<FName, FString> SpeakerRigPaths;
	TMap<FName, FString> LineFaceStatus;
	TMap<FName, FString> LineFaceBankPaths;
	TMap<FName, FString> LineSessionChips;

	// --- Localize page (exists only when a real translation provider is installed) ----------------

	TSharedRef<SWidget> MakeLocalizePage();

	/** The translator's languages, the siblings that exist, and their face-bank clones. */
	void RefreshLocalize();

	/** Localise every checked language, then clone the face-bank structure for each. */
	FReply OnCreateLocalizedClicked();

	/** After a language's speech bank exists: clone each serving face bank for it. */
	void CloneFaceBanksForLanguage(const FString& LocalizedBankPath, const FString& LanguageCode);

	FReply OnOpenLocalizedBank(FString BankPath);
	TSharedRef<ITableRow> MakeLocalizeLanguageRow(TSharedPtr<FLocalizeLanguageRow> Row, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeLocalizedBankRow(TSharedPtr<FLocalizedBankRow> Row, const TSharedRef<STableViewBase>& Owner);

	/** True when a translator other than the keyless pseudo-localiser is registered. */
	bool bLocalizePage = false;

	TArray<TSharedPtr<FString>> TranslatorIds;
	TSharedPtr<FString> ChosenTranslator;
	TArray<TSharedPtr<FLocalizeLanguageRow>> LocalizeLanguages;
	TArray<TSharedPtr<FLocalizedBankRow>> LocalizedBanks;
	TSharedPtr<SListView<TSharedPtr<FLocalizeLanguageRow>>> LocalizeLanguageList;
	TSharedPtr<SListView<TSharedPtr<FLocalizedBankRow>>> LocalizedBankList;

	/** A code typed by hand, for a translator that publishes no list or a language off it. */
	FString LocalizeCustomCode;
	bool bLocalizeForce = false;
	FText LocalizeMessage;

	/** Languages still translating from the last click; the message counts them down. */
	int32 LocalizePending = 0;

	/** A per-rig action: group the rows by rig, confirm the split, invoke once per slice. */
	FReply OnPerRigActionClicked(TSharedPtr<FSpeechLibraryAction> Action);

	/** The per-rig flow over an explicit row set - Auto-assign and per-line assignment ride it. */
	FReply RunPerRigAction(
		TSharedPtr<FSpeechLibraryAction> Action,
		TArray<TSharedPtr<FSpeechLibraryRow>> ForRows,
		const FString& ExplicitFaceBankPath);

	/** The first discovered per-rig action - what Assign and Auto-assign invoke. */
	TSharedPtr<FSpeechLibraryAction> FindPerRigAction() const;

	/** Every line in this bank with no clip in any linked face bank. */
	TArray<TSharedPtr<FSpeechLibraryRow>> UnassignedRows() const;

	/** Auto-assign: the per-rig flow over exactly the unassigned lines. */
	FReply OnAutoAssignClicked();

	/** The selection context for explicit rows, with extra fields an action's contract asks for. */
	FString BuildContextJsonForRows(
		const TArray<TSharedPtr<FSpeechLibraryRow>>& ForRows,
		const TMap<FString, FString>& ExtraFields) const;

	/** Reflection-invoke an action's function with a ready context. Returns what it said. */
	FString InvokeActionFunction(const TSharedPtr<FSpeechLibraryAction>& Action, const FString& ContextJson);

	/** Whether the PerformanceForge session class resolves - the whole session half rides this. */
	bool bPerformancePresent = false;

	FText PerformMessage;

	/** The selection plus the session fixtures, as the JSON every discovered action receives. */
	FString BuildSelectionContextJson() const;

	// Session fixtures, shown beside the discovered buttons and carried in the context.
	TArray<TSharedPtr<FString>> ModeOptions;
	TArray<TSharedPtr<FString>> TreatmentOptions;
	TArray<TSharedPtr<FString>> FaceBankOptions;
	TSharedPtr<FString> ChosenMode;
	TSharedPtr<FString> ChosenTreatment;
	TSharedPtr<FString> ChosenFaceBank;

	void RefreshFaceBanks();

	// ---------------------------------------------------------------------------------------------

	TArray<TSharedPtr<FString>> BankPaths;
	TSharedPtr<FString> ChosenBank;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> BankCombo;

	FText LastMessage;
};
