#include "SSpeechLibraryPanel.h"

#include "SpeechForge.h"
#include "SpeechForgeSubsystem.h"
#include "SpeechBank.h"
#include "SpeechSources.h"
#include "SpeechSpeaker.h"
#include "SpeechVoiceProfile.h"
#include "ISpeechTranslationProvider.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "PropertyCustomizationHelpers.h"
#include "Factories/SoundFactory.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectHash.h"
#include "Dom/JsonObject.h"
#include "Framework/Docking/TabManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "SSpeechLibraryPanel"

namespace
{
	// Page values, in workflow order. Ingest holds 0 and Perform holds 4 whether or not their
	// segments exist, so the other pages never renumber when a plugin appears.
	constexpr int32 PageIngest = 0;
	constexpr int32 PageCast = 1;
	constexpr int32 PageWrite = 2;
	constexpr int32 PageProduce = 3;
	constexpr int32 PagePerform = 4;
	constexpr int32 PageLocalize = 5;

	const FName ColumnRig(TEXT("Rig"));
	const FName ColumnFace(TEXT("Face"));
	const FName ColumnSessions(TEXT("Sessions"));

	const FName ColumnLine(TEXT("Line"));
	const FName ColumnSpeaker(TEXT("Speaker"));
	const FName ColumnText(TEXT("Text"));
	const FName ColumnDirection(TEXT("Direction"));
	const FName ColumnVoice(TEXT("Voice"));
	const FName ColumnStatus(TEXT("Status"));
	const FName ColumnOrigin(TEXT("Origin"));
	const FName ColumnDuration(TEXT("Duration"));
	const FName ColumnPlay(TEXT("Play"));

	// The editor has one preview channel, so the playing state is one pair: which sound, and the
	// component driving it. Every play affordance in this panel binds against these, which is what
	// makes a second click a stop, a click elsewhere a switch, and the end of the audio a revert -
	// the bindings re-evaluate, nothing is told.
	FString GPlayingSoundPath;
	TWeakObjectPtr<UAudioComponent> GPreviewComponent;

	bool IsSoundPathPlaying(const FString& SoundPath)
	{
		return !SoundPath.IsEmpty()
			&& GPlayingSoundPath == SoundPath
			&& GPreviewComponent.IsValid()
			&& GPreviewComponent->IsPlaying();
	}

	void StopPreview()
	{
		if (GPreviewComponent.IsValid())
		{
			GPreviewComponent->Stop();
		}
		GPreviewComponent.Reset();
		GPlayingSoundPath.Reset();
	}

	/** Audition a sound through the editor preview channel; a second call on the same sound stops it. */
	void TogglePlaySoundPath(const FString& SoundPath)
	{
		if (IsSoundPathPlaying(SoundPath))
		{
			StopPreview();
			return;
		}
		if (!GEditor || SoundPath.IsEmpty())
		{
			return;
		}
		if (USoundWave* Wave = LoadObject<USoundWave>(nullptr, *SoundPath))
		{
			GPreviewComponent = GEditor->PlayPreviewSound(Wave);
			GPlayingSoundPath = SoundPath;
		}
	}

	/** Where a remote voice's sample is cached once fetched - deterministic, so rows can bind to it. */
	FString PreviewAssetPathForVoice(const FString& VoiceId)
	{
		FString Safe = VoiceId;
		Safe.ReplaceInline(TEXT("-"), TEXT("_"));
		return FString::Printf(TEXT("/Game/_Generated/Speech/Previews/SWP_%s.SWP_%s"), *Safe, *Safe);
	}

	/** Play-or-stop icon for a bound sound path, sized to sit in a row. */
	TSharedRef<SWidget> MakePlayStateIcon(const FString& SoundPath)
	{
		return SNew(SImage)
			.Image_Lambda([SoundPath]()
			{
				return FAppStyle::GetBrush(IsSoundPathPlaying(SoundPath)
					? TEXT("Icons.Toolbar.Stop") : TEXT("Icons.Play"));
			})
			.DesiredSizeOverride(FVector2D(16.0, 16.0))
			.ColorAndOpacity_Lambda([SoundPath]() -> FSlateColor
			{
				return IsSoundPathPlaying(SoundPath)
					? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseForeground();
			});
	}

	/** The play cell used by both line tables. Absent rather than disabled when there is no audio. */
	TSharedRef<SWidget> MakePlayCell(const FString& SoundPath)
	{
		return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.Visibility(SoundPath.IsEmpty() ? EVisibility::Hidden : EVisibility::Visible)
			.ToolTipText_Lambda([SoundPath]()
			{
				return IsSoundPathPlaying(SoundPath)
					? NSLOCTEXT("SSpeechLibraryPanel", "StopTip", "Stop.")
					: NSLOCTEXT("SSpeechLibraryPanel", "PlayTip",
						"Hear the line's current audio - generated or recorded, whichever it is.");
			})
			.OnClicked_Lambda([SoundPath]() { TogglePlaySoundPath(SoundPath); return FReply::Handled(); })
			[
				MakePlayStateIcon(SoundPath)
			]
		];
	}

	FString OriginToString(ESpeechLineOrigin Origin)
	{
		switch (Origin)
		{
		case ESpeechLineOrigin::Generated: return TEXT("Generated");
		case ESpeechLineOrigin::Accepted:  return TEXT("Accepted");
		case ESpeechLineOrigin::Edited:    return TEXT("Edited");
		case ESpeechLineOrigin::Recorded:  return TEXT("Recorded");
		default:                           return TEXT("?");
		}
	}

	FString StatusToString(ESpeechLineStatus Status)
	{
		switch (Status)
		{
		case ESpeechLineStatus::Draft:      return TEXT("Draft");
		case ESpeechLineStatus::Generating: return TEXT("Generating");
		case ESpeechLineStatus::Generated:  return TEXT("Generated");
		case ESpeechLineStatus::Failed:     return TEXT("Failed");
		default:                            return TEXT("?");
		}
	}

	/** A plain text cell that tints while its line's audio plays. */
	TSharedRef<SWidget> MakeTextCell(const FText& Text, const FText& Tooltip, const FString& RowSound)
	{
		return SNew(STextBlock)
			.Text(Text)
			.ToolTipText(Tooltip)
			.Margin(FMargin(4, 2))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ColorAndOpacity_Lambda([RowSound]() -> FSlateColor
			{
				return IsSoundPathPlaying(RowSound)
					? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseForeground();
			});
	}

	/**
	 * One line row. Two shapes from one class: the Write page shows authoring cells that edit in
	 * place, the Produce page shows pipeline facts. Both end in the play toggle.
	 */
	class SSpeechLineRow : public SMultiColumnTableRow<TSharedPtr<FSpeechLibraryRow>>
	{
	public:
		SLATE_BEGIN_ARGS(SSpeechLineRow) : _Panel(nullptr), _bAuthoring(false) {}
			SLATE_ARGUMENT(TSharedPtr<FSpeechLibraryRow>, Row)
			SLATE_ARGUMENT(SSpeechLibraryPanel*, Panel)
			SLATE_ARGUMENT(bool, bAuthoring)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Row = InArgs._Row;
			Panel = InArgs._Panel;
			bAuthoring = InArgs._bAuthoring;
			SMultiColumnTableRow<TSharedPtr<FSpeechLibraryRow>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const FSpeechLineStatus& Status = Row->Status;
			const FString RowSound = Status.SoundPath;

			if (Column == ColumnLine)
			{
				return MakeTextCell(FText::FromName(Status.Handle.LineId), FText::GetEmpty(), RowSound);
			}

			if (Column == ColumnSpeaker)
			{
				if (!bAuthoring)
				{
					return MakeTextCell(FText::FromName(Status.SpeakerId), FText::GetEmpty(), RowSound);
				}
				return MakeSpeakerCell();
			}

			if (Column == ColumnText)
			{
				// Read-only in the row on purpose. Editing lives in the line editor below the
				// table, where there is room to rewrite and an honest place to talk about sync -
				// an editable cell hid both.
				return MakeTextCell(FText::FromString(Row->Text), FText::FromString(Row->Text), RowSound);
			}

			if (Column == ColumnDirection)
			{
				return MakeTextCell(FText::FromString(Row->Direction),
					LOCTEXT("DirectionTip",
						"Performance notes - whispers, exhausted, shouting. Kept out of the text so the subtitle never reads them aloud."),
					RowSound);
			}

			if (Column == ColumnVoice)
			{
				if (!bAuthoring)
				{
					return MakeTextCell(
						FText::FromString(Row->VoiceLabel),
						FText::FromString(Status.ResolvedVoice.SourceDescription),
						RowSound);
				}
				return MakeVoiceCell();
			}

			if (Column == ColumnStatus)
			{
				// Stale is the fact that changes what you do next, so it rides the status cell -
				// and it screams in proportion to what fixing it costs. Stale-plus-Generated is a
				// re-read for a cent; stale on a recorded or hand-edited line means the subtitle
				// and a real performance now disagree, and no button here can regenerate that.
				FString Value = StatusToString(Status.Status);
				FText Tooltip;

				if (Status.bWordsUnverified)
				{
					// Not an alarm. Nothing recorded what this audio says, so the honest report is
					// that the subtitle cannot be checked - which somebody clears by accepting the
					// line or replacing it, not by being shouted at.
					return MakeTextCell(
						FText::FromString(Value + TEXT("  (words unverified)")),
						LOCTEXT("WordsUnverified",
							"This audio predates subtitle checking, so nothing here knows what it "
							"actually says. Accept the line to make its current text the baseline, "
							"or regenerate/re-record it."),
						RowSound);
				}

				if (Status.bStale)
				{
					const bool bPerformance =
						Status.Origin == ESpeechLineOrigin::Recorded ||
						Status.Origin == ESpeechLineOrigin::Edited;

					// Which of the two questions failed decides the words, because they cost
					// different things to fix. A moved setting is a button. A moved script on a
					// performed line is a person, a microphone and a day.
					if (Status.bWordsDrifted)
					{
						Value += bPerformance ? TEXT("  SCRIPT CHANGED - PICKUP") : TEXT("  SCRIPT CHANGED");
					}
					else
					{
						Value += bPerformance ? TEXT("  STALE - PERFORMANCE") : TEXT("  STALE");
					}

					Tooltip = FText::FromString(Status.StaleReason);

					return SNew(STextBlock)
						.Text(FText::FromString(Value))
						.ToolTipText(Tooltip)
						.Margin(FMargin(4, 2))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.ColorAndOpacity(FSlateColor(bPerformance
							? FStyleColors::Error : FStyleColors::Warning));
				}
				return MakeTextCell(FText::FromString(Value), Tooltip, RowSound);
			}

			if (Column == ColumnOrigin)
			{
				// Whose performance, and whose voice - two facts, and on a re-voiced take they have
				// different answers.
				FString Value = OriginToString(Status.Origin);
				if (Status.Origin == ESpeechLineOrigin::Recorded || Status.Origin == ESpeechLineOrigin::Accepted)
				{
					Value += Status.bRevoiced ? TEXT(" (revoiced)") : TEXT(" (original)");
				}

				return MakeTextCell(FText::FromString(Value), FText::GetEmpty(), RowSound);
			}

			if (Column == ColumnDuration)
			{
				return MakeTextCell(
					Status.DurationSeconds > 0.f
						? FText::FromString(FString::Printf(TEXT("%.2fs"), Status.DurationSeconds))
						: FText::FromString(TEXT("-")),
					FText::GetEmpty(), RowSound);
			}

			// The Perform page's three facts, answered by the panel's caches rather than the row:
			// what rig the speaker drives, where this line's face is, and which sessions hold it.

			if (Column == ColumnRig)
			{
				const FString Rig = Panel ? Panel->GetRigLabel(Status.SpeakerId) : FString();
				return MakeTextCell(
					FText::FromString(Rig.IsEmpty() ? TEXT("no rig") : *Rig),
					LOCTEXT("RigTip",
						"What this speaker's face animation drives, from the speaker sheet's Rig "
						"Target. Voice and text never need one; face targeting and performance do. "
						"Set it on the speaker sheet (Cast page opens it)."),
					RowSound);
			}

			if (Column == ColumnFace)
			{
				const FString Face = Panel ? Panel->GetFaceStatusLabel(Status.Handle.LineId) : FString();
				if (!Panel)
				{
					return MakeTextCell(FText::FromString(TEXT("-")), FText::GetEmpty(), RowSound);
				}

				if (!Face.IsEmpty())
				{
					// Assigned: the fact plus the door - Open lands on this exact clip in the
					// bank that holds it.
					const FName LineId = Status.Handle.LineId;
					SSpeechLibraryPanel* PanelPtr = Panel;

					return SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							MakeTextCell(
								FText::FromString(Face),
								LOCTEXT("FaceTip",
									"This line's clip and how far it has got - prepared, solved, "
									"baked - and which face bank holds it."),
								RowSound)
						]

						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("OpenLineFace", "Open"))
							.ToolTipText(LOCTEXT("OpenLineFaceTip",
								"Open the Face Bank panel at this line's clip."))
							.OnClicked_Lambda([PanelPtr, LineId]()
							{
								return PanelPtr->OnOpenLineFaceClip(LineId);
							})
						];
				}

				// Unassigned, so the cell IS the fix: a menu of banks matching this speaker's
				// rig, plus a new bank for it. Mismatched rigs are simply not offered - a
				// MetaHuman line must not be assignable to the CC5 bank.
				SSpeechLibraryPanel* PanelPtr = Panel;
				const FName LineId = Status.Handle.LineId;
				const FName SpeakerId = Status.SpeakerId;

				return SNew(SComboButton)
					.ToolTipText(LOCTEXT("AssignFaceTip",
						"This line has no face clip yet. Assign it to a face bank whose skeleton "
						"matches the speaker's rig, or start a new bank for that rig."))
					.ButtonContent()
					[
						SNew(STextBlock).Text(LOCTEXT("AssignFace", "Assign..."))
					]
					.OnGetMenuContent_Lambda([PanelPtr, LineId, SpeakerId]()
					{
						FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);

						for (const TSharedPtr<FPerformFaceBankRow>& Bank :
							PanelPtr->GetAssignableFaceBanks(SpeakerId))
						{
							Menu.AddMenuEntry(
								FText::FromString(Bank->Label),
								FText::FromString(FString::Printf(TEXT("skeleton: %s"), *Bank->SkeletonLabel)),
								FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda(
									[PanelPtr, LineId, Path = Bank->Path]()
									{
										PanelPtr->OnAssignLineToBank(LineId, Path);
									})));
						}

						Menu.AddMenuEntry(
							LOCTEXT("AssignFaceNew", "New bank for this rig"),
							LOCTEXT("AssignFaceNewTip",
								"Create a face bank targeting this speaker's rig and put the "
								"line's clip in it."),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([PanelPtr, LineId]()
							{
								PanelPtr->OnAssignLineToNewBank(LineId);
							})));

						return Menu.MakeWidget();
					});
			}

			if (Column == ColumnSessions)
			{
				const FString Chips = Panel ? Panel->GetSessionChips(Status.Handle.LineId) : FString();
				return MakeTextCell(
					FText::FromString(Chips.IsEmpty() ? TEXT("-") : *Chips),
					LOCTEXT("SessionsTip",
						"Recording sessions that include this line. '-' means no session covers "
						"it yet - select lines and press Plan Session or Record Selected."),
					RowSound);
			}

			if (Column == ColumnPlay)
			{
				return MakePlayCell(RowSound);
			}

			return SNullWidget::NullWidget;
		}

	private:

		/** Who speaks this line - a dropdown over the cast, so re-assigning is one click. */
		TSharedRef<SWidget> MakeSpeakerCell()
		{
			const FSpeechLineHandle Handle = Row->Status.Handle;
			SSpeechLibraryPanel* PanelPtr = Panel;

			return SNew(SComboButton)
				.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
				.ToolTipText(LOCTEXT("SpeakerCellTip", "Who speaks this line. Define speakers on the Cast page."))
				.OnGetMenuContent_Lambda([PanelPtr, Handle]()
				{
					FMenuBuilder Menu(/*bShouldCloseAfterSelection=*/true, nullptr);
					if (!PanelPtr)
					{
						return Menu.MakeWidget();
					}
					for (const TSharedPtr<FSpeechCastRow>& Cast : PanelPtr->GetCastRows())
					{
						const FName SpeakerId = Cast->SpeakerId;
						Menu.AddMenuEntry(
							FText::FromString(Cast->GetLabel()),
							FText::FromName(SpeakerId),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([PanelPtr, Handle, SpeakerId]()
							{
								PanelPtr->SetLineSpeaker(Handle, SpeakerId);
							})));
					}
					return Menu.MakeWidget();
				})
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(FText::FromName(Row->Status.SpeakerId))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
		}

		/**
		 * The voice cell: shows the profile that answers, and opens the exception flow - a line
		 * override, set or cleared right where the question comes up.
		 */
		TSharedRef<SWidget> MakeVoiceCell()
		{
			const FSpeechLineHandle Handle = Row->Status.Handle;
			const FString OverridePath = Row->OverridePath;
			const FString Resolved = Row->VoiceLabel;
			const FString Source = Row->Status.ResolvedVoice.SourceDescription;
			SSpeechLibraryPanel* PanelPtr = Panel;

			const FString Shown = OverridePath.IsEmpty() ? Resolved : Resolved + TEXT(" *");

			return SNew(SComboButton)
				.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
				.ToolTipText(FText::FromString(Source.IsEmpty()
					? TEXT("No voice resolves. Cast the speaker on the Cast page.") : Source))
				.OnGetMenuContent_Lambda([PanelPtr, Handle, OverridePath, Resolved, Source]()
				{
					FMenuBuilder Menu(/*bShouldCloseAfterSelection=*/true, nullptr);
					if (!PanelPtr)
					{
						return Menu.MakeWidget();
					}

					Menu.BeginSection(NAME_None, FText::FromString(Source.IsEmpty()
						? TEXT("Unresolved") : FString::Printf(TEXT("Now: %s"), *Resolved)));
					if (!OverridePath.IsEmpty())
					{
						Menu.AddMenuEntry(
							LOCTEXT("ClearOverride", "Clear override - back to the speaker's voice"),
							FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([PanelPtr, Handle]()
							{
								PanelPtr->SetLineOverride(Handle, FString());
							})));
					}
					Menu.EndSection();

					Menu.BeginSection(NAME_None, LOCTEXT("OverrideWith", "Voice this one line with"));
					for (const TSharedPtr<FSpeechProfileRow>& Profile : PanelPtr->GetProfileRows())
					{
						const FString ProfilePath = Profile->AssetPath;
						Menu.AddMenuEntry(
							FText::FromString(Profile->Label),
							FText::FromString(FString::Printf(TEXT("%s voice %s"),
								*Profile->ProviderId.ToString(), *Profile->ProviderVoiceId)),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([PanelPtr, Handle, ProfilePath]()
							{
								PanelPtr->SetLineOverride(Handle, ProfilePath);
							})));
					}
					Menu.EndSection();

					return Menu.MakeWidget();
				})
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Shown))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
		}

		TSharedPtr<FSpeechLibraryRow> Row;
		SSpeechLibraryPanel* Panel = nullptr;
		bool bAuthoring = false;
	};

	/** One speaker on the Cast page. */
	class SSpeechCastRowWidget : public SMultiColumnTableRow<TSharedPtr<FSpeechCastRow>>
	{
	public:
		SLATE_BEGIN_ARGS(SSpeechCastRowWidget) : _Panel(nullptr) {}
			SLATE_ARGUMENT(TSharedPtr<FSpeechCastRow>, Entry)
			SLATE_ARGUMENT(SSpeechLibraryPanel*, Panel)
			SLATE_EVENT(FOnClicked, OnCreateSheet)
			SLATE_EVENT(FOnClicked, OnOpenSheet)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Entry = InArgs._Entry;
			Panel = InArgs._Panel;
			OnCreateSheet = InArgs._OnCreateSheet;
			OnOpenSheet = InArgs._OnOpenSheet;
			SMultiColumnTableRow<TSharedPtr<FSpeechCastRow>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			if (Column == TEXT("Speaker"))
			{
				const FString Label = Entry->GetLabel();
				return SNew(STextBlock)
					.Text(FText::FromString(Entry->LineCount > 0
						? FString::Printf(TEXT("%s  · in scene"), *Label) : Label))
					.ToolTipText(FText::FromString(FString::Printf(TEXT("Speaker id: %s%s"),
						*Entry->SpeakerId.ToString(),
						Entry->Description.IsEmpty() ? TEXT("") : *(TEXT("\n") + Entry->Description))))
					.Margin(FMargin(4, 2))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
			}
			if (Column == TEXT("CastVoice"))
			{
				return SNew(STextBlock)
					.Text(Entry->ProfileLabel.IsEmpty()
						? LOCTEXT("Uncast", "- uncast -") : FText::FromString(Entry->ProfileLabel))
					.ToolTipText(FText::FromString(Entry->ProfilePath))
					.Margin(FMargin(4, 2))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
			}
			if (Column == TEXT("Lines"))
			{
				return SNew(STextBlock).Text(FText::AsNumber(Entry->LineCount)).Margin(FMargin(4, 2));
			}
			if (Column == TEXT("Links"))
			{
				const FString Summary = FString::Join(Entry->BindingSummaries, TEXT("\n"));
				return SNew(STextBlock)
					.Text(Entry->BindingSummaries.Num() > 0
						? FText::AsNumber(Entry->BindingSummaries.Num()) : FText::FromString(TEXT("-")))
					.ToolTipText(FText::FromString(Summary))
					.Margin(FMargin(4, 2));
			}
			if (Column == TEXT("Sheet"))
			{
				const bool bHasSheet = !Entry->AssetPath.IsEmpty();
				return SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Center).Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(bHasSheet
						? LOCTEXT("OpenSheetTip", "Open the speaker's sheet - identity, voice, links.")
						: LOCTEXT("CreateSheetTip",
							"This id appears in lines but has no speaker sheet yet. Create one to cast a voice."))
					.OnClicked(bHasSheet ? OnOpenSheet : OnCreateSheet)
					[
						SNew(STextBlock).Text(bHasSheet
							? LOCTEXT("OpenSheet", "Open") : LOCTEXT("CreateSheet", "Create sheet"))
					]
				];
			}
			return SNullWidget::NullWidget;
		}

	private:
		TSharedPtr<FSpeechCastRow> Entry;
		SSpeechLibraryPanel* Panel = nullptr;
		FOnClicked OnCreateSheet;
		FOnClicked OnOpenSheet;
	};
}

void SSpeechLibraryPanel::Construct(const FArguments& InArgs)
{
	ModeOptions = {
		MakeShared<FString>(TEXT("Voice + Face")),
		MakeShared<FString>(TEXT("Voice")),
		MakeShared<FString>(TEXT("Face only")) };
	TreatmentOptions = {
		MakeShared<FString>(TEXT("Convert to cast voice")),
		MakeShared<FString>(TEXT("Use performance as-is")) };
	ChosenMode = ModeOptions[0];
	ChosenTreatment = TreatmentOptions[0];

	RefreshBanks();
	RefreshFaceBanks();
	DiscoverActions();

	// Somebody elsewhere in the pipeline asked to see a line - a recording session, a face bank, an
	// agent. Navigation is the thing this pipeline most obviously lacked: everything could reach
	// everything by asset path, and a person could reach none of it without searching by hand.
	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		FocusHandle = Forge->OnLibraryFocusRequested.AddSP(this, &SSpeechLibraryPanel::FocusOn);

		// Coalesced on purpose. A batch generation writes once per line, and refreshing the table a
		// hundred times would be worse than the stale rows this fixes; one refresh on the next tick
		// is the same answer for a fraction of the work.
		ChangedHandle = Forge->OnLibraryChanged.AddSP(this, &SSpeechLibraryPanel::MarkNeedsRefresh);
	}

	for (const FName ProviderId : USpeechForgeSubsystem::Get()
		? USpeechForgeSubsystem::Get()->GetProviderIds() : TArray<FName>())
	{
		ProviderOptions.Add(MakeShared<FName>(ProviderId));
	}
	if (ProviderOptions.Num() > 0)
	{
		ChosenProvider = ProviderOptions[0];
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// Bank picker on top; the page switcher on its own row below, where a workflow reads as a
		// workflow. Inline with Refresh it read as another bank control and took finding.
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("BankLabel", "Bank"))
			]
			// The engine's own asset picker: searchable, browse-to-asset, use-selected - a plain
			// dropdown stops scaling somewhere around a dozen banks, and a game has hundreds.
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 8, 0)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(USpeechBank::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([this]()
				{
					return ChosenBank.IsValid() ? *ChosenBank : FString();
				})
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					ChosenBank = Asset.IsValid()
						? TSharedPtr<FString>(MakeShared<FString>(Asset.GetObjectPathString()))
						: TSharedPtr<FString>();

					// A pick belongs to the bank it was made for.
					IngestPickedAsset.Reset();
					RefreshAll();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.OnClicked_Lambda([this]() { RefreshBanks(); RefreshAll(); return FReply::Handled(); })
			]
		]

		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(8, 6, 8, 4)
		[
			SAssignNew(PageControl, SSegmentedControl<int32>)
			.Value_Lambda([this]() { return ActivePage; })
			.OnValueChanged_Lambda([this](int32 NewPage) { ActivePage = NewPage; })
			.UniformPadding(FMargin(24.f, 4.f))
		]

		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return ActivePage; })

			+ SWidgetSwitcher::Slot()
			[
				MakeIngestPage()
			]

			+ SWidgetSwitcher::Slot()
			[
				MakeCastPage()
			]

			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 4)
				[
					SAssignNew(WriteList, SListView<TSharedPtr<FSpeechLibraryRow>>)
					.ListItemsSource(&Rows)
					.OnGenerateRow(this, &SSpeechLibraryPanel::MakeWriteRow)
					.OnSelectionChanged(this, &SSpeechLibraryPanel::OnWriteSelectionChanged)
					// Multi, for Delete Selected. The line editor follows the row the selection
					// event names, which with a range-select is the one under the cursor.
					.SelectionMode(ESelectionMode::Multi)
					.HeaderRow(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(ColumnLine).DefaultLabel(LOCTEXT("ColLine", "Line")).FillWidth(0.10f)
						+ SHeaderRow::Column(ColumnSpeaker).DefaultLabel(LOCTEXT("ColSpeaker", "Speaker")).FillWidth(0.12f)
						+ SHeaderRow::Column(ColumnText).DefaultLabel(LOCTEXT("ColText", "Text")).FillWidth(0.42f)
						+ SHeaderRow::Column(ColumnDirection).DefaultLabel(LOCTEXT("ColDirection", "Direction")).FillWidth(0.16f)
						+ SHeaderRow::Column(ColumnVoice).DefaultLabel(LOCTEXT("ColVoice", "Voice")).FillWidth(0.16f)
						+ SHeaderRow::Column(ColumnPlay).DefaultLabel(FText::GetEmpty()).FixedWidth(28.f))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("DeleteLines", "Delete Selected"))
						.ToolTipText(LOCTEXT("DeleteLinesTip",
							"Remove the selected lines from this bank. Their generated audio "
							"assets stay in the project."))
						.IsEnabled_Lambda([this]()
						{
							return WriteList.IsValid() && WriteList->GetNumItemsSelected() > 0;
						})
						.OnClicked(this, &SSpeechLibraryPanel::OnDeleteSelectedLines)
					]

					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearLines", "Clear All Lines"))
						.ToolTipText(LOCTEXT("ClearLinesTip",
							"Remove every line from this bank, keeping its cast, language and "
							"source. Audio assets stay in the project."))
						.IsEnabled_Lambda([this]() { return Rows.Num() > 0; })
						.OnClicked(this, &SSpeechLibraryPanel::OnClearBankLines)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
				[
					MakeLineEditor()
				]
			]

			+ SWidgetSwitcher::Slot()
			[
				MakeProducePage()
			]

			+ SWidgetSwitcher::Slot()
			[
				MakePerformPage()
			]

			+ SWidgetSwitcher::Slot()
			[
				MakeLocalizePage()
			]
		]
	];

	// The segments, in the order the work happens: Ingest leads when anything can ingest, Perform
	// closes when anything can put a face or a performance on a line - a page that could only say
	// "install something" is noise, so either is absent without its plugin. Built imperatively
	// because a declarative slot cannot be conditional; the switcher's slots match the Page
	// constants either way.
	const bool bPerformPage = DiscoveredActions.ContainsByPredicate(
		[](const TSharedPtr<FSpeechLibraryAction>& Action)
		{
			return Action.IsValid() && Action->Page == TEXT("Perform");
		});

	// "Localization capability" means a translator that produces real words: any registered
	// provider but the core's own pseudo-localiser.
	if (FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr())
	{
		for (const FName Id : Module->GetTranslationProviderIds())
		{
			if (Id != FName(TEXT("Pseudo")))
			{
				bLocalizePage = true;
				break;
			}
		}
	}

	if (IngestActions.Num() > 0)
	{
		PageControl->AddSlot(PageIngest, /*bRebuildChildren=*/false)
			.Text(LOCTEXT("PageIngest", "Ingest"))
			.ToolTip(LOCTEXT("PageIngestTip",
				"Where the lines come from: harvest a dialogue, re-harvest after script edits, "
				"and see whether bank and source still agree."));
	}
	PageControl->AddSlot(PageCast, /*bRebuildChildren=*/false)
		.Text(LOCTEXT("PageCast", "Cast"))
		.ToolTip(LOCTEXT("PageCastTip", "Who is in the scene, and what they sound like."));
	PageControl->AddSlot(PageWrite, /*bRebuildChildren=*/false)
		.Text(LOCTEXT("PageWrite", "Write"))
		.ToolTip(LOCTEXT("PageWriteTip", "The lines - text, direction, speaker, and per-line voice exceptions."));
	PageControl->AddSlot(PageProduce, /*bRebuildChildren=*/false)
		.Text(LOCTEXT("PageProduce", "Produce"))
		.ToolTip(LOCTEXT("PageProduceTip", "Generate audio - selection, cost, and the pipeline table."));
	if (bPerformPage)
	{
		PageControl->AddSlot(PagePerform, /*bRebuildChildren=*/false)
			.Text(LOCTEXT("PagePerform", "Perform"))
			.ToolTip(LOCTEXT("PagePerformTip",
				"Lines become bodies: face banks, each line's rig and face status, and the "
				"recording sessions that cover this bank."));
	}
	// Localize closes the row, and only when a real translator is installed: the keyless
	// pseudo-localiser that ships with the core proves the pipeline for agents, but a page whose
	// only language is gibberish is not a surface.
	if (bLocalizePage)
	{
		PageControl->AddSlot(PageLocalize, /*bRebuildChildren=*/false)
			.Text(LOCTEXT("PageLocalize", "Localize"))
			.ToolTip(LOCTEXT("PageLocalizeTip",
				"One sibling bank per language, joined to this one by line id - with its face banks "
				"cloned alongside. Choose languages, create, then generate and solve them like any bank."));
	}
	PageControl->RebuildChildren();

	ActivePage = IngestActions.Num() > 0 ? PageIngest : PageCast;

	RefreshAll();
}

USpeechForgeSubsystem* SSpeechLibraryPanel::Subsystem() const
{
	return USpeechForgeSubsystem::Get();
}

// -------------------------------------------------------------------------------------------------
// Banks and refresh order
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::MarkNeedsRefresh()
{
	bPendingRefresh = true;
}

void SSpeechLibraryPanel::Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);

	if (bPendingRefresh)
	{
		bPendingRefresh = false;
		RefreshRows();
	}
}

void SSpeechLibraryPanel::FocusOn(const FString& BankPath, FName LineId)
{
	// A bank made moments ago may not be in the combo's list yet; a double-click on it must still
	// land, so the list catches up before the match runs.
	const bool bKnown = BankPaths.ContainsByPredicate([&BankPath](const TSharedPtr<FString>& Option)
	{
		return Option.IsValid() && (Option->Contains(BankPath) || BankPath.Contains(*Option));
	});
	if (!bKnown)
	{
		RefreshBanks();
	}

	// Bank paths arrive in both forms - "/Game/X/SB_A" and "/Game/X/SB_A.SB_A" - depending on who
	// stored them, so the match is by either containing the other rather than by equality.
	for (const TSharedPtr<FString>& Option : BankPaths)
	{
		if (!Option.IsValid())
		{
			continue;
		}

		if (*Option == BankPath || Option->Contains(BankPath) || BankPath.Contains(*Option))
		{
			ChosenBank = Option;
			ActivePage = PageWrite;

			// A pick belongs to the bank it was made for; this bank shows its own stamp instead.
			IngestPickedAsset.Reset();

			// Everything, not just the rows: the cast, face banks and sessions all follow the
			// bank, and a focus is a bank change like any other.
			RefreshAll();
			break;
		}
	}

	if (LineId.IsNone() || !WriteList.IsValid())
	{
		return;
	}

	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		if (Row.IsValid() && Row->Status.Handle.LineId == LineId)
		{
			WriteList->SetSelection(Row, ESelectInfo::Direct);
			WriteList->RequestScrollIntoView(Row);
			break;
		}
	}
}

void SSpeechLibraryPanel::RefreshBanks()
{
	BankPaths.Reset();

	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		for (const FString& Path : Forge->FindSpeechAssets())
		{
			BankPaths.Add(MakeShared<FString>(Path));
		}
	}

	// Deliberately NO default selection. A panel that silently opens on whichever bank sorts
	// first looks like a statement about that bank - the person picks, or a double-click on a
	// bank asset picks for them.

	if (BankCombo.IsValid())
	{
		BankCombo->RefreshOptions();
	}
}

TSharedRef<SWidget> SSpeechLibraryPanel::MakeBankEntry(TSharedPtr<FString> Path)
{
	// Base filename, not GetShortName: the paths are object paths ("/Game/X/SB_A.SB_A"), and
	// GetShortName keeps the ".SB_A" suffix a human should never have to read.
	return SNew(STextBlock).Text(FText::FromString(FPaths::GetBaseFilename(*Path)));
}

void SSpeechLibraryPanel::OnBankChosen(TSharedPtr<FString> Path, ESelectInfo::Type)
{
	ChosenBank = Path;

	// A pick belongs to the bank it was made for; the new bank shows its own stamp instead.
	IngestPickedAsset.Reset();

	RefreshAll();
}

FText SSpeechLibraryPanel::BankComboLabel() const
{
	if (ChosenBank.IsValid())
	{
		return FText::FromString(FPaths::GetBaseFilename(*ChosenBank));
	}

	return BankPaths.Num() > 0
		? LOCTEXT("PickABank", "Pick a bank...")
		: LOCTEXT("NoBank", "No speech banks found");
}

void SSpeechLibraryPanel::RefreshAll()
{
	// Dependency order: profile labels name the rows' voices, and the rows' speaker ids and counts
	// shape the cast list.
	RefreshProfiles();
	RefreshRows();
	RefreshCast();

	// The face bank follows the speech bank, because the link belongs to the bank and not to the
	// panel. Without this a bank change left the previous scene's faces selected.
	RefreshFaceBanks();

	// Perform reads the cast (rigs) and the registry (face banks, sessions), so it goes last.
	RefreshPerform();

	// Localize reads Perform's face-bank facts to report the clones, so after it.
	RefreshLocalize();
}

// -------------------------------------------------------------------------------------------------
// Lines
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::RefreshRows()
{
	Rows.Reset();

	USpeechForgeSubsystem* Forge = Subsystem();
	if (Forge && ChosenBank.IsValid())
	{
		const TArray<FSpeechLineHandle> Handles =
			Forge->ExpandHandles({ FSpeechLineHandle(*ChosenBank, NAME_None) });

		// Outside edits are detected here rather than waiting to be asked for. This used to be a
		// button nobody pressed, which meant a de-essed line stayed marked Generated until the
		// pipeline overwrote it. It is affordable on every refresh because it stats each audio
		// package first and only hashes the ones written since it last looked.
		Forge->DetectEditedAudio(Handles);

		// Whether this bank's own script has moved underneath it. One file time, not a text compare.
		BankSourceDrifted = false;
		for (const FSpeechSourceDrift& Drift : Forge->CheckSourceDrift())
		{
			if (Drift.BankPath.Contains(*ChosenBank) || ChosenBank->Contains(Drift.BankPath))
			{
				BankSourceDrifted = true;
				BankSourceDriftDetail = Drift.SourceWrittenAt == FDateTime()
					? FString::Printf(TEXT("its source '%s' no longer exists"),
						*FPaths::GetBaseFilename(Drift.SourceAssetPath))
					: FString::Printf(TEXT("'%s' was written %s, after this bank last read it"),
						*FPaths::GetBaseFilename(Drift.SourceAssetPath),
						*Drift.SourceWrittenAt.ToString());
				break;
			}
		}

		// Text and direction come off the asset directly - the status report deliberately carries
		// pipeline facts, and the teleprompter needs the words.
		UObject* Asset = LoadObject<UObject>(nullptr, **ChosenBank);
		ISpeechLineSource* Source = Asset ? Cast<ISpeechLineSource>(Asset) : nullptr;

		// The source stamp decides which write-backs the line editor offers, and whether it warns
		// that a bank-only edit dies at the next harvest.
		BankSourceAdapter = NAME_None;
		BankSourceAssetPath.Reset();
		if (const USpeechBank* AsBank = Cast<USpeechBank>(Asset))
		{
			BankSourceAdapter = AsBank->SourceAdapter;
			BankSourceAssetPath = AsBank->SourceAssetPath;
		}

		// An empty bank must list nothing. GetLineStatus reads an empty handle list as "the whole
		// library" - the right meaning for a tool asked about everything, the wrong one for a panel
		// scoped to one bank that simply has no lines yet. Without this guard a freshly created
		// bank showed every line of every bank in the project, localised siblings included.
		const TArray<FSpeechLineStatus> LineStatus =
			Handles.Num() > 0 ? Forge->GetLineStatus(Handles) : TArray<FSpeechLineStatus>();

		// On a localised bank, which lines have a performance behind them in the source language.
		// Loaded once for the whole refresh rather than asked per row: the answer is the same for
		// every line, and it decides whether dubbing is offered at all.
		const USpeechBank* SourceLanguageBank = nullptr;
		bBankIsLocalized = false;
		if (const USpeechBank* AsBank = Cast<USpeechBank>(Asset))
		{
			if (!AsBank->LanguageCode.IsEmpty() && !AsBank->SourceBankPath.IsEmpty())
			{
				bBankIsLocalized = true;
				SourceLanguageBank = LoadObject<USpeechBank>(nullptr, *AsBank->SourceBankPath);
			}
		}

		for (const FSpeechLineStatus& Status : LineStatus)
		{
			TSharedRef<FSpeechLibraryRow> Row = MakeShared<FSpeechLibraryRow>();
			Row->Status = Status;

			if (Source)
			{
				if (const FSpeechLine* Line = Source->FindLine(Status.Handle.LineId))
				{
					Row->Text = Line->Text;
					Row->Direction = Line->Direction;
					if (!Line->VoiceOverride.IsNull())
					{
						Row->OverridePath = Line->VoiceOverride.ToSoftObjectPath().ToString();
					}
				}
			}

			// Dubbable when the source line is a performance rather than synthesis. A generated
			// source is better regenerated in the new language: same voice, right words, a fraction
			// of the price. A performed one is what dubbing exists for.
			if (SourceLanguageBank)
			{
				if (const FSpeechLine* SourceLine = SourceLanguageBank->FindLine(Status.Handle.LineId))
				{
					Row->bSourceRecorded =
						SourceLine->HasAudio() && SourceLine->Origin != ESpeechLineOrigin::Generated;
				}
			}

			// The label a human reads: the profile's name. The override's profile when one is set,
			// else whichever profile the resolution's provider voice belongs to.
			if (!Row->OverridePath.IsEmpty())
			{
				const FString* Label = ProfileLabelByPath.Find(Row->OverridePath);
				Row->VoiceLabel = Label ? *Label : FPackageName::GetShortName(Row->OverridePath);
			}
			else if (!Status.ResolvedVoice.ProviderVoiceId.IsEmpty())
			{
				const FString Key = FString::Printf(TEXT("%s|%s"),
					*Status.ResolvedVoice.ProviderId.ToString(), *Status.ResolvedVoice.ProviderVoiceId);
				const FString* Label = ProfileLabelByKey.Find(Key);
				Row->VoiceLabel = Label ? *Label : Status.ResolvedVoice.ProviderVoiceId.Left(8);
			}
			else
			{
				Row->VoiceLabel = TEXT("-");
			}

			Rows.Add(Row);
		}
	}

	if (WriteList.IsValid())
	{
		WriteList->RequestListRefresh();
	}
	if (ProduceList.IsValid())
	{
		ProduceList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SSpeechLibraryPanel::MakeWriteRow(
	TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SSpeechLineRow, Owner).Row(Row).Panel(this).bAuthoring(true);
}

TSharedRef<ITableRow> SSpeechLibraryPanel::MakeProduceRow(
	TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SSpeechLineRow, Owner).Row(Row).Panel(this).bAuthoring(false);
}

TSharedRef<ITableRow> SSpeechLibraryPanel::MakePerformRow(
	TSharedPtr<FSpeechLibraryRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	// The same row class as Produce; the Perform header simply asks it different columns.
	return SNew(SSpeechLineRow, Owner).Row(Row).Panel(this).bAuthoring(false);
}

TArray<FSpeechLineHandle> SSpeechLibraryPanel::SelectedHandles() const
{
	TArray<FSpeechLineHandle> Handles;
	if (ProduceList.IsValid())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : ProduceList->GetSelectedItems())
		{
			Handles.Add(Row->Status.Handle);
		}
	}
	return Handles;
}

void SSpeechLibraryPanel::SetLineSpeaker(const FSpeechLineHandle& Handle, FName SpeakerId)
{
	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
		{
			if (Row->Status.Handle == Handle)
			{
				Forge->UpdateLineAuthoring(Handle, Row->Text, Row->Direction, SpeakerId);
				break;
			}
		}
		RefreshAll();
	}
}

void SSpeechLibraryPanel::SetLineOverride(const FSpeechLineHandle& Handle, const FString& ProfilePath)
{
	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		Forge->SetLineVoiceOverride(Handle, ProfilePath);
		RefreshAll();
	}
}

// -------------------------------------------------------------------------------------------------
// The line editor
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SSpeechLibraryPanel::MakeLineEditor()
{
	// One button per discovered write-back, each visible only when the current bank's source stamp
	// carries its adapter's key - the with-or-without-extensions boundary, drawn per bank.
	TSharedRef<SHorizontalBox> SyncButtons = SNew(SHorizontalBox);
	for (const TSharedPtr<FSpeechLineSyncAction>& Action : SyncActions)
	{
		SyncButtons->AddSlot().AutoWidth().Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.Visibility_Lambda([this, Action]()
			{
				return !BankSourceAdapter.IsNone() && Action->AdapterKey == BankSourceAdapter
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ToolTipText(FText::FromString(Action->Tooltip))
			.OnClicked(this, &SSpeechLibraryPanel::OnSyncLineEdit, Action)
			[
				SNew(STextBlock).Text(FText::FromString(Action->Label))
			]
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8)
		.Visibility_Lambda([this]()
		{
			return EditingHandle.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return FText::Format(LOCTEXT("EditHeader", "Edit line - {0}"),
							FText::FromName(EditingHandle.LineId));
					})
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						if (BankSourceAdapter.IsNone())
						{
							return LOCTEXT("OwnSource", "This bank is its own source - an edit here is the whole edit.");
						}
						return FText::Format(LOCTEXT("HarvestedSource",
							"Harvested from {0} via {1}. An edit kept only here is overwritten by the next harvest - write it back to stay in step."),
							FText::FromString(FPaths::GetBaseFilename(BankSourceAssetPath)),
							FText::FromName(BankSourceAdapter));
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			]

			// Drift the bank cannot see on its own: the script moved somewhere else. Suspicion
			// rather than proof - a resave with no text change lands here too - so it says what it
			// observed and what to press, instead of claiming lines are wrong.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					return BankSourceDrifted ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("SourceDrifted",
						"The script this bank came from has changed - {0}. Re-harvest to pull the "
						"edits in; any line whose words moved will then say so."),
						FText::FromString(BankSourceDriftDetail));
				})
				.ColorAndOpacity(FSlateColor(FStyleColors::Warning))
				.AutoWrapText(true)
			]

			// The scream. A generated line going stale is a re-read for a cent; a recorded line
			// going stale means the subtitle and a human performance now disagree, and the only
			// fixes are a re-record or a pickup session. That difference is loud, and it is loud
			// BEFORE the edit, not after.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					const TSharedPtr<FSpeechLibraryRow> Row = FindEditingRow();
					return Row.IsValid() &&
						(Row->Status.Origin == ESpeechLineOrigin::Recorded ||
						 Row->Status.Origin == ESpeechLineOrigin::Edited)
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					const TSharedPtr<FSpeechLibraryRow> Row = FindEditingRow();
					if (Row.IsValid() && Row->Status.bStale)
					{
						return LOCTEXT("PerformanceStale",
							"OUT OF STEP: this text no longer matches the recorded performance behind it. "
							"Regeneration will not touch a recorded line - re-record it or book a pickup.");
					}
					return LOCTEXT("PerformanceWarning",
						"A recorded performance is behind this line. Rewriting the text puts the subtitle "
						"and the performance out of step - fixing that is a re-record or a pickup session, "
						"never a regeneration.");
				})
				.ColorAndOpacity_Lambda([this]() -> FSlateColor
				{
					const TSharedPtr<FSpeechLibraryRow> Row = FindEditingRow();
					return Row.IsValid() && Row->Status.bStale
						? FSlateColor(FStyleColors::Error) : FSlateColor(FStyleColors::Warning);
				})
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SAssignNew(EditTextBox, SMultiLineEditableTextBox)
				.HintText(LOCTEXT("TextHint", "What the player reads and what is spoken."))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SAssignNew(EditDirectionBox, SEditableTextBox)
				.HintText(LOCTEXT("DirectionHint",
					"Direction - whispers, exhausted, shouting. Never spoken, never in the subtitle."))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return LineEditorMessage; })
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ApplyEdit", "Apply to bank"))
					.ToolTipText(LOCTEXT("ApplyEditTip",
						"Save onto the bank line. Changed text reads as stale in Produce - a priced report, never a silent regeneration."))
					.OnClicked(this, &SSpeechLibraryPanel::OnApplyLineEdit)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SyncButtons
				]
			]
		];
}

TSharedPtr<FSpeechLibraryRow> SSpeechLibraryPanel::FindEditingRow() const
{
	if (EditingHandle.IsValid())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
		{
			if (Row->Status.Handle == EditingHandle)
			{
				return Row;
			}
		}
	}
	return nullptr;
}

void SSpeechLibraryPanel::OnWriteSelectionChanged(TSharedPtr<FSpeechLibraryRow> Row, ESelectInfo::Type)
{
	EditingHandle = Row.IsValid() ? Row->Status.Handle : FSpeechLineHandle();
	LineEditorMessage = FText::GetEmpty();

	if (EditTextBox.IsValid())
	{
		EditTextBox->SetText(Row.IsValid() ? FText::FromString(Row->Text) : FText::GetEmpty());
	}
	if (EditDirectionBox.IsValid())
	{
		EditDirectionBox->SetText(Row.IsValid() ? FText::FromString(Row->Direction) : FText::GetEmpty());
	}
}

FReply SSpeechLibraryPanel::OnApplyLineEdit()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !EditingHandle.IsValid() || !EditTextBox.IsValid() || !EditDirectionBox.IsValid())
	{
		return FReply::Handled();
	}

	const FSpeechLineHandle Handle = EditingHandle;
	Forge->UpdateLineAuthoring(Handle,
		EditTextBox->GetText().ToString(),
		EditDirectionBox->GetText().ToString(),
		NAME_None);

	LineEditorMessage = LOCTEXT("Applied", "Saved to the bank.");
	RefreshAll();

	// The refresh rebuilt the rows; put the selection - and therefore the editor - back on the
	// same line, so an apply does not end the person's editing session.
	if (WriteList.IsValid())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
		{
			if (Row->Status.Handle == Handle)
			{
				WriteList->SetSelection(Row, ESelectInfo::Direct);
				break;
			}
		}
	}
	LineEditorMessage = LOCTEXT("Applied", "Saved to the bank.");

	return FReply::Handled();
}

TArray<FName> SSpeechLibraryPanel::SelectedWriteLineIds() const
{
	TArray<FName> LineIds;
	if (WriteList.IsValid())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : WriteList->GetSelectedItems())
		{
			if (Row.IsValid())
			{
				LineIds.Add(Row->Status.Handle.LineId);
			}
		}
	}
	return LineIds;
}

FReply SSpeechLibraryPanel::OnDeleteSelectedLines()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const TArray<FName> LineIds = SelectedWriteLineIds();
	if (!Forge || !ChosenBank.IsValid() || LineIds.Num() == 0)
	{
		return FReply::Handled();
	}

	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("DeleteLinesAsk",
			"Remove {0} line(s) from {1}?\n\nTheir text and direction are lost; generated audio "
			"assets stay in the project."),
			LineIds.Num(),
			FText::FromString(FPaths::GetBaseFilename(*ChosenBank))));

	if (Choice != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	FString Error;
	const int32 Removed = Forge->RemoveBankLines(*ChosenBank, LineIds, Error);

	LineEditorMessage = Error.IsEmpty()
		? FText::Format(LOCTEXT("DeletedLines", "Removed {0} line(s). Audio assets stay."), Removed)
		: FText::FromString(Error);

	// The edited line may be among the removed; a stale handle would resurrect it on Apply.
	if (LineIds.Contains(EditingHandle.LineId))
	{
		EditingHandle = FSpeechLineHandle();
	}

	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnClearBankLines()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !ChosenBank.IsValid() || Rows.Num() == 0)
	{
		return FReply::Handled();
	}

	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("ClearLinesAsk",
			"Remove all {0} line(s) from {1}?\n\nThe cast, language and source stamp stay, and so "
			"do generated audio assets. The lines' text and direction are lost."),
			Rows.Num(),
			FText::FromString(FPaths::GetBaseFilename(*ChosenBank))));

	if (Choice != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	FString Error;
	const int32 Removed = Forge->ClearBankLines(*ChosenBank, Error);

	LineEditorMessage = Error.IsEmpty()
		? FText::Format(LOCTEXT("ClearedLines", "Removed {0} line(s). Audio assets stay."), Removed)
		: FText::FromString(Error);

	EditingHandle = FSpeechLineHandle();
	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnSyncLineEdit(TSharedPtr<FSpeechLineSyncAction> Action)
{
	// The bank half first: the write-back reads the bank, so the bank must already say what the
	// editor says.
	OnApplyLineEdit();

	UClass* ToolClass = Action->ToolClass.Get();
	UFunction* Function = ToolClass ? ToolClass->FindFunctionByName(Action->FunctionName) : nullptr;
	if (!Function || !EditingHandle.IsValid())
	{
		LineEditorMessage = LOCTEXT("SyncGone", "That write-back's plugin is no longer loaded.");
		return FReply::Handled();
	}

	// The contract's shape: two strings in declared order - the bank, then the line - and a string
	// back saying what happened.
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(Function->ParmsSize));
	FMemory::Memzero(Frame, Function->ParmsSize);

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(Frame);
		}
	}

	int32 StringIndex = 0;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) &&
			!It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				AsString->SetPropertyValue_InContainer(Frame,
					StringIndex == 0 ? EditingHandle.AssetPath : EditingHandle.LineId.ToString());
				++StringIndex;
			}
		}
	}

	ToolClass->GetDefaultObject()->ProcessEvent(Function, Frame);

	FString ResultText;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				ResultText = AsString->GetPropertyValue_InContainer(Frame);
			}
		}
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->DestroyValue_InContainer(Frame);
		}
	}

	LineEditorMessage = ResultText.IsEmpty()
		? LOCTEXT("SyncFailed", "The write-back refused - the Output Log says why.")
		: FText::FromString(ResultText);

	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Cast page
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::RefreshProfiles()
{
	ProfileRows.Reset();
	ProfileLabelByKey.Reset();
	ProfileLabelByPath.Reset();

	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge)
	{
		return;
	}

	for (const FString& Path : Forge->FindVoiceProfiles())
	{
		const USpeechVoiceProfile* Profile = LoadObject<USpeechVoiceProfile>(nullptr, *Path);
		if (!Profile)
		{
			continue;
		}

		TSharedRef<FSpeechProfileRow> Row = MakeShared<FSpeechProfileRow>();
		Row->AssetPath = Path;
		Row->Label = Profile->GetLabel();
		Row->ProviderId = Profile->ProviderId;
		Row->ProviderVoiceId = Profile->ProviderVoiceId;
		ProfileRows.Add(Row);

		ProfileLabelByPath.Add(Path, Row->Label);
		if (!Profile->ProviderVoiceId.IsEmpty())
		{
			ProfileLabelByKey.Add(FString::Printf(TEXT("%s|%s"),
				*Profile->ProviderId.ToString(), *Profile->ProviderVoiceId), Row->Label);
		}
	}

	if (ProfileList.IsValid())
	{
		ProfileList->RequestListRefresh();
	}
}

void SSpeechLibraryPanel::RefreshCast()
{
	// Remember the selection across the rebuild - assigning a voice should not deselect the speaker.
	const TSharedPtr<FSpeechCastRow> Previous = SelectedSpeaker();
	const FName PreviousId = Previous.IsValid() ? Previous->SpeakerId : NAME_None;

	CastRows.Reset();

	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge)
	{
		return;
	}

	// Lines in the current bank, by speaker - the "in scene" facts.
	TMap<FName, int32> BankLineCounts;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		++BankLineCounts.FindOrAdd(Row->Status.SpeakerId);
	}

	// Every sheet in the project.
	TSet<FName> Sheeted;
	for (const FString& Path : Forge->FindSpeakerAssets())
	{
		const USpeechSpeaker* Speaker = LoadObject<USpeechSpeaker>(nullptr, *Path);
		if (!Speaker)
		{
			continue;
		}

		TSharedRef<FSpeechCastRow> Row = MakeShared<FSpeechCastRow>();
		Row->SpeakerId = Speaker->SpeakerId;
		Row->DisplayName = Speaker->DisplayName;
		Row->Description = Speaker->Description;
		Row->AssetPath = Path;
		Row->LineCount = BankLineCounts.FindRef(Speaker->SpeakerId);

		if (!Speaker->VoiceProfile.IsNull())
		{
			Row->ProfilePath = Speaker->VoiceProfile.ToSoftObjectPath().ToString();
			const FString* Label = ProfileLabelByPath.Find(Row->ProfilePath);
			Row->ProfileLabel = Label ? *Label : FPackageName::GetShortName(Row->ProfilePath);
		}

		for (const TPair<FName, FSoftObjectPath>& Binding : Speaker->ExternalBindings)
		{
			Row->BindingSummaries.Add(FString::Printf(TEXT("%s -> %s"),
				*Binding.Key.ToString(), *Binding.Value.GetAssetName()));
		}

		Sheeted.Add(Speaker->SpeakerId);
		CastRows.Add(Row);
	}

	// Ids that speak in this bank but have no sheet yet - shown so the gap is one click to close.
	for (const TPair<FName, int32>& Count : BankLineCounts)
	{
		if (!Count.Key.IsNone() && !Sheeted.Contains(Count.Key))
		{
			TSharedRef<FSpeechCastRow> Row = MakeShared<FSpeechCastRow>();
			Row->SpeakerId = Count.Key;
			Row->LineCount = Count.Value;
			CastRows.Add(Row);
		}
	}

	// Used-by counts for the profile list.
	for (const TSharedPtr<FSpeechProfileRow>& Profile : ProfileRows)
	{
		Profile->UsedBySpeakers = 0;
		for (const TSharedPtr<FSpeechCastRow>& Cast : CastRows)
		{
			if (Cast->ProfilePath == Profile->AssetPath)
			{
				++Profile->UsedBySpeakers;
			}
		}
	}

	// The scene by default: the whole ensemble is one checkbox away, and a project with fifty
	// speakers should not bury the four who are actually in this bank. After the used-by counts,
	// which are project facts and must not follow the filter.
	if (!bShowAllSpeakers)
	{
		CastRows.RemoveAll([](const TSharedPtr<FSpeechCastRow>& Row)
		{
			return Row->LineCount == 0;
		});
	}

	// The scene's people first, then the rest of the ensemble alphabetically.
	CastRows.StableSort([](const TSharedPtr<FSpeechCastRow>& A, const TSharedPtr<FSpeechCastRow>& B)
	{
		if ((A->LineCount > 0) != (B->LineCount > 0))
		{
			return A->LineCount > 0;
		}
		return A->GetLabel() < B->GetLabel();
	});

	if (CastList.IsValid())
	{
		CastList->RequestListRefresh();

		TSharedPtr<FSpeechCastRow> ToSelect;
		for (const TSharedPtr<FSpeechCastRow>& Row : CastRows)
		{
			if (Row->SpeakerId == PreviousId)
			{
				ToSelect = Row;
				break;
			}
		}
		if (!ToSelect.IsValid() && CastRows.Num() > 0)
		{
			ToSelect = CastRows[0];
		}
		if (ToSelect.IsValid())
		{
			CastList->SetSelection(ToSelect);
		}
	}
}

TSharedPtr<FSpeechCastRow> SSpeechLibraryPanel::SelectedSpeaker() const
{
	if (CastList.IsValid())
	{
		TArray<TSharedPtr<FSpeechCastRow>> Selected = CastList->GetSelectedItems();
		if (Selected.Num() > 0)
		{
			return Selected[0];
		}
	}
	return nullptr;
}

TSharedRef<SWidget> SSpeechLibraryPanel::MakeCastPage()
{
	return SNew(SBox).Padding(FMargin(8, 4))
	[
		SNew(SSplitter).Orientation(Orient_Horizontal)

		// Left: the cast - every speaker, sheeted or not, the scene's people first.
		+ SSplitter::Slot().Value(0.5f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 8, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CastHeader", "The cast"))
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						return bShowAllSpeakers ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						bShowAllSpeakers = State == ECheckBoxState::Checked;
						RefreshCast();
					})
					.ToolTipText(LOCTEXT("ShowAllSpeakersTip",
						"Off: only the speakers with lines in this bank - the scene. On: the whole "
						"project cast."))
					[
						SNew(STextBlock).Text(LOCTEXT("ShowAllSpeakers", "Show all speakers"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SAssignNew(NewSpeakerBox, SEditableTextBox)
					.HintText(LOCTEXT("NewSpeakerHint", "New speaker id..."))
					.MinDesiredWidth(120.f)
					.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type Commit)
					{
						if (Commit == ETextCommit::OnEnter)
						{
							OnAddSpeaker();
						}
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("AddSpeaker", "Add"))
					.ToolTipText(LOCTEXT("AddSpeakerTip",
						"Create a speaker sheet. Define the cast before the scene - lines then name the speaker by this id."))
					.OnClicked(this, &SSpeechLibraryPanel::OnAddSpeaker)
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.f).Padding(0, 0, 8, 0)
			[
				SAssignNew(CastList, SListView<TSharedPtr<FSpeechCastRow>>)
				.ListItemsSource(&CastRows)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([this](TSharedPtr<FSpeechCastRow> Entry, const TSharedRef<STableViewBase>& Owner)
				{
					return SNew(SSpeechCastRowWidget, Owner)
						.Entry(Entry)
						.Panel(this)
						.OnCreateSheet_Lambda([this, Entry]() { return OnCreateSheet(Entry); })
						.OnOpenSheet_Lambda([this, Entry]() { return OnOpenAsset(Entry->AssetPath); });
				})
				.HeaderRow(
					SNew(SHeaderRow)
					+ SHeaderRow::Column(TEXT("Speaker")).DefaultLabel(LOCTEXT("CastColSpeaker", "Speaker")).FillWidth(0.30f)
					+ SHeaderRow::Column(TEXT("CastVoice")).DefaultLabel(LOCTEXT("CastColVoice", "Voice")).FillWidth(0.26f)
					+ SHeaderRow::Column(TEXT("Lines")).DefaultLabel(LOCTEXT("CastColLines", "Lines")).FillWidth(0.10f)
					+ SHeaderRow::Column(TEXT("Links")).DefaultLabel(LOCTEXT("CastColLinks", "Links")).FillWidth(0.10f)
					+ SHeaderRow::Column(TEXT("Sheet")).DefaultLabel(FText::GetEmpty()).FillWidth(0.24f))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 8, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					const TSharedPtr<FSpeechCastRow> Selected = SelectedSpeaker();
					if (!Selected.IsValid())
					{
						return LOCTEXT("NoSpeakerHint", "Select a speaker to see their sheet.");
					}
					if (Selected->AssetPath.IsEmpty())
					{
						return LOCTEXT("UnsheetedHint",
							"No sheet yet - create one to give this speaker an identity and a voice.");
					}
					return Selected->Description.IsEmpty()
						? LOCTEXT("NoDescription", "No casting notes on the sheet yet - Open to add some.")
						: FText::FromString(Selected->Description);
				})
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		// Right: the instruments - the project's profiles, and the provider browser to make more.
		+ SSplitter::Slot().Value(0.5f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ProfilesHeader", "Voice profiles - reusable, provider-agnostic"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().FillHeight(0.45f).Padding(8, 0, 0, 0)
			[
				SAssignNew(ProfileList, SListView<TSharedPtr<FSpeechProfileRow>>)
				.ListItemsSource(&ProfileRows)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([this](TSharedPtr<FSpeechProfileRow> Profile, const TSharedRef<STableViewBase>& Owner)
				{
					return SNew(STableRow<TSharedPtr<FSpeechProfileRow>>, Owner)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Profile->UsedBySpeakers > 0
								? FString::Printf(TEXT("%s  (%s, %d speaker%s)"),
									*Profile->Label, *Profile->ProviderId.ToString(),
									Profile->UsedBySpeakers, Profile->UsedBySpeakers == 1 ? TEXT("") : TEXT("s"))
								: FString::Printf(TEXT("%s  (%s)"),
									*Profile->Label, *Profile->ProviderId.ToString())))
							.ToolTipText(FText::FromString(Profile->ProviderVoiceId))
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.Visibility_Lambda([Profile]()
							{
								// A sample can only play once the browser has cached it. Absent
								// rather than broken when it has not been fetched yet.
								return LoadObject<USoundWave>(nullptr,
									*PreviewAssetPathForVoice(Profile->ProviderVoiceId))
									? EVisibility::Visible : EVisibility::Hidden;
							})
							.ToolTipText(LOCTEXT("ProfilePreviewTip", "Hear the cached sample. Click again to stop."))
							.OnClicked_Lambda([Profile]()
							{
								TogglePlaySoundPath(PreviewAssetPathForVoice(Profile->ProviderVoiceId));
								return FReply::Handled();
							})
							[
								MakePlayStateIcon(PreviewAssetPathForVoice(Profile->ProviderVoiceId))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("AssignProfile", "Assign"))
							.ToolTipText_Lambda([this]()
							{
								const TSharedPtr<FSpeechCastRow> Selected = SelectedSpeaker();
								return Selected.IsValid()
									? FText::FromString(FString::Printf(
										TEXT("Cast %s in this voice. Their lines go stale and say so; nothing regenerates by itself."),
										*Selected->GetLabel()))
									: LOCTEXT("AssignNoSpeaker", "Select a speaker on the left first.");
							})
							.IsEnabled_Lambda([this]() { return SelectedSpeaker().IsValid(); })
							.OnClicked(this, &SSpeechLibraryPanel::OnAssignProfile, Profile)
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("OpenProfile", "Open"))
							.ToolTipText(LOCTEXT("OpenProfileTip", "Open the profile asset - settings, model, provenance."))
							.OnClicked(this, &SSpeechLibraryPanel::OnOpenAsset, Profile->AssetPath)
						]
					];
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(8, 6)
			[
				SNew(SSeparator)
			]

			// The provider browser: fetch, audition, then either keep the voice as a profile or
			// cast it onto the selected speaker in one step.
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("BrowserHeader", "Browse provider voices"))
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					SNew(SComboBox<TSharedPtr<FName>>)
					.OptionsSource(&ProviderOptions)
					.OnGenerateWidget_Lambda([](TSharedPtr<FName> Item)
					{
						return SNew(STextBlock).Text(FText::FromName(*Item));
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FName> Item, ESelectInfo::Type)
					{
						ChosenProvider = Item;
					})
					.InitiallySelectedItem(ChosenProvider)
					[
						SNew(STextBlock).Text_Lambda([this]()
						{
							return ChosenProvider.IsValid()
								? FText::FromName(*ChosenProvider)
								: LOCTEXT("NoProvider", "No providers");
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("FetchVoices", "Fetch Voices"))
					.OnClicked(this, &SSpeechLibraryPanel::OnFetchVoices)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(this, &SSpeechLibraryPanel::CastingMessage)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return RemoteVoices.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("FetchHint",
					"Fetch Voices lists your account's voices. Preview any, then Save as Profile to keep it, or Cast to give it straight to the selected speaker."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]

			// How voices get onto the account at all - contextual, shipped by the provider plugin,
			// so it appears for providers that have such a place and for nobody else.
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 2)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
					return Provider.IsValid() && !Provider->GetVoiceLibraryUrl().IsEmpty()
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
					return Provider.IsValid() ? Provider->GetVoiceLibraryHint() : FText::GetEmpty();
				})
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(SBox).HAlign(HAlign_Left)
				[
					SNew(SHyperlink)
					.Visibility_Lambda([this]()
					{
						const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
						return Provider.IsValid() && !Provider->GetVoiceLibraryUrl().IsEmpty()
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([this]()
					{
						const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
						return Provider.IsValid()
							? FText::Format(LOCTEXT("AddVoicesLink", "Add voices on {0}"),
								FText::FromString(Provider->GetDisplayName()))
							: FText::GetEmpty();
					})
					.ToolTipText_Lambda([this]()
					{
						const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
						return Provider.IsValid()
							? FText::FromString(Provider->GetVoiceLibraryUrl()) : FText::GetEmpty();
					})
					.OnNavigate_Lambda([this]()
					{
						const TSharedPtr<ISpeechProvider> Provider = GetChosenProvider();
						if (Provider.IsValid() && !Provider->GetVoiceLibraryUrl().IsEmpty())
						{
							FPlatformProcess::LaunchURL(*Provider->GetVoiceLibraryUrl(), nullptr, nullptr);
						}
					})
				]
			]

			+ SVerticalBox::Slot().FillHeight(0.55f).Padding(8, 0, 0, 0)
			[
				SAssignNew(RemoteVoiceList, SListView<TSharedPtr<FSpeechRemoteVoice>>)
				.ListItemsSource(&RemoteVoices)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([this](TSharedPtr<FSpeechRemoteVoice> Voice, const TSharedRef<STableViewBase>& Owner)
				{
					return SNew(STableRow<TSharedPtr<FSpeechRemoteVoice>>, Owner)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Voice->bIsPremade
								? Voice->Name + TEXT("  (stock)") : Voice->Name))
							.ToolTipText(FText::FromString(FString::Printf(TEXT("%s\n%s"),
								*Voice->Id, *Voice->Description)))
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.Visibility(Voice->PreviewUrl.IsEmpty() ? EVisibility::Hidden : EVisibility::Visible)
							.ToolTipText(LOCTEXT("PreviewVoiceTip", "Hear the provider's sample of this voice. Click again to stop."))
							.OnClicked(this, &SSpeechLibraryPanel::OnPreviewVoice, Voice)
							[
								MakePlayStateIcon(PreviewAssetPathForVoice(Voice->Id))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("SaveProfile", "Save as Profile"))
							.ToolTipText(LOCTEXT("SaveProfileTip",
								"Keep this voice as a project asset - provider, preset and settings - to cast now or later."))
							.OnClicked(this, &SSpeechLibraryPanel::OnSaveProfile, Voice)
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("Cast", "Cast"))
							.ToolTipText_Lambda([this]()
							{
								const TSharedPtr<FSpeechCastRow> Selected = SelectedSpeaker();
								return Selected.IsValid()
									? FText::FromString(FString::Printf(
										TEXT("Save as a profile and cast %s in it, in one step."),
										*Selected->GetLabel()))
									: LOCTEXT("CastNoSpeaker", "Select a speaker on the left first.");
							})
							.IsEnabled_Lambda([this]() { return SelectedSpeaker().IsValid(); })
							.OnClicked(this, &SSpeechLibraryPanel::OnCastVoice, Voice)
						]
					];
				})
			]
		]
	];
}

FReply SSpeechLibraryPanel::OnAddSpeaker()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const FString Typed = NewSpeakerBox.IsValid() ? NewSpeakerBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (!Forge || Typed.IsEmpty())
	{
		return FReply::Handled();
	}

	Forge->CreateOrUpdateSpeaker(FName(*Typed), FString(), FString(), FString());
	if (NewSpeakerBox.IsValid())
	{
		NewSpeakerBox->SetText(FText::GetEmpty());
	}
	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnCreateSheet(TSharedPtr<FSpeechCastRow> Row)
{
	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		Forge->CreateOrUpdateSpeaker(Row->SpeakerId, FString(), FString(), FString());
		RefreshAll();
	}
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnAssignProfile(TSharedPtr<FSpeechProfileRow> Profile)
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const TSharedPtr<FSpeechCastRow> Selected = SelectedSpeaker();
	if (!Forge || !Selected.IsValid())
	{
		return FReply::Handled();
	}

	Forge->CreateOrUpdateSpeaker(Selected->SpeakerId, FString(), FString(), Profile->AssetPath);
	CastingMessageText = FText::FromString(FString::Printf(TEXT("%s now speaks as %s."),
		*Selected->GetLabel(), *Profile->Label));
	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnOpenAsset(FString AssetPath)
{
	if (GEditor && !AssetPath.IsEmpty())
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath))
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
		}
	}
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// The provider browser
// -------------------------------------------------------------------------------------------------

FText SSpeechLibraryPanel::CastingMessage() const
{
	return CastingMessageText;
}

TSharedPtr<ISpeechProvider> SSpeechLibraryPanel::GetChosenProvider() const
{
	USpeechForgeSubsystem* Forge = Subsystem();
	return Forge && ChosenProvider.IsValid() ? Forge->FindProvider(*ChosenProvider) : nullptr;
}

FReply SSpeechLibraryPanel::OnFetchVoices()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !ChosenProvider.IsValid())
	{
		CastingMessageText = LOCTEXT("NoProviderToFetch", "No provider to fetch from.");
		return FReply::Handled();
	}

	TSharedPtr<ISpeechProvider> Provider = Forge->FindProvider(*ChosenProvider);
	if (!Provider.IsValid())
	{
		CastingMessageText = LOCTEXT("ProviderGone", "That provider is not registered.");
		return FReply::Handled();
	}

	CastingMessageText = LOCTEXT("Fetching", "Fetching...");

	Provider->ListVoices([this](bool bSuccess, const TArray<FSpeechRemoteVoice>& Voices, const FString& Error)
	{
		if (!bSuccess)
		{
			CastingMessageText = FText::FromString(Error);
			return;
		}

		RemoteVoices.Reset();
		for (const FSpeechRemoteVoice& Voice : Voices)
		{
			RemoteVoices.Add(MakeShared<FSpeechRemoteVoice>(Voice));
		}
		CastingMessageText = FText::FromString(FString::Printf(TEXT("%d voice(s) on the account."), Voices.Num()));

		if (RemoteVoiceList.IsValid())
		{
			RemoteVoiceList->RequestListRefresh();
		}
	});

	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnPreviewVoice(TSharedPtr<FSpeechRemoteVoice> Voice)
{
	// The provider's sample, cached as a small asset the first time and auditioned like any sound.
	const FString AssetPath = PreviewAssetPathForVoice(Voice->Id);
	FString AssetName;
	AssetPath.Split(TEXT("."), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

	if (IsSoundPathPlaying(AssetPath))
	{
		StopPreview();
		return FReply::Handled();
	}

	if (USoundWave* Cached = LoadObject<USoundWave>(nullptr, *AssetPath))
	{
		TogglePlaySoundPath(AssetPath);
		return FReply::Handled();
	}

	CastingMessageText = LOCTEXT("FetchingPreview", "Fetching the sample...");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(Voice->PreviewUrl);
	Http->SetVerb(TEXT("GET"));
	Http->OnProcessRequestComplete().BindLambda(
		[this, AssetName, AssetPath](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				CastingMessageText = LOCTEXT("PreviewFailed", "The sample could not be fetched.");
				return;
			}

			const FString TempFile = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("VoicePreview"), TEXT(".mp3"));
			if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *TempFile))
			{
				CastingMessageText = LOCTEXT("PreviewWriteFailed", "The sample could not be written.");
				return;
			}

			FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = TempFile;
			Task->DestinationPath = TEXT("/Game/_Generated/Speech/Previews");
			Task->DestinationName = AssetName;
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			Task->bSave = true;

			USoundFactory* Factory = NewObject<USoundFactory>();
			Factory->bAutoCreateCue = false;
			Task->Factory = Factory;

			AssetTools.Get().ImportAssetTasks({ Task });
			IFileManager::Get().Delete(*TempFile);

			CastingMessageText = FText::GetEmpty();
			TogglePlaySoundPath(AssetPath);
		});
	Http->ProcessRequest();

	return FReply::Handled();
}

FString SSpeechLibraryPanel::EnsureProfileForRemoteVoice(TSharedPtr<FSpeechRemoteVoice> Voice)
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge)
	{
		return FString();
	}

	const FName ProviderId = ChosenProvider.IsValid() ? *ChosenProvider : NAME_None;

	// One profile per provider preset. Saving the same voice twice must not litter the project.
	const FString Existing = Forge->FindVoiceProfileByProviderVoice(ProviderId, Voice->Id);
	if (!Existing.IsEmpty())
	{
		return Existing;
	}

	return Forge->CreateVoiceProfile(
		FString(), Voice->Name, Voice->Id, Voice->Name, ProviderId, FString(),
		Voice->bIsPremade ? ESpeechVoiceProvenance::Premade : ESpeechVoiceProvenance::Unknown);
}

FReply SSpeechLibraryPanel::OnSaveProfile(TSharedPtr<FSpeechRemoteVoice> Voice)
{
	const FString Path = EnsureProfileForRemoteVoice(Voice);
	CastingMessageText = Path.IsEmpty()
		? LOCTEXT("ProfileFailed", "Could not create the profile.")
		: FText::FromString(FString::Printf(TEXT("%s kept as %s."),
			*Voice->Name, *FPackageName::GetShortName(Path)));
	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnCastVoice(TSharedPtr<FSpeechRemoteVoice> Voice)
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const TSharedPtr<FSpeechCastRow> Selected = SelectedSpeaker();
	if (!Forge || !Selected.IsValid())
	{
		CastingMessageText = LOCTEXT("NoSpeakerSelected", "Select the speaker to cast first, on the left.");
		return FReply::Handled();
	}

	const FString ProfilePath = EnsureProfileForRemoteVoice(Voice);
	if (ProfilePath.IsEmpty())
	{
		CastingMessageText = LOCTEXT("ProfileFailed", "Could not create the profile.");
		return FReply::Handled();
	}

	Forge->CreateOrUpdateSpeaker(Selected->SpeakerId, FString(), FString(), ProfilePath);
	CastingMessageText = FText::FromString(FString::Printf(TEXT("%s cast as %s."),
		*Selected->GetLabel(), *Voice->Name));
	RefreshAll();
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Produce page
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SSpeechLibraryPanel::MakeProducePage()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 4)
		[
			SAssignNew(ProduceList, SListView<TSharedPtr<FSpeechLibraryRow>>)
			.ListItemsSource(&Rows)
			.OnGenerateRow(this, &SSpeechLibraryPanel::MakeProduceRow)
			.SelectionMode(ESelectionMode::Multi)
			.HeaderRow(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColumnLine).DefaultLabel(LOCTEXT("ColLine2", "Line")).FillWidth(0.12f)
				+ SHeaderRow::Column(ColumnSpeaker).DefaultLabel(LOCTEXT("ColSpeaker2", "Speaker")).FillWidth(0.10f)
				+ SHeaderRow::Column(ColumnText).DefaultLabel(LOCTEXT("ColText2", "Text")).FillWidth(0.36f)
				+ SHeaderRow::Column(ColumnVoice).DefaultLabel(LOCTEXT("ColVoice2", "Voice")).FillWidth(0.12f)
				+ SHeaderRow::Column(ColumnStatus).DefaultLabel(LOCTEXT("ColStatus2", "Status")).FillWidth(0.12f)
				+ SHeaderRow::Column(ColumnOrigin).DefaultLabel(LOCTEXT("ColOrigin2", "Origin")).FillWidth(0.09f)
				+ SHeaderRow::Column(ColumnDuration).DefaultLabel(LOCTEXT("ColDuration2", "Length")).FillWidth(0.07f)
				+ SHeaderRow::Column(ColumnPlay).DefaultLabel(FText::GetEmpty()).FixedWidth(28.f))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 8)
		[
			MakeActionBar()
		];
}

namespace
{
	TSharedRef<SWidget> MakeStringCombo(TArray<TSharedPtr<FString>>* Options, TSharedPtr<FString>* Chosen)
	{
		return SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(Options)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock).Text(FText::FromString(*Item));
			})
			.OnSelectionChanged_Lambda([Chosen](TSharedPtr<FString> Item, ESelectInfo::Type)
			{
				*Chosen = Item;
			})
			.InitiallySelectedItem(*Chosen)
			[
				SNew(STextBlock).Text_Lambda([Chosen]()
				{
					return Chosen->IsValid()
						? FText::FromString(**Chosen)
						: NSLOCTEXT("SSpeechLibraryPanel", "None", "-");
				})
			];
	}
}

TSharedRef<SWidget> SSpeechLibraryPanel::MakeActionBar()
{
	// The discovered buttons: whatever installed plugins tagged for this panel. Built in a loop
	// because the list is data, which is the whole point.
	TSharedRef<SHorizontalBox> ActionButtons = SNew(SHorizontalBox);
	for (const TSharedPtr<FSpeechLibraryAction>& Action : DiscoveredActions)
	{
		// An action that named another page lives there instead - Re-harvest sits on Ingest,
		// beside the source it re-reads.
		if (!Action->Page.IsEmpty())
		{
			continue;
		}

		ActionButtons->AddSlot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ToolTipText(FText::FromString(Action->Tooltip))
			.OnClicked(this, &SSpeechLibraryPanel::OnActionClicked, Action)
			[
				SNew(STextBlock).Text(FText::FromString(Action->Label))
			]
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[
					SNew(STextBlock)
					.Text(this, &SSpeechLibraryPanel::MessageText)
					.AutoWrapText(true)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[
					SNew(STextBlock).Text(this, &SSpeechLibraryPanel::EstimateText)
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.IsEnabled(this, &SSpeechLibraryPanel::CanGenerate)
					.OnClicked(this, &SSpeechLibraryPanel::OnGenerateClicked)
					[
						SNew(STextBlock).Text(LOCTEXT("Generate", "Generate Selected"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("GenerateAllTip",
						"Generate every line in this bank that is missing audio or stale. Current "
						"lines cost nothing and are skipped; recorded lines are never touched. "
						"Safe to press twice - the second press finds nothing to do."))
					.IsEnabled_Lambda([this]() { return ChosenBank.IsValid() && Rows.Num() > 0; })
					.OnClicked(this, &SSpeechLibraryPanel::OnGenerateAllClicked)
					[
						SNew(STextBlock).Text(LOCTEXT("GenerateAll", "Generate All"))
					]
				]

				// Dubbing sits beside generation because it is the other answer to the same
				// question - how this localised line gets its audio - and the choice is per line,
				// never a mode. On a source-language bank there is nothing to dub from, so the
				// button is not there at all rather than there and refusing.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("DubTip",
						"Carry the source language's recording into this one: the actor's own voice, "
						"their pacing, the new words. Keeps a captured face usable underneath, because "
						"the timing survives. Costs by the minute of source audio, well above synthesis, "
						"and takes about twenty seconds a line."))
					.Visibility(this, &SSpeechLibraryPanel::DubVisibility)
					.IsEnabled(this, &SSpeechLibraryPanel::CanDub)
					.OnClicked(this, &SSpeechLibraryPanel::OnDubClicked)
					[
						SNew(STextBlock).Text(LOCTEXT("Dub", "Dub from Source"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("RegenerateTip",
						"Force: re-generate the selected lines whether they are current, stale or "
						"missing, after a confirmation with the cost. The one thing force never "
						"does is overwrite a recorded performance - those lines are skipped."))
					.IsEnabled(this, &SSpeechLibraryPanel::CanGenerate)
					.OnClicked(this, &SSpeechLibraryPanel::OnRegenerateClicked)
					[
						SNew(STextBlock).Text(LOCTEXT("Regenerate", "Re-generate Selected"))
					]
				]
			]

			// The discovered buttons that stayed here - Perform-page actions moved out with their
			// fixtures, which now live where the sessions and face banks do.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SSpacer)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					ActionButtons
				]
			]
		];
}

FText SSpeechLibraryPanel::EstimateText() const
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const TArray<FSpeechLineHandle> Handles = SelectedHandles();
	if (!Forge || Handles.Num() == 0)
	{
		return FText::GetEmpty();
	}

	const FSpeechCostEstimate Estimate = Forge->EstimateGenerationCost(Handles, /*bForce=*/false);
	const TCHAR* Currency = Estimate.Currency.IsEmpty() ? TEXT("USD") : *Estimate.Currency;

	FString Text = Estimate.LineCount > 0
		? FString::Printf(TEXT("%d to generate (%d current), ~%.3f %s"),
			Estimate.LineCount, Estimate.SkippedCount, Estimate.EstimatedCost, Currency)
		: FString::Printf(TEXT("%d selected, all current - nothing to spend"), Estimate.SkippedCount);

	// Converting is a second meter, charged later and by the second. Leaving it out of this row
	// would understate what pressing Record commits to, which on a paid pipeline is the one number
	// that must not be a surprise.
	if (ChosenTreatment.IsValid() && ChosenTreatment->StartsWith(TEXT("Convert")))
	{
		const FSpeechCostEstimate Conversion = Forge->EstimateConversionCost(Handles, FString());

		if (Conversion.BilledSeconds > 0.f)
		{
			Text += Conversion.EstimatedCost > 0.f
				? FString::Printf(TEXT("  +  re-voicing %.1fs, ~%.3f %s"),
					Conversion.BilledSeconds, Conversion.EstimatedCost, Currency)
				: FString::Printf(TEXT("  +  re-voicing %.1fs (set a per-minute rate to price it)"),
					Conversion.BilledSeconds);
		}

		if (Conversion.UnpricedCount > 0)
		{
			// Not a failure. A conversion is billed for audio that exists, and these takes have not
			// been recorded yet - so the only honest thing to report is that the number arrives with
			// the performance.
			Text += FString::Printf(
				TEXT("  +  re-voicing %d not yet priced - billed per second of what you record"),
				Conversion.UnpricedCount);
		}
	}

	return FText::FromString(Text);
}

FText SSpeechLibraryPanel::MessageText() const
{
	if (!LastMessage.IsEmpty())
	{
		return LastMessage;
	}

	return Rows.Num() == 0
		? LOCTEXT("NoLines", "No lines. Pick a bank, or author one - NP_VoiceOver harvests dialogue into banks.")
		: FText::GetEmpty();
}

bool SSpeechLibraryPanel::CanGenerate() const
{
	return SelectedHandles().Num() > 0;
}

FReply SSpeechLibraryPanel::OnGenerateClicked()
{
	if (USpeechForgeSubsystem* Forge = Subsystem())
	{
		const FString Result = Forge->GenerateLines(SelectedHandles(), /*bForce=*/false);
		LastMessage = FText::FromString(Result);
		RefreshAll();
	}
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnGenerateAllClicked()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !ChosenBank.IsValid())
	{
		return FReply::Handled();
	}

	// The whole bank, no selection required. GenerateLines already skips current, busy and
	// non-pipeline-owned lines, which is the idempotence: a second press finds nothing to do.
	const TArray<FSpeechLineHandle> BankHandles { FSpeechLineHandle(*ChosenBank, NAME_None) };

	const FSpeechCostEstimate Estimate = Forge->EstimateGenerationCost(BankHandles, /*bForce=*/false);
	if (Estimate.LineCount == 0)
	{
		LastMessage = LOCTEXT("GenerateAllNothing", "Everything is current - nothing to generate.");
		return FReply::Handled();
	}

	const FString Currency = Estimate.Currency.IsEmpty() ? TEXT("USD") : Estimate.Currency;
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::FromString(FString::Printf(
			TEXT("Generate %d line(s) that are missing audio or stale, ~%.3f %s?\n\n")
			TEXT("%d current line(s) are skipped and cost nothing. Recorded and hand-edited ")
			TEXT("lines are never touched."),
			Estimate.LineCount, Estimate.EstimatedCost, *Currency, Estimate.SkippedCount)));

	if (Choice != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	LastMessage = FText::FromString(Forge->GenerateLines(BankHandles, /*bForce=*/false));
	RefreshAll();
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnRegenerateClicked()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	const TArray<FSpeechLineHandle> Handles = SelectedHandles();
	if (!Forge || Handles.Num() == 0)
	{
		return FReply::Handled();
	}

	// Counts for an honest warning: what force re-pays for, and what it still will not touch.
	// Force means "I know it looks current" - it never means "throw away a performance", so
	// anything the pipeline no longer owns is skipped by GenerateLines and said so here.
	int32 CurrentCount = 0;
	int32 ProtectedCount = 0;
	if (ProduceList.IsValid())
	{
		for (const TSharedPtr<FSpeechLibraryRow>& Row : ProduceList->GetSelectedItems())
		{
			if (!Row.IsValid())
			{
				continue;
			}

			if (Row->Status.Origin != ESpeechLineOrigin::Generated)
			{
				++ProtectedCount;
			}
			else if (Row->Status.Status == ESpeechLineStatus::Generated && !Row->Status.bStale)
			{
				++CurrentCount;
			}
		}
	}

	const FSpeechCostEstimate Estimate = Forge->EstimateGenerationCost(Handles, /*bForce=*/true);
	const FString Currency = Estimate.Currency.IsEmpty() ? TEXT("USD") : Estimate.Currency;

	FString Ask = FString::Printf(TEXT("Force re-generate %d line(s), ~%.3f %s?"),
		Estimate.LineCount, Estimate.EstimatedCost, *Currency);

	if (CurrentCount > 0)
	{
		Ask += FString::Printf(
			TEXT("\n\n%d of them are current - this re-pays for audio that did not need it."),
			CurrentCount);
	}

	if (ProtectedCount > 0)
	{
		Ask += FString::Printf(
			TEXT("\n\n%d recorded, edited or accepted line(s) stay untouched - the pipeline never ")
			TEXT("overwrites audio it no longer owns. Re-record or run a pickup session for those."),
			ProtectedCount);
	}

	const EAppReturnType::Type Choice =
		FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Ask));

	if (Choice != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	LastMessage = FText::FromString(Forge->GenerateLines(Handles, /*bForce=*/true));
	RefreshAll();
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Dubbing
//
// The other way a localised line gets its audio. Generation re-reads the translated text in the cast
// voice; a dub carries the source language's *performance* across - the actor's voice, their pacing,
// their breaths - which is what keeps a captured face usable underneath, because the timing survives.
//
// So it is a per-line choice, not a mode: in a real localised bank most lines are generated and the
// performed few are dubbed. That is why the button sits beside Generate rather than on the Localize
// page, which is about banks and languages.
// -------------------------------------------------------------------------------------------------

EVisibility SSpeechLibraryPanel::DubVisibility() const
{
	return bBankIsLocalized ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SSpeechLibraryPanel::CanDub() const
{
	if (DubsRunning > 0 || !ProduceList.IsValid())
	{
		return false;
	}

	for (const TSharedPtr<FSpeechLibraryRow>& Row : ProduceList->GetSelectedItems())
	{
		if (Row.IsValid() && Row->bSourceRecorded)
		{
			return true;
		}
	}

	return false;
}

FReply SSpeechLibraryPanel::OnDubClicked()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !ProduceList.IsValid())
	{
		return FReply::Handled();
	}

	// Only the lines that have something to dub. A generated source line in the selection is left
	// alone rather than refused: selecting a range and pressing the button should do the sensible
	// thing to each row, and say what it skipped.
	TArray<FSpeechLineHandle> Handles;
	int32 SkippedNoRecording = 0;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : ProduceList->GetSelectedItems())
	{
		if (!Row.IsValid())
		{
			continue;
		}

		if (Row->bSourceRecorded)
		{
			Handles.Add(Row->Status.Handle);
		}
		else
		{
			++SkippedNoRecording;
		}
	}

	if (Handles.Num() == 0)
	{
		LastMessage = LOCTEXT("DubNothing",
			"None of the selected lines has a recording behind it in the source language. "
			"Generate those instead - it is cheaper and says the right words.");
		return FReply::Handled();
	}

	// No figure is offered, deliberately. Dubbing bills by the minute of source audio at a rate
	// this panel has never measured, and a made-up number about money is worse than none.
	FString Ask = FString::Printf(
		TEXT("Dub %d line(s) from the source language?\n\n")
		TEXT("Each one sends its source recording to be re-spoken in this language, keeping the ")
		TEXT("actor's voice and pacing. This bills dubbing minutes - well above synthesis - and ")
		TEXT("takes about twenty seconds a line."),
		Handles.Num());

	if (SkippedNoRecording > 0)
	{
		Ask += FString::Printf(
			TEXT("\n\n%d selected line(s) have a generated source and are left alone. Generate those."),
			SkippedNoRecording);
	}

	if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Ask)) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	// One at a time. The service renders a whole clip per request and running the selection in
	// parallel would spend the whole bill before the first failure could stop it.
	DubsRunning = Handles.Num();
	LastMessage = FText::Format(
		LOCTEXT("DubStarted", "Dubbing {0} line(s) - about twenty seconds each."), Handles.Num());

	TSharedRef<TArray<FString>> Results = MakeShared<TArray<FString>>();
	DubNext(Handles, 0, Results);

	return FReply::Handled();
}

void SSpeechLibraryPanel::DubNext(
	TArray<FSpeechLineHandle> Handles, int32 Index, TSharedRef<TArray<FString>> Results)
{
	USpeechForgeSubsystem* Forge = Subsystem();

	if (!Forge || Index >= Handles.Num())
	{
		DubsRunning = 0;

		int32 Failed = 0;
		for (const FString& Result : *Results)
		{
			Failed += Result.IsEmpty() ? 0 : 1;
		}

		LastMessage = Failed == 0
			? FText::Format(LOCTEXT("DubDone",
				"Dubbed {0} line(s). They are Recorded now, and their words are unverified against "
				"the translation - listen once. Re-solve their faces to move the mouths."),
				Handles.Num())
			: FText::Format(LOCTEXT("DubPartly", "Dubbed {0} of {1}. {2}"),
				Handles.Num() - Failed, Handles.Num(), FText::FromString((*Results)[0]));

		RefreshAll();
		return;
	}

	// Weak, because a dub outlives a closed panel - the editor must not be holding a dead widget
	// when the download lands.
	TWeakPtr<SSpeechLibraryPanel> WeakSelf = SharedThis(this);

	Forge->DubLineAudio(Handles[Index],
		[WeakSelf, Handles, Index, Results](bool bSuccess, const FString& Message)
	{
		if (!bSuccess)
		{
			Results->Add(Message);
		}

		if (const TSharedPtr<SSpeechLibraryPanel> Self = WeakSelf.Pin())
		{
			Self->LastMessage = FText::Format(
				NSLOCTEXT("SSpeechLibraryPanel", "DubProgress", "Dubbing {0} of {1}..."),
				Index + 1, Handles.Num());
			Self->DubNext(Handles, Index + 1, Results);
		}
	});
}

// -------------------------------------------------------------------------------------------------
// Session fixtures
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::RefreshFaceBanks()
{
	FaceBankOptions.Reset();
	FaceBankOptions.Add(MakeShared<FString>(TEXT("-")));

	// Face banks found by class path through the registry, so this panel needs no FaceForge type.
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssetsByClass(
		FTopLevelAssetPath(TEXT("/Script/FaceForge"), TEXT("FaceBank")), Assets, true);

	// The bank that claims the open speech bank, read off the registry tag rather than by loading
	// anything. Still no FaceForge type here - a tag is a string.
	FString LinkedFaceBank;
	const FString OpenBank = ChosenBank.IsValid() ? *ChosenBank : FString();

	for (const FAssetData& Data : Assets)
	{
		FaceBankOptions.Add(MakeShared<FString>(Data.GetSoftObjectPath().ToString()));

		FString Claimed;
		if (OpenBank.IsEmpty() || LinkedFaceBank.Len() > 0 ||
			!Data.GetTagValue(TEXT("SourceSpeechBankPath"), Claimed) || Claimed.IsEmpty())
		{
			continue;
		}

		// Package paths compared by EQUALITY, never by substring. "/Game/X/SB_A" is contained in
		// "/Game/X/SB_A_ES", so a substring match let a localised sibling answer for its source
		// bank and the source answer for the sibling - and this selection is a recording session's
		// target, so the wrong answer costs a take rather than a click.
		FString ClaimedPackage = Claimed;
		ClaimedPackage.Split(TEXT("."), &ClaimedPackage, nullptr);
		FString OpenPackage = OpenBank;
		OpenPackage.Split(TEXT("."), &OpenPackage, nullptr);

		if (ClaimedPackage == OpenPackage)
		{
			LinkedFaceBank = Data.GetSoftObjectPath().ToString();
		}
	}

	// The linked bank, or none. Never "the first one in the project", which is what this used to do
	// and how a session got recorded into a bank belonging to another scene - the kind of mistake
	// that costs a take rather than a click, and is likeliest exactly when somebody is working fast.
	ChosenFaceBank = FaceBankOptions[0];
	if (!LinkedFaceBank.IsEmpty())
	{
		for (const TSharedPtr<FString>& Option : FaceBankOptions)
		{
			if (Option.IsValid() && *Option == LinkedFaceBank)
			{
				ChosenFaceBank = Option;
				break;
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------
// Discovered actions - buttons from tool metadata, never written here
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::DiscoverActions()
{
	DiscoveredActions.Reset();
	SyncActions.Reset();
	IngestActions.Reset();

	// The toolset base class found by path, so this panel needs no registry dependency. Every
	// derived class is scanned for functions tagged SpeechLibraryAction - installing a plugin adds
	// buttons with no edit here, and a third party gets the same seam we use.
	UClass* ToolsetBase = FindObject<UClass>(nullptr, TEXT("/Script/ToolsetRegistry.ToolsetDefinition"));
	if (!ToolsetBase)
	{
		return;
	}

	TArray<UClass*> ToolsetClasses;
	GetDerivedClasses(ToolsetBase, ToolsetClasses, /*bRecursive=*/true);

	for (UClass* ToolsetClass : ToolsetClasses)
	{
		for (TFieldIterator<UFunction> It(ToolsetClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FString Label = It->GetMetaData(TEXT("SpeechLibraryAction"));
			if (!Label.IsEmpty())
			{
				TSharedRef<FSpeechLibraryAction> Action = MakeShared<FSpeechLibraryAction>();
				Action->Label = Label;
				Action->Tooltip = It->GetMetaData(TEXT("ToolTip"));
				Action->ToolClass = ToolsetClass;
				Action->FunctionName = It->GetFName();
				Action->Page = It->GetMetaData(TEXT("SpeechLibraryPage"));
				Action->Group = It->GetMetaData(TEXT("SpeechLibraryGroup"));
				Action->bPerRig = !It->GetMetaData(TEXT("SpeechLibraryPerRig")).IsEmpty();
				DiscoveredActions.Add(Action);
			}

			// The two-way-sync contract's other half: a write-back the line editor offers on banks
			// whose source stamp matches the adapter key. See FSpeechLineSyncAction.
			const FString SyncLabel = It->GetMetaData(TEXT("SpeechLineSync"));
			if (!SyncLabel.IsEmpty())
			{
				TSharedRef<FSpeechLineSyncAction> Sync = MakeShared<FSpeechLineSyncAction>();
				Sync->Label = SyncLabel;
				Sync->Tooltip = It->GetMetaData(TEXT("ToolTip"));
				Sync->AdapterKey = FName(*It->GetMetaData(TEXT("SpeechLineSyncAdapter")));
				Sync->ToolClass = ToolsetClass;
				Sync->FunctionName = It->GetFName();
				SyncActions.Add(Sync);
			}

			// An ingestion method: pulls lines into a bank from some other asset. The Ingest page
			// exists exactly when at least one of these is found. See FSpeechIngestAction.
			const FString IngestLabel = It->GetMetaData(TEXT("SpeechIngest"));
			if (!IngestLabel.IsEmpty())
			{
				TSharedRef<FSpeechIngestAction> Ingest = MakeShared<FSpeechIngestAction>();
				Ingest->Label = IngestLabel;
				Ingest->Tooltip = It->GetMetaData(TEXT("ToolTip"));
				Ingest->AssetClassPath = It->GetMetaData(TEXT("SpeechIngestClass"));
				Ingest->AdapterKey = FName(*It->GetMetaData(TEXT("SpeechIngestAdapter")));
				Ingest->ToolClass = ToolsetClass;
				Ingest->FunctionName = It->GetFName();
				IngestActions.Add(Ingest);
			}
		}
	}

	IngestActions.Sort([](const TSharedPtr<FSpeechIngestAction>& A, const TSharedPtr<FSpeechIngestAction>& B)
	{
		return A->Label < B->Label;
	});

	DiscoveredActions.Sort([](const TSharedPtr<FSpeechLibraryAction>& A, const TSharedPtr<FSpeechLibraryAction>& B)
	{
		return A->Label < B->Label;
	});
}

FString SSpeechLibraryPanel::BuildSelectionContextJson() const
{
	// The selection comes from whichever page the action was pressed on - Perform's list when
	// Perform is up, Produce's otherwise. Same rows either way; different tables select them.
	const TSharedPtr<SListView<TSharedPtr<FSpeechLibraryRow>>>& SelectionList =
		ActivePage == PagePerform && PerformList.IsValid() ? PerformList : ProduceList;

	if (!SelectionList.IsValid())
	{
		return FString();
	}

	return BuildContextJsonForRows(SelectionList->GetSelectedItems(), TMap<FString, FString>());
}

FString SSpeechLibraryPanel::BuildContextJsonForRows(
	const TArray<TSharedPtr<FSpeechLibraryRow>>& ForRows,
	const TMap<FString, FString>& ExtraFields) const
{
	USpeechForgeSubsystem* Forge = Subsystem();
	UObject* Asset = ChosenBank.IsValid() ? LoadObject<UObject>(nullptr, **ChosenBank) : nullptr;
	ISpeechLineSource* Source = Asset ? Cast<ISpeechLineSource>(Asset) : nullptr;

	if (!Forge || !Source)
	{
		return FString();
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	// Whatever an action's own contract asks to ride along - per-rig fields, and whatever the
	// next contract needs. Set first so the standard fields always win a name clash.
	for (const TPair<FString, FString>& Extra : ExtraFields)
	{
		Root->SetStringField(Extra.Key, Extra.Value);
	}

	const FString Mode = ChosenMode.IsValid() && *ChosenMode == TEXT("Voice") ? TEXT("voice")
		: (ChosenMode.IsValid() && *ChosenMode == TEXT("Face only") ? TEXT("face_only") : TEXT("voice_and_video"));
	Root->SetStringField(TEXT("bankPath"), *ChosenBank);
	Root->SetStringField(TEXT("mode"), Mode);
	Root->SetStringField(TEXT("treatment"),
		ChosenTreatment.IsValid() && *ChosenTreatment == TEXT("Use performance as-is") ? TEXT("as_is") : TEXT("convert"));

	if (ChosenFaceBank.IsValid() && *ChosenFaceBank != TEXT("-"))
	{
		Root->SetStringField(TEXT("faceBankPath"), *ChosenFaceBank);
	}

	TArray<TSharedPtr<FJsonValue>> Lines;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : ForRows)
	{
		if (!Row.IsValid())
		{
			continue;
		}

		const FSpeechLineHandle& Handle = Row->Status.Handle;
		const FSpeechLine* Line = Source->FindLine(Handle.LineId);
		if (!Line)
		{
			continue;
		}

		TSharedRef<FJsonObject> LineObject = MakeShared<FJsonObject>();
		LineObject->SetStringField(TEXT("assetPath"), Handle.AssetPath);
		LineObject->SetStringField(TEXT("lineId"), Handle.LineId.ToString());
		LineObject->SetStringField(TEXT("text"), Line->Text);
		LineObject->SetStringField(TEXT("direction"), Line->Direction);
		LineObject->SetStringField(TEXT("speaker"), Row->Status.SpeakerId.ToString());
		LineObject->SetStringField(TEXT("voiceId"), Row->Status.ResolvedVoice.ProviderVoiceId);
		LineObject->SetStringField(TEXT("voiceLabel"), Row->VoiceLabel);
		LineObject->SetStringField(TEXT("soundPath"), Row->Status.SoundPath);

		// The line's OWN face bank - the one that holds its clip - so a session spanning rigs
		// solves each take into the right bank without anyone picking anything.
		if (const FString* LineFaceBank = LineFaceBankPaths.Find(Handle.LineId))
		{
			LineObject->SetStringField(TEXT("faceBankPath"), *LineFaceBank);
		}

		Lines.Add(MakeShared<FJsonValueObject>(LineObject));
	}
	Root->SetArrayField(TEXT("lines"), Lines);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

FReply SSpeechLibraryPanel::OnActionClicked(TSharedPtr<FSpeechLibraryAction> Action)
{
	// An action reports beside the buttons it belongs to: the Ingest page's line, the Perform
	// page's line, or Produce's for the unpaged rest.
	FText& Message = Action->Page == TEXT("Ingest") ? IngestMessage
		: Action->Page == TEXT("Perform") ? PerformMessage
		: LastMessage;

	// A per-rig action does not take the selection as one lump - it slices by rig first.
	if (Action->bPerRig)
	{
		return OnPerRigActionClicked(Action);
	}

	const FString Context = BuildSelectionContextJson();
	if (Context.IsEmpty())
	{
		Message = LOCTEXT("NoContext", "Pick a bank and select lines first.");
		return FReply::Handled();
	}

	Message = FText::FromString(InvokeActionFunction(Action, Context));
	RefreshAll();
	return FReply::Handled();
}

FString SSpeechLibraryPanel::InvokeActionFunction(
	const TSharedPtr<FSpeechLibraryAction>& Action, const FString& ContextJson)
{
	UClass* ToolClass = Action->ToolClass.Get();
	UFunction* Function = ToolClass ? ToolClass->FindFunctionByName(Action->FunctionName) : nullptr;
	if (!Function)
	{
		return TEXT("That action's plugin is no longer loaded.");
	}

	// One FString in, one FString (or async result) out - the convention every tagged tool keeps.
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(Function->ParmsSize));
	FMemory::Memzero(Frame, Function->ParmsSize);

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(Frame);
		}
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) &&
			!It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				AsString->SetPropertyValue_InContainer(Frame, ContextJson);
				break;
			}
		}
	}

	ToolClass->GetDefaultObject()->ProcessEvent(Function, Frame);

	FString ResultText = FString::Printf(TEXT("%s started."), *Action->Label);
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				ResultText = AsString->GetPropertyValue_InContainer(Frame);
			}
		}
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->DestroyValue_InContainer(Frame);
		}
	}

	return ResultText;
}

namespace SpeechPerformPrivate
{
	/** "/Game/X/SB_A.SB_A" and "/Game/X/SB_A" both become "/Game/X/SB_A". */
	FString ToPackagePath(const FString& Path)
	{
		FString Out = Path;
		Out.Split(TEXT("."), &Out, nullptr);
		return Out;
	}

	/**
	 * Whether a stored reference names exactly this bank. Compared as package paths and by
	 * equality, never by substring - "SB_A_DE" contains "SB_A", and a localized sibling claiming
	 * its source bank's face banks was the bug this replaces.
	 */
	bool RefersToBank(const FString& StoredPath, const FString& BankPackage)
	{
		return ToPackagePath(StoredPath) == BankPackage;
	}

	/** A string-ish property off a struct instance, whatever its actual type. */
	FString ReadStructString(const UScriptStruct* Struct, const void* Instance, FName PropName)
	{
		const FProperty* Prop = Struct ? Struct->FindPropertyByName(PropName) : nullptr;
		if (const FStrProperty* AsString = CastField<FStrProperty>(Prop))
		{
			return AsString->GetPropertyValue_InContainer(Instance);
		}
		if (const FNameProperty* AsName = CastField<FNameProperty>(Prop))
		{
			return AsName->GetPropertyValue_InContainer(Instance).ToString();
		}
		return FString();
	}

	/** An enum property's display name off a struct instance, e.g. a face clip's status. */
	FString ReadStructEnum(const UScriptStruct* Struct, const void* Instance, FName PropName)
	{
		const FProperty* Prop = Struct ? Struct->FindPropertyByName(PropName) : nullptr;
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Prop))
		{
			const int64 Value = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(
				AsEnum->ContainerPtrToValuePtr<void>(Instance));
			return AsEnum->GetEnum()->GetDisplayNameTextByValue(Value).ToString();
		}
		if (const FByteProperty* AsByte = CastField<FByteProperty>(Prop))
		{
			const uint8 Value = AsByte->GetPropertyValue_InContainer(Instance);
			return AsByte->Enum
				? AsByte->Enum->GetDisplayNameTextByValue(Value).ToString()
				: FString::FromInt(Value);
		}
		return FString();
	}

	/**
	 * Call a toolset function that takes strings and returns one, filling its string parameters
	 * in declaration order. How this panel opens foreign panels - a face bank, a recording
	 * session, a clip within one - without linking the plugin that owns them: found by class
	 * path, gone means a message, never an error.
	 */
	FString InvokeStringTool(const TCHAR* ClassPath, const TCHAR* FunctionName,
		const FString& Argument, const FString& SecondArgument = FString())
	{
		UClass* ToolClass = FindObject<UClass>(nullptr, ClassPath);
		UFunction* Function = ToolClass ? ToolClass->FindFunctionByName(FunctionName) : nullptr;
		if (!Function)
		{
			return TEXT("That plugin is not loaded.");
		}

		uint8* Frame = static_cast<uint8*>(FMemory_Alloca(Function->ParmsSize));
		FMemory::Memzero(Frame, Function->ParmsSize);

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm))
			{
				It->InitializeValue_InContainer(Frame);
			}
		}

		int32 StringsFilled = 0;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) &&
				!It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
				{
					AsString->SetPropertyValue_InContainer(
						Frame, StringsFilled == 0 ? Argument : SecondArgument);

					if (++StringsFilled == 2)
					{
						break;
					}
				}
			}
		}

		ToolClass->GetDefaultObject()->ProcessEvent(Function, Frame);

		FString Result;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
				{
					Result = AsString->GetPropertyValue_InContainer(Frame);
				}
			}
		}

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm))
			{
				It->DestroyValue_InContainer(Frame);
			}
		}

		return Result;
	}
}

FReply SSpeechLibraryPanel::OnPerRigActionClicked(TSharedPtr<FSpeechLibraryAction> Action)
{
	// The chosen lines, or the whole bank when nothing is chosen - "give my lines faces" should
	// not require a select-all first.
	TArray<TSharedPtr<FSpeechLibraryRow>> ForRows =
		PerformList.IsValid() ? PerformList->GetSelectedItems() : TArray<TSharedPtr<FSpeechLibraryRow>>();
	if (ForRows.Num() == 0)
	{
		ForRows = Rows;
	}

	return RunPerRigAction(Action, MoveTemp(ForRows), FString());
}

FReply SSpeechLibraryPanel::RunPerRigAction(
	TSharedPtr<FSpeechLibraryAction> Action,
	TArray<TSharedPtr<FSpeechLibraryRow>> ForRows,
	const FString& ExplicitFaceBankPath)
{
	if (!Action.IsValid())
	{
		PerformMessage = LOCTEXT("PerRigGone", "That action's plugin is no longer loaded.");
		return FReply::Handled();
	}

	if (!ChosenBank.IsValid())
	{
		PerformMessage = LOCTEXT("PerRigNoBank", "Pick a bank first.");
		return FReply::Handled();
	}

	if (ForRows.Num() == 0)
	{
		PerformMessage = LOCTEXT("PerRigNoLines", "Nothing to assign - every line has a face clip.");
		return FReply::Handled();
	}

	// An explicit target skips the grouping entirely: the person picked the bank, and a picker
	// that second-guesses is not a picker.
	if (!ExplicitFaceBankPath.IsEmpty())
	{
		TMap<FString, FString> Extra;
		Extra.Add(TEXT("explicitFaceBankPath"), ExplicitFaceBankPath);

		const FString Json = BuildContextJsonForRows(ForRows, Extra);
		if (!Json.IsEmpty())
		{
			PerformMessage = FText::FromString(InvokeActionFunction(Action, Json));
			RefreshAll();
		}
		return FReply::Handled();
	}

	// Slice by the speakers' rigs. Speakers with no rig ride the default slice: their voice works
	// regardless, and the Rig badge upstream already says what is missing.
	TMap<FString, TArray<TSharedPtr<FSpeechLibraryRow>>> Groups;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : ForRows)
	{
		if (!Row.IsValid())
		{
			continue;
		}

		const FString* Rig = SpeakerRigPaths.Find(Row->Status.SpeakerId);
		Groups.FindOrAdd(Rig ? *Rig : FString()).Add(Row);
	}

	// Which rig owns the DEFAULT, unsuffixed bank: the MetaHuman archetype when present - the
	// family's home rig - else whichever slice is biggest.
	FString DefaultRig;
	{
		int32 Best = -1;
		for (const TPair<FString, TArray<TSharedPtr<FSpeechLibraryRow>>>& Group : Groups)
		{
			if (Group.Key.Contains(TEXT("Face_Archetype_Skeleton")))
			{
				DefaultRig = Group.Key;
				break;
			}
			if (Group.Value.Num() > Best)
			{
				Best = Group.Value.Num();
				DefaultRig = Group.Key;
			}
		}
	}

	// Rigless lines join the default slice rather than forming a ghost slice of their own.
	if (!DefaultRig.IsEmpty())
	{
		if (TArray<TSharedPtr<FSpeechLibraryRow>>* Rigless = Groups.Find(FString()))
		{
			Groups.FindOrAdd(DefaultRig).Append(*Rigless);
			Groups.Remove(FString());
		}
	}

	// One rig is the usual case and stays silent. Several is worth saying out loud: one face bank
	// per voice bank is the norm, and this bank breaks it for a reason the person should read.
	if (Groups.Num() > 1)
	{
		FString Plan;
		for (const TPair<FString, TArray<TSharedPtr<FSpeechLibraryRow>>>& Group : Groups)
		{
			const FString RigName = Group.Key.IsEmpty()
				? FString(TEXT("no rig")) : FPaths::GetBaseFilename(Group.Key);

			Plan += Group.Key == DefaultRig
				? FString::Printf(TEXT("\n    %s - %d line(s) -> the default face bank"),
					*RigName, Group.Value.Num())
				: FString::Printf(TEXT("\n    %s - %d line(s) -> a bank suffixed _%s"),
					*RigName, Group.Value.Num(), *RigName);
		}

		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
			FText::FromString(FString::Printf(
				TEXT("Usually one face bank serves one voice bank - but this bank's speakers use ")
				TEXT("%d different rigs, so it needs one face bank per rig:\n%s\n\nCreate/update ")
				TEXT("them now?"),
				Groups.Num(), *Plan)));

		if (Choice != EAppReturnType::Yes)
		{
			return FReply::Handled();
		}
	}

	TArray<FString> Results;
	for (const TPair<FString, TArray<TSharedPtr<FSpeechLibraryRow>>>& Group : Groups)
	{
		TMap<FString, FString> Extra;

		// A serving bank already targeting this rig wins outright - the line-level model reuses
		// homes before inventing them. Only a rig with no serving bank creates one.
		const TSharedPtr<FPerformFaceBankRow>* Existing = Group.Key.IsEmpty() ? nullptr
			: PerformFaceBanks.FindByPredicate(
				[&Group](const TSharedPtr<FPerformFaceBankRow>& Bank)
				{
					return Bank.IsValid() && Bank->SkeletonPath == Group.Key;
				});

		if (Existing)
		{
			Extra.Add(TEXT("explicitFaceBankPath"), (*Existing)->Path);
		}
		else
		{
			if (!Group.Key.IsEmpty())
			{
				Extra.Add(TEXT("targetSkeletonPath"), Group.Key);
			}
			if (Group.Key != DefaultRig)
			{
				Extra.Add(TEXT("faceBankSuffix"), FPaths::GetBaseFilename(Group.Key));
			}
		}

		const FString Json = BuildContextJsonForRows(Group.Value, Extra);
		if (!Json.IsEmpty())
		{
			Results.Add(InvokeActionFunction(Action, Json));
		}
	}

	PerformMessage = FText::FromString(FString::Join(Results, TEXT("\n")));
	RefreshAll();
	return FReply::Handled();
}

TSharedPtr<FSpeechLibraryAction> SSpeechLibraryPanel::FindPerRigAction() const
{
	for (const TSharedPtr<FSpeechLibraryAction>& Action : DiscoveredActions)
	{
		if (Action.IsValid() && Action->bPerRig)
		{
			return Action;
		}
	}
	return nullptr;
}

TArray<TSharedPtr<FSpeechLibraryRow>> SSpeechLibraryPanel::UnassignedRows() const
{
	TArray<TSharedPtr<FSpeechLibraryRow>> Unassigned;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		if (Row.IsValid() && GetFaceStatusLabel(Row->Status.Handle.LineId).IsEmpty())
		{
			Unassigned.Add(Row);
		}
	}
	return Unassigned;
}

FReply SSpeechLibraryPanel::OnAutoAssignClicked()
{
	TSharedPtr<FSpeechLibraryAction> Action = FindPerRigAction();
	if (!Action.IsValid())
	{
		PerformMessage = LOCTEXT("PerRigGone", "That action's plugin is no longer loaded.");
		return FReply::Handled();
	}

	// Auto-assign fills ASSOCIATED banks only. It never creates one and never touches a bank that
	// does not serve this voice bank - "auto" must be predictable, and creating is Create/Update
	// Face Bank's job, chosen on purpose.
	if (PerformFaceBanks.Num() == 0)
	{
		PerformMessage = LOCTEXT("AutoAssignNoBanks",
			"No face bank is associated with this voice bank, so there is nothing to auto-assign "
			"into. Create/Update Face Bank makes the banks first.");
		return FReply::Handled();
	}

	// Bucket the unassigned lines into the associated bank their rig matches.
	TMap<FString, TArray<TSharedPtr<FSpeechLibraryRow>>> PerBank;
	int32 NoHome = 0;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : UnassignedRows())
	{
		const FString* Rig = SpeakerRigPaths.Find(Row->Status.SpeakerId);
		const TSharedPtr<FPerformFaceBankRow>* Match = nullptr;
		if (Rig && !Rig->IsEmpty())
		{
			Match = PerformFaceBanks.FindByPredicate(
				[Rig](const TSharedPtr<FPerformFaceBankRow>& Bank)
				{
					return Bank.IsValid() && Bank->SkeletonPath == *Rig;
				});
		}

		if (Match)
		{
			PerBank.FindOrAdd((*Match)->Path).Add(Row);
		}
		else
		{
			++NoHome;
		}
	}

	if (PerBank.Num() == 0)
	{
		PerformMessage = LOCTEXT("AutoAssignNoMatch",
			"None of the unassigned lines match an associated bank's skeleton - check the "
			"speakers' rigs, or use Create/Update Face Bank for a new rig's bank.");
		return FReply::Handled();
	}

	TArray<FString> Results;
	for (const TPair<FString, TArray<TSharedPtr<FSpeechLibraryRow>>>& Bucket : PerBank)
	{
		TMap<FString, FString> Extra;
		Extra.Add(TEXT("explicitFaceBankPath"), Bucket.Key);

		const FString Json = BuildContextJsonForRows(Bucket.Value, Extra);
		if (!Json.IsEmpty())
		{
			Results.Add(InvokeActionFunction(Action, Json));
		}
	}

	if (NoHome > 0)
	{
		Results.Add(FString::Printf(
			TEXT("%d line(s) skipped - no associated bank matches their rig (or the speaker has ")
			TEXT("no rig). Create/Update Face Bank covers those."), NoHome));
	}

	PerformMessage = FText::FromString(FString::Join(Results, TEXT("\n")));
	RefreshAll();
	return FReply::Handled();
}

TArray<TSharedPtr<FPerformFaceBankRow>> SSpeechLibraryPanel::GetAssignableFaceBanks(FName SpeakerId) const
{
	TArray<TSharedPtr<FPerformFaceBankRow>> Matching;

	const FString* Rig = SpeakerRigPaths.Find(SpeakerId);
	if (!Rig || Rig->IsEmpty())
	{
		return Matching;
	}

	// Serving banks first - the natural homes - then ANY bank in the project targeting this rig:
	// the line is the join, so a character's bank may take lines from every scene they appear in.
	// The skeleton is the one restriction, and it is read off the registry tag, never by loading.
	TSet<FString> Seen;
	for (const TSharedPtr<FPerformFaceBankRow>& Bank : PerformFaceBanks)
	{
		if (Bank.IsValid() && Bank->SkeletonPath == *Rig)
		{
			Matching.Add(Bank);
			Seen.Add(SpeechPerformPrivate::ToPackagePath(Bank->Path));
		}
	}

	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> FaceBankAssets;
	AssetRegistry.Get().GetAssetsByClass(
		FTopLevelAssetPath(TEXT("/Script/FaceForge"), TEXT("FaceBank")), FaceBankAssets, true);

	const FString RigPackage = SpeechPerformPrivate::ToPackagePath(*Rig);
	for (const FAssetData& Data : FaceBankAssets)
	{
		FString Skeleton;
		if (!Data.GetTagValue(TEXT("TargetSkeleton"), Skeleton) || Skeleton.IsEmpty())
		{
			// A bank saved before the tag existed answers after its next save stamps it.
			continue;
		}

		// A soft-pointer tag may arrive plain or as "Skeleton'/Game/X.A'".
		int32 Quote = INDEX_NONE;
		if (Skeleton.FindChar(TEXT('\''), Quote))
		{
			Skeleton = Skeleton.Mid(Quote + 1);
			Skeleton.RemoveFromEnd(TEXT("'"));
		}

		if (SpeechPerformPrivate::ToPackagePath(Skeleton) != RigPackage)
		{
			continue;
		}

		const FString Path = Data.GetSoftObjectPath().ToString();
		if (Seen.Contains(SpeechPerformPrivate::ToPackagePath(Path)))
		{
			continue;
		}

		TSharedRef<FPerformFaceBankRow> Row = MakeShared<FPerformFaceBankRow>();
		Row->Path = Path;
		Row->Label = Data.AssetName.ToString();
		Row->SkeletonPath = *Rig;
		Row->SkeletonLabel = FPaths::GetBaseFilename(*Rig);
		Matching.Add(Row);
	}

	return Matching;
}

FReply SSpeechLibraryPanel::OnAssignLineToBank(FName LineId, FString FaceBankPath)
{
	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		if (Row.IsValid() && Row->Status.Handle.LineId == LineId)
		{
			return RunPerRigAction(FindPerRigAction(), { Row }, FaceBankPath);
		}
	}
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnAssignLineToNewBank(FName LineId)
{
	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		if (!Row.IsValid() || Row->Status.Handle.LineId != LineId)
		{
			continue;
		}

		// A new bank needs to know its rig; a speaker without one has nothing to target and the
		// Rig badge already says what to fix.
		const FString* Rig = SpeakerRigPaths.Find(Row->Status.SpeakerId);
		if (!Rig || Rig->IsEmpty())
		{
			PerformMessage = LOCTEXT("AssignNeedsRig",
				"This speaker has no rig - set Rig Target on the speaker sheet first (Cast page).");
			return FReply::Handled();
		}

		TSharedPtr<FSpeechLibraryAction> Action = FindPerRigAction();
		if (!Action.IsValid())
		{
			PerformMessage = LOCTEXT("PerRigGone", "That action's plugin is no longer loaded.");
			return FReply::Handled();
		}

		TMap<FString, FString> Extra;
		Extra.Add(TEXT("targetSkeletonPath"), *Rig);
		if (PerformFaceBanks.Num() > 0)
		{
			// The first bank of a speech bank is the unsuffixed default; every later one carries
			// its rig's name.
			Extra.Add(TEXT("faceBankSuffix"), FPaths::GetBaseFilename(*Rig));
		}

		const FString Json = BuildContextJsonForRows({ Row }, Extra);
		if (!Json.IsEmpty())
		{
			PerformMessage = FText::FromString(InvokeActionFunction(Action, Json));
			RefreshAll();
		}
		return FReply::Handled();
	}
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Ingest page - methods from tool metadata, never written here
// -------------------------------------------------------------------------------------------------

namespace SpeechIngestPrivate
{
	/**
	 * Does this asset satisfy an ingest method's class filter - the class itself, a subclass, or a
	 * Blueprint whose native parent is one? The last case is what a Narrative dialogue is: the
	 * registry knows it as a Blueprint, and only its NativeParentClass tag says what it really is.
	 */
	bool MatchesClass(const FAssetData& Asset, const FString& ClassPath)
	{
		FTopLevelAssetPath Wanted;
		if (!Wanted.TrySetPath(ClassPath))
		{
			return false;
		}

		if (Asset.AssetClassPath == Wanted)
		{
			return true;
		}

		UClass* WantedClass = FindObject<UClass>(Wanted);

		if (WantedClass)
		{
			if (UClass* AssetClass = FindObject<UClass>(Asset.AssetClassPath))
			{
				if (AssetClass->IsChildOf(WantedClass))
				{
					return true;
				}
			}
		}

		FString ParentExport;
		if (Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, ParentExport))
		{
			const FString ParentPath = FPackageName::ExportTextPathToObjectPath(ParentExport);
			if (ParentPath == ClassPath)
			{
				return true;
			}
			if (WantedClass)
			{
				if (UClass* Parent = FindObject<UClass>(nullptr, *ParentPath))
				{
					return Parent->IsChildOf(WantedClass);
				}
			}
		}

		return false;
	}
}

FString SSpeechLibraryPanel::IngestSourcePath(const TSharedPtr<FSpeechIngestAction>& Action) const
{
	if (!IngestPickedAsset.IsEmpty())
	{
		return Action->bPickedMatches ? IngestPickedAsset : FString();
	}

	return !BankSourceAdapter.IsNone() && BankSourceAdapter == Action->AdapterKey
		? BankSourceAssetPath : FString();
}

TSharedRef<SWidget> SSpeechLibraryPanel::MakeIngestPage()
{
	// One bordered surface for everything about the source - status, drift, picker, methods - so
	// the page reads as one subject with room to breathe, per the house style.
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("IngestHeader", "Dialogue source"))
		.Font(FAppStyle::GetFontStyle("BoldFont"))
	];

	// Where the chosen bank already reads from, so assigning a source and seeing the assignment
	// are the same place. Live, because ingesting below changes the answer.
	Box->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text_Lambda([this]() -> FText
		{
			if (!ChosenBank.IsValid())
			{
				return LOCTEXT("IngestNoBank",
					"Pick a bank above, or create one in the Content Browser: right-click > "
					"Automation Forge > SpeechForge > Speech Bank.");
			}

			if (BankSourceAdapter.IsNone())
			{
				return LOCTEXT("IngestUnsourced",
					"This bank has no source yet. Ingest below to fill it and give it one - the "
					"source is remembered on the bank, and Re-harvest pulls new lines from it "
					"from then on.");
			}

			return FText::Format(LOCTEXT("IngestSource",
				"Reads from {0} via {1}. Lines already generated keep their audio when the "
				"source is read again."),
				FText::FromString(FPaths::GetBaseFilename(BankSourceAssetPath)),
				FText::FromName(BankSourceAdapter));
		})
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];

	// Drift the bank cannot see on its own: the script moved somewhere else. The warning lives
	// where the cure does - Re-harvest is a button on this page.
	Box->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Visibility_Lambda([this]()
		{
			return BankSourceDrifted ? EVisibility::Visible : EVisibility::Collapsed;
		})
		.Text_Lambda([this]()
		{
			return FText::Format(LOCTEXT("IngestSourceDrifted",
				"The source has changed since this bank last read it - {0}. Re-harvest to pull "
				"the edits in; any line whose words moved will then say so."),
				FText::FromString(BankSourceDriftDetail));
		})
		.ColorAndOpacity(FSlateColor(FStyleColors::Warning))
		.AutoWrapText(true)
	];

	// ONE source, several ways to read it. A single picker - the dialogue is one thing - and a
	// button per method below it; two pickers implied two sources, and there is one.
	if (IngestActions.Num() > 0)
	{
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("IngestSourceLabel", "Source"))
			]

			// AllowedClass stays UObject because the interesting assets are Blueprints - the
			// registry files a dialogue under Blueprint, and only the filter below can look at
			// its parentage. An explicit pick wins; otherwise the bank's own saved stamp shows,
			// which is what survives an editor restart.
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UObject::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([this]() -> FString
				{
					if (!IngestPickedAsset.IsEmpty())
					{
						return IngestPickedAsset;
					}

					for (const TSharedPtr<FSpeechIngestAction>& Action : IngestActions)
					{
						if (!BankSourceAdapter.IsNone() && BankSourceAdapter == Action->AdapterKey)
						{
							return BankSourceAssetPath;
						}
					}
					return FString();
				})
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					IngestPickedAsset = Asset.IsValid() ? Asset.GetObjectPathString() : FString();
					for (const TSharedPtr<FSpeechIngestAction>& Action : IngestActions)
					{
						Action->bPickedMatches = Asset.IsValid() &&
							SpeechIngestPrivate::MatchesClass(Asset, Action->AssetClassPath);
					}
				})
				.OnShouldFilterAsset_Lambda([this](const FAssetData& Asset)
				{
					for (const TSharedPtr<FSpeechIngestAction>& Action : IngestActions)
					{
						if (SpeechIngestPrivate::MatchesClass(Asset, Action->AssetClassPath))
						{
							return false;
						}
					}
					return true;
				})
			]
		];

		TSharedRef<SHorizontalBox> MethodButtons = SNew(SHorizontalBox);
		for (const TSharedPtr<FSpeechIngestAction>& Action : IngestActions)
		{
			MethodButtons->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(Action->Label))
				.ToolTipText(FText::FromString(Action->Tooltip))
				.IsEnabled_Lambda([this, Action]()
				{
					return ChosenBank.IsValid() && !IngestSourcePath(Action).IsEmpty();
				})
				.OnClicked(this, &SSpeechLibraryPanel::OnIngestClicked, Action)
			];
		}

		// The page-tagged actions ride the same row: Re-harvest re-reads the same source the
		// methods read, and one row of source operations beats two orphaned strips.
		for (const TSharedPtr<FSpeechLibraryAction>& PageAction : DiscoveredActions)
		{
			if (PageAction->Page != TEXT("Ingest"))
			{
				continue;
			}

			MethodButtons->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.ContentPadding(FMargin(12, 4))
				.ToolTipText(FText::FromString(PageAction->Tooltip))
				.OnClicked(this, &SSpeechLibraryPanel::OnActionClicked, PageAction)
				[
					SNew(STextBlock).Text(FText::FromString(PageAction->Label))
				]
			];
		}

		Box->AddSlot().AutoHeight()
		[
			MethodButtons
		];
	}

	TSharedRef<SVerticalBox> Page = SNew(SVerticalBox);

	Page->AddSlot().AutoHeight().Padding(8, 8, 8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		[
			Box
		]
	];

	Page->AddSlot().AutoHeight().Padding(8, 4, 8, 8)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text_Lambda([this]() { return IngestMessage; })
	];

	return Page;
}

FReply SSpeechLibraryPanel::OnIngestClicked(TSharedPtr<FSpeechIngestAction> Action)
{
	UClass* ToolClass = Action->ToolClass.Get();
	UFunction* Function = ToolClass ? ToolClass->FindFunctionByName(Action->FunctionName) : nullptr;
	if (!Function)
	{
		IngestMessage = LOCTEXT("IngestGone", "That method's plugin is no longer loaded.");
		return FReply::Handled();
	}

	const FString SourcePath = IngestSourcePath(Action);
	if (!ChosenBank.IsValid() || SourcePath.IsEmpty())
	{
		IngestMessage = LOCTEXT("IngestNeedsBoth", "Pick a bank and a source asset first.");
		return FReply::Handled();
	}

	// Ingesting the SAME source again is the idempotent path - line ids are the source's own, so
	// nothing duplicates and generated audio stays. A DIFFERENT source into a stamped bank is a
	// decision, not a default: replace, merge, or back out. This dialog exists because a test
	// harvest once merged a whole second dialogue into a bark bank without a word said.
	const bool bSameSource =
		SourcePath.Contains(BankSourceAssetPath) || BankSourceAssetPath.Contains(SourcePath);

	if (!BankSourceAdapter.IsNone() && !bSameSource)
	{
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNoCancel,
			FText::Format(LOCTEXT("IngestReplaceAsk",
				"This bank reads from {0}.\n\n"
				"Yes: replace - clear this bank's lines, then ingest {1}.\n"
				"No: merge - keep the current lines and add {1}'s beside them.\n"
				"Cancel: do nothing.\n\n"
				"Either way, audio assets already generated stay in the project."),
				FText::FromString(FPaths::GetBaseFilename(BankSourceAssetPath)),
				FText::FromString(FPaths::GetBaseFilename(SourcePath))));

		if (Choice == EAppReturnType::Cancel)
		{
			return FReply::Handled();
		}

		if (Choice == EAppReturnType::Yes)
		{
			FString ClearError;
			if (USpeechForgeSubsystem* Forge = Subsystem())
			{
				Forge->ClearBankLines(*ChosenBank, ClearError);
			}
			if (!ClearError.IsEmpty())
			{
				IngestMessage = FText::FromString(ClearError);
				return FReply::Handled();
			}
		}
	}

	// The ingest convention: two strings, source asset first, target bank second, a string back.
	// Filled in declaration order, which is how the contract states it - see FSpeechIngestAction.
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(Function->ParmsSize));
	FMemory::Memzero(Frame, Function->ParmsSize);

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(Frame);
		}
	}

	int32 StringsFilled = 0;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) &&
			!It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				AsString->SetPropertyValue_InContainer(
					Frame, StringsFilled == 0 ? SourcePath : *ChosenBank);

				if (++StringsFilled == 2)
				{
					break;
				}
			}
		}
	}

	ToolClass->GetDefaultObject()->ProcessEvent(Function, Frame);

	FString ResultText;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			if (const FStrProperty* AsString = CastField<FStrProperty>(*It))
			{
				ResultText = AsString->GetPropertyValue_InContainer(Frame);
			}
		}
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->DestroyValue_InContainer(Frame);
		}
	}

	IngestMessage = ResultText.IsEmpty()
		? FText::Format(LOCTEXT("IngestSilent",
			"{0} reported nothing - the Output Log has the reason."), FText::FromString(Action->Label))
		: FText::FromString(ResultText);

	// The harvest changed lines, speakers and the source stamp; every page needs the news.
	RefreshAll();
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Perform page - faces and performances, everything discovered or reached by reflection
// -------------------------------------------------------------------------------------------------

void SSpeechLibraryPanel::RefreshPerform()
{
	PerformFaceBanks.Reset();
	PerformSessions.Reset();
	SpeakerRigLabels.Reset();
	SpeakerRigPaths.Reset();
	LineFaceStatus.Reset();
	LineFaceBankPaths.Reset();
	LineSessionChips.Reset();

	// "PerformanceForge installed" means exactly: its session class resolves.
	UClass* SessionClass = FindObject<UClass>(
		FTopLevelAssetPath(TEXT("/Script/PerformanceForge"), TEXT("PerformanceSessionAsset")));
	bPerformancePresent = SessionClass != nullptr;

	if (ChosenBank.IsValid())
	{
		FString BankPackage = *ChosenBank;
		BankPackage.Split(TEXT("."), &BankPackage, nullptr);

		// Rigs: each speaker sheet's own answer. Adapter-derived suggestions come later; the
		// sheet's field is the contract that works with no game framework installed.
		for (const TSharedPtr<FSpeechCastRow>& Cast : CastRows)
		{
			if (!Cast.IsValid() || Cast->AssetPath.IsEmpty())
			{
				continue;
			}

			if (USpeechSpeaker* Sheet = LoadObject<USpeechSpeaker>(nullptr, *Cast->AssetPath))
			{
				if (!Sheet->RigTarget.IsNull())
				{
					SpeakerRigLabels.Add(Cast->SpeakerId, Sheet->RigTarget.GetAssetName());
					SpeakerRigPaths.Add(Cast->SpeakerId, Sheet->RigTarget.ToString());
				}
			}
		}

		FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		// THE LINE IS THE JOIN, WITHIN ONE LANGUAGE. A face bank serves this voice bank because it
		// HOLDS these lines' clips - not because of any bank-to-bank link - so one face bank may
		// serve many voice banks and one voice bank many face banks, and the restrictions a bank
		// imposes are its skeleton and its language. Candidates come cheap off the ClipIdIndex
		// registry tag; banks saved before the index existed are loaded once, and their next save
		// stamps it.
		TSet<FName> SpeechLineIdSet;
		for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
		{
			if (Row.IsValid())
			{
				SpeechLineIdSet.Add(Row->Status.Handle.LineId);
			}
		}

		// A localised bank holds its source's line ids on purpose - that is what "a bank per
		// language, joined by line id" means - so the line join alone matches every language at
		// once. Language is therefore part of the question, not a tiebreak: the Spanish bank was
		// listing the English, Spanish and German face banks for all six lines, and offering an
		// English mouth for Spanish words on every row.
		//
		// A face bank's language is the language of the speech bank it CLAIMS, read off that
		// bank's own registry tag. Derived rather than stored, because the claim is deliberately
		// the only copy of the link and a second copy of a derived fact is a second thing that can
		// disagree. Nothing needs re-saving for this to work.
		TMap<FString, FString> SpeechBankLanguages;
		{
			TArray<FAssetData> SpeechBankAssets;
			AssetRegistry.Get().GetAssetsByClass(
				USpeechBank::StaticClass()->GetClassPathName(), SpeechBankAssets, true);

			for (const FAssetData& Data : SpeechBankAssets)
			{
				FString Language;
				Data.GetTagValue(GET_MEMBER_NAME_CHECKED(USpeechBank, LanguageCode), Language);
				SpeechBankLanguages.Add(
					SpeechPerformPrivate::ToPackagePath(Data.GetSoftObjectPath().ToString()), Language);
			}
		}

		const FString* OpenLanguage = SpeechBankLanguages.Find(BankPackage);
		const FString BankLanguage = OpenLanguage ? *OpenLanguage : FString();

		TArray<FAssetData> FaceBankAssets;
		AssetRegistry.Get().GetAssetsByClass(
			FTopLevelAssetPath(TEXT("/Script/FaceForge"), TEXT("FaceBank")), FaceBankAssets, true);

		for (const FAssetData& Data : FaceBankAssets)
		{
			// Language before ids, because it is identity rather than a hint: a bank of another
			// language cannot serve these lines however many ids it shares with them.
			FString Claimed;
			const FString* ClaimLanguage =
				Data.GetTagValue(TEXT("SourceSpeechBankPath"), Claimed) && !Claimed.IsEmpty()
					? SpeechBankLanguages.Find(SpeechPerformPrivate::ToPackagePath(Claimed))
					: nullptr;

			// A bank claiming nothing has no language to be asked for - hand-built, or made before
			// banks claimed one. It belongs to the source language, the only one that existed then.
			if (ClaimLanguage ? (*ClaimLanguage != BankLanguage) : !BankLanguage.IsEmpty())
			{
				continue;
			}

			// Shortlist by the index: any of our line ids present? A missing index means a
			// legacy bank - load and look, once.
			FString Index;
			if (Data.GetTagValue(TEXT("ClipIdIndex"), Index) && !Index.IsEmpty())
			{
				bool bAnyHit = false;
				for (const FName LineId : SpeechLineIdSet)
				{
					if (Index.Contains(TEXT(";") + LineId.ToString() + TEXT(";")))
					{
						bAnyHit = true;
						break;
					}
				}
				if (!bAnyHit)
				{
					continue;
				}
			}

			UObject* FaceBank = LoadObject<UObject>(nullptr, *Data.GetSoftObjectPath().ToString());
			if (!FaceBank)
			{
				continue;
			}

			const FArrayProperty* ClipsProp =
				CastField<FArrayProperty>(FaceBank->GetClass()->FindPropertyByName(TEXT("Clips")));
			const FStructProperty* ClipProp =
				ClipsProp ? CastField<FStructProperty>(ClipsProp->Inner) : nullptr;
			if (!ClipProp)
			{
				continue;
			}

			TSharedRef<FPerformFaceBankRow> FaceBankRow = MakeShared<FPerformFaceBankRow>();
			FaceBankRow->Path = Data.GetSoftObjectPath().ToString();
			FaceBankRow->Label = Data.AssetName.ToString();

			if (const FSoftObjectProperty* SkeletonProp = CastField<FSoftObjectProperty>(
				FaceBank->GetClass()->FindPropertyByName(TEXT("TargetSkeleton"))))
			{
				const FSoftObjectPtr Value = SkeletonProp->GetPropertyValue_InContainer(FaceBank);
				FaceBankRow->SkeletonPath = Value.ToSoftObjectPath().ToString();
				FaceBankRow->SkeletonLabel = Value.ToSoftObjectPath().GetAssetName();
			}

			FScriptArrayHelper Clips(ClipsProp, ClipsProp->ContainerPtrToValuePtr<void>(FaceBank));
			for (int32 Index2 = 0; Index2 < Clips.Num(); ++Index2)
			{
				const void* Clip = Clips.GetRawPtr(Index2);
				const FString ClipId = SpeechPerformPrivate::ReadStructString(
					ClipProp->Struct, Clip, TEXT("ClipId"));
				if (ClipId.IsEmpty())
				{
					continue;
				}

				const FName ClipName(*ClipId);
				if (!SpeechLineIdSet.Contains(ClipName))
				{
					continue;
				}

				// The clip's own audio path used to stand in for its language here. It could not:
				// a freshly cloned localised bank has no audio yet and so matched every language
				// by id alone, and a line re-generated to a new sound made its own face look
				// foreign. The bank's language answers the question the audio was guessing at.
				++FaceBankRow->CoveredCount;

				const FString Status = SpeechPerformPrivate::ReadStructEnum(
					ClipProp->Struct, Clip, TEXT("Status"));
				const FString Entry = FString::Printf(TEXT("%s - %s"), *Status, *FaceBankRow->Label);
				FString& Value = LineFaceStatus.FindOrAdd(ClipName);
				Value += Value.IsEmpty() ? Entry : TEXT("  /  ") + Entry;

				// Where the row's Open button jumps. First bank wins the jump; the status text
				// still names every holder, so a duplicate stays visible.
				if (!LineFaceBankPaths.Contains(ClipName))
				{
					LineFaceBankPaths.Add(ClipName, FaceBankRow->Path);
				}
			}

			// Serving means holding at least one of OUR lines' faces.
			if (FaceBankRow->CoveredCount > 0)
			{
				PerformFaceBanks.Add(FaceBankRow);
			}
		}

		// Sessions covering this bank: the registry stamp answers cheaply; an older, unstamped
		// asset is loaded, read, and restamped in passing so it answers cheaply next time.
		if (bPerformancePresent)
		{
			TArray<FAssetData> SessionAssets;
			AssetRegistry.Get().GetAssetsByClass(
				FTopLevelAssetPath(TEXT("/Script/PerformanceForge"), TEXT("PerformanceSessionAsset")),
				SessionAssets, true);

			for (const FAssetData& Data : SessionAssets)
			{
				FString Stamp;
				const bool bStamped = Data.GetTagValue(TEXT("BankPaths"), Stamp) && !Stamp.IsEmpty();
				if (bStamped && !Stamp.Contains(BankPackage))
				{
					continue;
				}

				UObject* Session = LoadObject<UObject>(nullptr, *Data.GetSoftObjectPath().ToString());
				if (!Session)
				{
					continue;
				}

				const FArrayProperty* LinesProp =
					CastField<FArrayProperty>(Session->GetClass()->FindPropertyByName(TEXT("Lines")));
				const FStructProperty* LineProp =
					LinesProp ? CastField<FStructProperty>(LinesProp->Inner) : nullptr;
				if (!LineProp)
				{
					continue;
				}

				FScriptArrayHelper SessionLines(LinesProp, LinesProp->ContainerPtrToValuePtr<void>(Session));

				TArray<FString> AllBanks;
				TArray<FName> BankLineIds;
				for (int32 Index = 0; Index < SessionLines.Num(); ++Index)
				{
					const void* Line = SessionLines.GetRawPtr(Index);
					const FString LineBank = SpeechPerformPrivate::ReadStructString(
						LineProp->Struct, Line, TEXT("AssetPath"));
					const FString LineId = SpeechPerformPrivate::ReadStructString(
						LineProp->Struct, Line, TEXT("LineId"));

					AllBanks.AddUnique(LineBank);
					if (SpeechPerformPrivate::RefersToBank(LineBank, BankPackage))
					{
						BankLineIds.Add(FName(*LineId));
					}
				}

				if (!bStamped)
				{
					// Restamp so the next visit is a registry hit. Dirtied, not saved: it rides
					// the next save like any other edit.
					if (FStrProperty* StampProp = CastField<FStrProperty>(
						Session->GetClass()->FindPropertyByName(TEXT("BankPaths"))))
					{
						StampProp->SetPropertyValue_InContainer(Session, FString::Join(AllBanks, TEXT(";")));
						Session->MarkPackageDirty();
					}
				}

				if (BankLineIds.Num() == 0)
				{
					continue;
				}

				TSharedRef<FPerformSessionRow> Row = MakeShared<FPerformSessionRow>();
				Row->Path = Data.GetSoftObjectPath().ToString();
				Row->Label = Data.AssetName.ToString();
				Row->BankLineCount = BankLineIds.Num();
				Row->TotalLineCount = SessionLines.Num();
				PerformSessions.Add(Row);

				for (const FName LineId : BankLineIds)
				{
					FString& Chips = LineSessionChips.FindOrAdd(LineId);
					Chips += Chips.IsEmpty() ? Row->Label : TEXT(", ") + Row->Label;
				}
			}
		}
	}

	if (PerformFaceBankList.IsValid())
	{
		PerformFaceBankList->RequestListRefresh();
	}
	if (PerformSessionList.IsValid())
	{
		PerformSessionList->RequestListRefresh();
	}
	if (PerformList.IsValid())
	{
		PerformList->RequestListRefresh();
	}
}

TSharedRef<SWidget> SSpeechLibraryPanel::MakePerformPage()
{
	// The page's discovered buttons, split by what they act on: per-rig actions build face banks
	// and belong in the Faces box; an action naming the Dialogue group hands finished work to the
	// game and gets its own box; the rest are the recording flow and belong in Performance.
	TSharedRef<SHorizontalBox> FaceButtons = SNew(SHorizontalBox);
	TSharedRef<SHorizontalBox> SessionButtons = SNew(SHorizontalBox);
	TSharedRef<SHorizontalBox> DialogueButtons = SNew(SHorizontalBox);
	int32 DialogueButtonCount = 0;

	for (const TSharedPtr<FSpeechLibraryAction>& Action : DiscoveredActions)
	{
		if (Action->Page != TEXT("Perform"))
		{
			continue;
		}

		const bool bDialogue = Action->Group == TEXT("Dialogue");
		DialogueButtonCount += bDialogue ? 1 : 0;

		(bDialogue ? DialogueButtons : (Action->bPerRig ? FaceButtons : SessionButtons))
			->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.ContentPadding(FMargin(12, 4))
			.ToolTipText(FText::FromString(Action->Tooltip))
			.OnClicked(this, &SSpeechLibraryPanel::OnActionClicked, Action)
			[
				SNew(STextBlock).Text(FText::FromString(Action->Label))
			]
		];
	}

	FaceButtons->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
	[
		SNew(SButton)
		.ContentPadding(FMargin(12, 4))
		.Text(LOCTEXT("AutoAssign", "Auto-assign Unassigned"))
		.ToolTipText(LOCTEXT("AutoAssignTip",
			"Give every line with no face clip a home: existing banks catch the rigs they "
			"target, missing banks are created per rig, and a multi-rig split asks first."))
		.IsEnabled_Lambda([this]()
		{
			return FindPerRigAction().IsValid() && UnassignedRows().Num() > 0;
		})
		.OnClicked(this, &SSpeechLibraryPanel::OnAutoAssignClicked)
	];

	return SNew(SVerticalBox)

	// --- To the game: the last step, and the only one that leaves the pipeline ------------------
	//
	// First rather than last, because it is what the page is for: everything below builds the two
	// halves, and this is where they become a scene that speaks. Absent entirely when no adapter is
	// installed - with no dialogue framework there is nowhere for finished work to go.

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		.Visibility(DialogueButtonCount > 0 ? EVisibility::Visible : EVisibility::Collapsed)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PerformDialogueHeader", "To the game"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("PerformDialogueHint",
					"Put this bank's work onto the dialogue it was harvested from. The bank remembers "
					"which one, so there is nothing to pick. Safe to press again as more lines finish."))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				DialogueButtons
			]
		]
	]

	// --- Faces: its own bordered surface, so the banks read as management, not as more rows -----

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PerformFacesHeader", "Faces"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() -> FText
				{
					if (!ChosenBank.IsValid())
					{
						return LOCTEXT("PerformNoBank", "Pick a bank above.");
					}
					return PerformFaceBanks.Num() == 0
						? LOCTEXT("PerformNoFaceBank",
							"No face bank serves this speech bank yet. Press Create/Update Face "
							"Bank - one bank per rig is made from the lines' speakers.")
						: LOCTEXT("PerformFaceBanks", "Face banks serving this bank:");
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FStyleColors::Warning))
				.Visibility_Lambda([this]()
				{
					return UnassignedRows().Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("UnassignedReport",
						"{0} line(s) are in no face bank yet - Assign them on their rows, or "
						"Auto-assign."), UnassignedRows().Num());
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(PerformFaceBankList, SListView<TSharedPtr<FPerformFaceBankRow>>)
				.ListItemsSource(&PerformFaceBanks)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow_Lambda([this](TSharedPtr<FPerformFaceBankRow> Row, const TSharedRef<STableViewBase>& Owner)
					-> TSharedRef<ITableRow>
				{
					return SNew(STableRow<TSharedPtr<FPerformFaceBankRow>>, Owner)
					.Padding(FMargin(0, 4))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 24, 0)
						[
							SNew(STextBlock).Text(FText::FromString(Row->Label))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 24, 0)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Row->SkeletonLabel.IsEmpty()
								? TEXT("no skeleton configured")
								: *FString::Printf(TEXT("skeleton: %s"), *Row->SkeletonLabel)))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 24, 0)
						[
							SNew(STextBlock)
							.Text(FText::FromString(FString::Printf(TEXT("covers %d of %d line(s)"),
								Row->CoveredCount, Rows.Num())))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("OpenFaceBank", "Open in Face Bank Panel"))
							.ContentPadding(FMargin(12, 2))
							.ToolTipText(LOCTEXT("OpenFaceBankTip",
								"Open the Face Bank panel with this bank selected - solve, "
								"correct and bake live there."))
							.OnClicked(this, &SSpeechLibraryPanel::OnOpenFaceBank, Row->Path)
						]
					];
				})
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				FaceButtons
			]
		]
	]

	// --- The lines, with their bodies' facts ----------------------------------------------------

	+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 4)
	[
		SAssignNew(PerformList, SListView<TSharedPtr<FSpeechLibraryRow>>)
		.ListItemsSource(&Rows)
		.OnGenerateRow(this, &SSpeechLibraryPanel::MakePerformRow)
		.SelectionMode(ESelectionMode::Multi)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(ColumnLine).DefaultLabel(LOCTEXT("ColLine", "Line")).FillWidth(0.13f)
			+ SHeaderRow::Column(ColumnSpeaker).DefaultLabel(LOCTEXT("ColSpeaker", "Speaker")).FillWidth(0.11f)
			+ SHeaderRow::Column(ColumnRig).DefaultLabel(LOCTEXT("ColRig", "Rig")).FillWidth(0.16f)
			+ SHeaderRow::Column(ColumnFace).DefaultLabel(LOCTEXT("ColFace", "Face")).FillWidth(0.22f)
			+ SHeaderRow::Column(ColumnSessions).DefaultLabel(LOCTEXT("ColSessions", "Sessions")).FillWidth(0.24f)
			+ SHeaderRow::Column(ColumnDuration).DefaultLabel(LOCTEXT("ColLength", "Length")).FillWidth(0.08f)
			+ SHeaderRow::Column(ColumnPlay).DefaultLabel(FText::GetEmpty()).FixedWidth(28.f))
	]

	// --- Performance: the recording flow, its own surface, only when PerformanceForge exists ----

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		.Visibility_Lambda([this]()
		{
			return bPerformancePresent ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PerformSessionsHeader", "Performance"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("SessionLabel", "Session:"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					MakeStringCombo(&ModeOptions, &ChosenMode)
				]
				// No face bank picker here, deliberately: each line resolves its own face bank
				// from its clip's home, and the linked default covers lines that have none yet.
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakeStringCombo(&TreatmentOptions, &ChosenTreatment)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					return PerformSessions.Num() == 0
						? LOCTEXT("PerformNoSessions",
							"No recording session covers this bank yet - select lines and press "
							"Plan Session or Record Selected.")
						: LOCTEXT("PerformSessions", "Sessions covering this bank:");
				})
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(PerformSessionList, SListView<TSharedPtr<FPerformSessionRow>>)
				.ListItemsSource(&PerformSessions)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow_Lambda([this](TSharedPtr<FPerformSessionRow> Row, const TSharedRef<STableViewBase>& Owner)
					-> TSharedRef<ITableRow>
				{
					return SNew(STableRow<TSharedPtr<FPerformSessionRow>>, Owner)
					.Padding(FMargin(0, 4))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 24, 0)
						[
							SNew(STextBlock).Text(FText::FromString(Row->Label))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 24, 0)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Row->BankLineCount == Row->TotalLineCount
								? FString::Printf(TEXT("%d line(s)"), Row->TotalLineCount)
								: FString::Printf(TEXT("%d of its %d line(s) are from this bank"),
									Row->BankLineCount, Row->TotalLineCount)))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("OpenSession", "Open in Capture Panel"))
							.ContentPadding(FMargin(12, 2))
							.ToolTipText(LOCTEXT("OpenSessionTip",
								"Open Performance Capture with this session loaded, ready to record."))
							.OnClicked(this, &SSpeechLibraryPanel::OnOpenSession, Row->Path)
						]
					];
				})
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SessionButtons
			]
		]
	]

	// --- The message line -----------------------------------------------------------------------

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text_Lambda([this]() { return PerformMessage; })
	];
}

FReply SSpeechLibraryPanel::OnOpenFaceBank(FString FaceBankPath)
{
	const FString Result = SpeechPerformPrivate::InvokeStringTool(
		TEXT("/Script/FaceForgeToolset.FaceForgeToolset"), TEXT("OpenFaceBank"), FaceBankPath);

	if (!Result.IsEmpty())
	{
		PerformMessage = FText::FromString(Result);
	}
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnOpenLineFaceClip(FName LineId)
{
	const FString* BankPath = LineFaceBankPaths.Find(LineId);
	if (!BankPath)
	{
		return FReply::Handled();
	}

	const FString Result = SpeechPerformPrivate::InvokeStringTool(
		TEXT("/Script/FaceForgeToolset.FaceForgeToolset"), TEXT("OpenFaceBank"),
		*BankPath, LineId.ToString());

	if (!Result.IsEmpty())
	{
		PerformMessage = FText::FromString(Result);
	}
	return FReply::Handled();
}

FReply SSpeechLibraryPanel::OnOpenSession(FString SessionPath)
{
	const FString Result = SpeechPerformPrivate::InvokeStringTool(
		TEXT("/Script/PerformanceForgeToolset.PerformanceForgeToolset"),
		TEXT("OpenRecordingSession"), SessionPath);

	if (!Result.IsEmpty())
	{
		PerformMessage = FText::FromString(Result);
	}
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Localize page
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SSpeechLibraryPanel::MakeLocalizePage()
{
	return SNew(SVerticalBox)

	// --- Translator: which service, and whether it can be paid --------------------------------

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LocalizeTranslatorHeader", "Translator"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("LocalizeService", "Service"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox).MinDesiredWidth(160.f)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&TranslatorIds)
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Id)
						{
							return SNew(STextBlock).Text(FText::FromString(Id.IsValid() ? *Id : FString()));
						})
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Id, ESelectInfo::Type)
						{
							if (Id.IsValid())
							{
								ChosenTranslator = Id;
								RefreshLocalize();
							}
						})
						[
							SNew(STextBlock).Text_Lambda([this]()
							{
								return ChosenTranslator.IsValid()
									? FText::FromString(*ChosenTranslator)
									: LOCTEXT("LocalizeNoTranslator", "No translator installed");
							})
						]
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]()
					{
						FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
						const TSharedPtr<ISpeechTranslationProvider> Translator = Module && ChosenTranslator.IsValid()
							? Module->FindTranslationProvider(FName(**ChosenTranslator))
							: nullptr;
						if (!Translator.IsValid())
						{
							return FText::GetEmpty();
						}
						return Translator->HasCredential()
							? LOCTEXT("LocalizeKeyOk", "Key present. Translation is billed per character when sent.")
							: LOCTEXT("LocalizeKeyMissing", "No API key - add one on the Keys page (Tools > Automation Forge > Keys).");
					})
				]
			]
		]
	]

	// --- Languages: tick what to create -------------------------------------------------------

	+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LocalizeLanguagesHeader", "Languages"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("LocalizeLanguagesHint",
					"Tick the languages to create. Each becomes a sibling bank joined to this one by line "
					"id, with this bank's face banks cloned beside it. A language that already exists is "
					"updated: new and rewritten lines translate, current ones cost nothing."))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).MaxDesiredHeight(200.f)
				[
					SAssignNew(LocalizeLanguageList, SListView<TSharedPtr<FLocalizeLanguageRow>>)
					.ListItemsSource(&LocalizeLanguages)
					.OnGenerateRow(this, &SSpeechLibraryPanel::MakeLocalizeLanguageRow)
					.SelectionMode(ESelectionMode::None)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("LocalizeOtherCode", "Other code"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox).MinDesiredWidth(90.f)
					[
						SNew(SEditableTextBox)
						.HintText(LOCTEXT("LocalizeOtherHint", "e.g. sr"))
						.ToolTipText(LOCTEXT("LocalizeOtherTip",
							"A language the list does not offer. The translator decides whether it can."))
						.Text_Lambda([this]() { return FText::FromString(LocalizeCustomCode); })
						.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
						{
							LocalizeCustomCode = Text.ToString().TrimStartAndEnd();
						})
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bLocalizeForce ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bLocalizeForce = State == ECheckBoxState::Checked; })
					.ToolTipText(LOCTEXT("LocalizeForceTip",
						"Re-translate every line, current ones included. Bills every character again."))
					[
						SNew(STextBlock).Text(LOCTEXT("LocalizeForce", "Re-translate everything"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(24, 0, 0, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.ContentPadding(FMargin(12, 4))
					.Text(LOCTEXT("LocalizeCreate", "Create / Update Localized Banks"))
					.ToolTipText(LOCTEXT("LocalizeCreateTip",
						"Translate this bank into every ticked language - a speech bank each, its face banks "
						"cloned - then generate, solve and bake them like any other bank."))
					.IsEnabled_Lambda([this]()
					{
						if (!ChosenBank.IsValid() || !ChosenTranslator.IsValid() || LocalizePending > 0)
						{
							return false;
						}
						if (!LocalizeCustomCode.IsEmpty())
						{
							return true;
						}
						return LocalizeLanguages.ContainsByPredicate(
							[](const TSharedPtr<FLocalizeLanguageRow>& Row) { return Row.IsValid() && Row->bChecked; });
					})
					.OnClicked(this, &SSpeechLibraryPanel::OnCreateLocalizedClicked)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]() { return LocalizeMessage; })
			]
		]
	]

	// --- Localized banks: what exists, how current it is, and its faces -----------------------

	+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 4, 8, 8)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LocalizedBanksHeader", "Localized banks"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return LocalizedBanks.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("LocalizedBanksNone", "None yet for this bank."))
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(LocalizedBankList, SListView<TSharedPtr<FLocalizedBankRow>>)
				.ListItemsSource(&LocalizedBanks)
				.OnGenerateRow(this, &SSpeechLibraryPanel::MakeLocalizedBankRow)
				.SelectionMode(ESelectionMode::None)
			]
		]
	];
}

TSharedRef<ITableRow> SSpeechLibraryPanel::MakeLocalizeLanguageRow(
	TSharedPtr<FLocalizeLanguageRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<TSharedPtr<FLocalizeLanguageRow>>, Owner)
		.Padding(FMargin(4, 2))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([Row]() { return Row->bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Row](ECheckBoxState State) { Row->bChecked = State == ECheckBoxState::Checked; })
				[
					SNew(SBox).MinDesiredWidth(70.f)
					[
						SNew(STextBlock).Text(FText::FromString(Row->Code))
					]
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(Row->ExistingBankPath.IsEmpty()
					? FText::GetEmpty()
					: FText::Format(LOCTEXT("LocalizeExists", "exists: {0}"),
						FText::FromString(FPackageName::GetShortName(SpeechPerformPrivate::ToPackagePath(Row->ExistingBankPath)))))
			]
		];
}

TSharedRef<ITableRow> SSpeechLibraryPanel::MakeLocalizedBankRow(
	TSharedPtr<FLocalizedBankRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	const FText Counts = FText::Format(
		LOCTEXT("LocalizedCounts", "{0} current, {1} stale, {2} missing  |  {3} with audio"),
		FText::AsNumber(Row->LinesCurrent), FText::AsNumber(Row->LinesStale),
		FText::AsNumber(Row->LinesMissing), FText::AsNumber(Row->LinesWithAudio));

	return SNew(STableRow<TSharedPtr<FLocalizedBankRow>>, Owner)
		.Padding(FMargin(4, 4))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(60.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Row->LanguageCode.ToUpper()))
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(SBox).MinDesiredWidth(220.f)
				[
					SNew(STextBlock).Text(FText::FromString(Row->Label))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
			[
				SNew(STextBlock).Text(Counts)
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(16, 0, 0, 0)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(Row->FaceBankLabels.IsEmpty()
					? LOCTEXT("LocalizedNoFaces", "no face banks")
					: FText::Format(LOCTEXT("LocalizedFaces", "faces: {0}"), FText::FromString(Row->FaceBankLabels)))
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("LocalizedOpen", "Open"))
				.ToolTipText(LOCTEXT("LocalizedOpenTip", "Show this language's bank here - generate it, then solve its faces from Perform."))
				.OnClicked(this, &SSpeechLibraryPanel::OnOpenLocalizedBank, Row->BankPath)
			]
		];
}

void SSpeechLibraryPanel::RefreshLocalize()
{
	// Ticks survive a refresh, by code: a refresh fires on every bank event and must not untick
	// what somebody just chose.
	TSet<FString> PreviouslyChecked;
	for (const TSharedPtr<FLocalizeLanguageRow>& Row : LocalizeLanguages)
	{
		if (Row.IsValid() && Row->bChecked)
		{
			PreviouslyChecked.Add(Row->Code);
		}
	}

	TranslatorIds.Reset();
	LocalizeLanguages.Reset();
	LocalizedBanks.Reset();

	FSpeechForgeModule* Module = FSpeechForgeModule::GetPtr();
	USpeechForgeSubsystem* Forge = Subsystem();

	if (Module)
	{
		// Real translators first, the pseudo-localiser last - it is there for completeness.
		TArray<FName> Ids = Module->GetTranslationProviderIds();
		Ids.Sort([](const FName& A, const FName& B)
		{
			const bool bAPseudo = A == FName(TEXT("Pseudo"));
			const bool bBPseudo = B == FName(TEXT("Pseudo"));
			return bAPseudo != bBPseudo ? !bAPseudo : A.LexicalLess(B);
		});
		for (const FName Id : Ids)
		{
			TranslatorIds.Add(MakeShared<FString>(Id.ToString()));
		}

		const bool bChosenStillThere = ChosenTranslator.IsValid() && TranslatorIds.ContainsByPredicate(
			[this](const TSharedPtr<FString>& Id) { return *Id == *ChosenTranslator; });
		if (!bChosenStillThere)
		{
			ChosenTranslator = TranslatorIds.Num() > 0 ? TranslatorIds[0] : TSharedPtr<FString>();
		}
	}

	// The siblings that exist, with their counts - and the face banks cloned for each, found the
	// way Perform finds a bank's faces: by the claim the clone carries.
	TMap<FString, FString> ExistingByLanguage;
	if (Forge && ChosenBank.IsValid())
	{
		TArray<FAssetData> FaceBanks;
		if (UClass* FaceBankClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/FaceForge"), TEXT("FaceBank"))))
		{
			FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			AssetRegistry.Get().GetAssetsByClass(FaceBankClass->GetClassPathName(), FaceBanks, true);
		}

		for (const FSpeechLocalizationStatus& Status : Forge->GetLocalizationStatus(*ChosenBank))
		{
			TSharedRef<FLocalizedBankRow> Row = MakeShared<FLocalizedBankRow>();
			Row->LanguageCode   = Status.LanguageCode;
			Row->BankPath       = Status.BankPath;
			Row->Label          = FPackageName::GetShortName(SpeechPerformPrivate::ToPackagePath(Status.BankPath));
			Row->LinesCurrent   = Status.LinesCurrent;
			Row->LinesStale     = Status.LinesStale;
			Row->LinesMissing   = Status.LinesMissing;
			Row->LinesWithAudio = Status.LinesWithAudio;

			const FString LocalizedPackage = SpeechPerformPrivate::ToPackagePath(Status.BankPath);
			TArray<FString> Faces;
			for (const FAssetData& Data : FaceBanks)
			{
				FString Claim;
				if (Data.GetTagValue(TEXT("SourceSpeechBankPath"), Claim)
					&& SpeechPerformPrivate::RefersToBank(Claim, LocalizedPackage))
				{
					Faces.Add(Data.AssetName.ToString());
				}
			}
			Faces.Sort();
			Row->FaceBankLabels = FString::Join(Faces, TEXT(", "));

			ExistingByLanguage.Add(Status.LanguageCode, Status.BankPath);
			LocalizedBanks.Add(Row);
		}
	}

	// The translator's own list; a language that exists but is off the list still appears, so it
	// can be re-run from here.
	TArray<FString> Codes;
	if (Module && ChosenTranslator.IsValid())
	{
		if (const TSharedPtr<ISpeechTranslationProvider> Translator = Module->FindTranslationProvider(FName(**ChosenTranslator)))
		{
			Codes = Translator->GetTargetLanguages();
		}
	}
	for (const TPair<FString, FString>& Existing : ExistingByLanguage)
	{
		Codes.AddUnique(Existing.Key);
	}

	for (const FString& Code : Codes)
	{
		TSharedRef<FLocalizeLanguageRow> Row = MakeShared<FLocalizeLanguageRow>();
		Row->Code = Code;
		Row->bChecked = PreviouslyChecked.Contains(Code);
		if (const FString* Existing = ExistingByLanguage.Find(Code))
		{
			Row->ExistingBankPath = *Existing;
		}
		LocalizeLanguages.Add(Row);
	}

	if (LocalizeLanguageList.IsValid())
	{
		LocalizeLanguageList->RequestListRefresh();
	}
	if (LocalizedBankList.IsValid())
	{
		LocalizedBankList->RequestListRefresh();
	}
}

FReply SSpeechLibraryPanel::OnCreateLocalizedClicked()
{
	USpeechForgeSubsystem* Forge = Subsystem();
	if (!Forge || !ChosenBank.IsValid() || !ChosenTranslator.IsValid())
	{
		return FReply::Handled();
	}

	TArray<FString> Codes;
	for (const TSharedPtr<FLocalizeLanguageRow>& Row : LocalizeLanguages)
	{
		if (Row.IsValid() && Row->bChecked)
		{
			Codes.AddUnique(Row->Code);
		}
	}
	if (!LocalizeCustomCode.IsEmpty())
	{
		Codes.AddUnique(LocalizeCustomCode);
	}
	if (Codes.Num() == 0)
	{
		LocalizeMessage = LOCTEXT("LocalizeNothingTicked", "Tick at least one language.");
		return FReply::Handled();
	}

	const FString BankPath = *ChosenBank;
	const FName TranslatorId(**ChosenTranslator);

	LocalizePending = Codes.Num();
	LocalizeMessage = FText::Format(
		LOCTEXT("LocalizeStarted", "Translating {0} language(s) through {1}..."),
		FText::AsNumber(Codes.Num()), FText::FromName(TranslatorId));

	// Each language completes on its own, frames or seconds later, and the panel may be gone by
	// then: everything the callback touches goes through a weak pointer.
	TWeakPtr<SWidget> WeakSelf = AsShared();

	for (const FString& Code : Codes)
	{
		const FString Error = Forge->LocalizeBank(BankPath, Code, TranslatorId, bLocalizeForce,
			[WeakSelf, Code](bool bOk, const FString& Summary)
			{
				const TSharedPtr<SWidget> Pinned = WeakSelf.Pin();
				if (!Pinned.IsValid())
				{
					return;
				}
				SSpeechLibraryPanel* Self = static_cast<SSpeechLibraryPanel*>(Pinned.Get());

				Self->LocalizePending = FMath::Max(0, Self->LocalizePending - 1);

				FString Report = Summary;
				if (bOk && Self->ChosenBank.IsValid() && Self->Subsystem())
				{
					// The sibling's path comes from the status report rather than being derived
					// here - one place spells the naming convention.
					for (const FSpeechLocalizationStatus& Status : Self->Subsystem()->GetLocalizationStatus(*Self->ChosenBank))
					{
						if (Status.LanguageCode == Code)
						{
							Self->CloneFaceBanksForLanguage(Status.BankPath, Code);
							break;
						}
					}
				}

				Self->LocalizeMessage = FText::FromString(
					Self->LocalizeMessage.ToString().IsEmpty() ? Report
					: Self->LocalizeMessage.ToString() + TEXT("\n") + Report);

				if (Self->LocalizePending == 0)
				{
					Self->RefreshAll();
				}
			});

		if (!Error.IsEmpty())
		{
			LocalizePending = FMath::Max(0, LocalizePending - 1);
			LocalizeMessage = FText::FromString(LocalizeMessage.ToString() + TEXT("\n") + Code + TEXT(": ") + Error);
		}
	}

	if (LocalizePending == 0)
	{
		RefreshAll();
	}
	return FReply::Handled();
}

void SSpeechLibraryPanel::CloneFaceBanksForLanguage(const FString& LocalizedBankPath, const FString& LanguageCode)
{
	// The lines each serving face bank holds, as Perform worked them out - the clone gets exactly
	// those, so a bank split by rig stays split by rig in every language.
	TMap<FString, TArray<TSharedPtr<FSpeechLibraryRow>>> PerBank;
	for (const TSharedPtr<FSpeechLibraryRow>& Row : Rows)
	{
		if (!Row.IsValid())
		{
			continue;
		}
		if (const FString* FaceBank = LineFaceBankPaths.Find(Row->Status.Handle.LineId))
		{
			PerBank.FindOrAdd(*FaceBank).Add(Row);
		}
	}

	if (PerBank.Num() == 0)
	{
		return;
	}

	TArray<FString> Results;
	for (const TPair<FString, TArray<TSharedPtr<FSpeechLibraryRow>>>& Bank : PerBank)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("sourceFaceBankPath"), Bank.Key);
		Root->SetStringField(TEXT("localizedSpeechBankPath"), LocalizedBankPath);
		Root->SetStringField(TEXT("languageCode"), LanguageCode);

		TArray<TSharedPtr<FJsonValue>> Lines;
		for (const TSharedPtr<FSpeechLibraryRow>& Row : Bank.Value)
		{
			TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
			Line->SetStringField(TEXT("lineId"), Row->Status.Handle.LineId.ToString());
			Line->SetStringField(TEXT("speaker"), Row->Status.SpeakerId.ToString());
			Lines.Add(MakeShared<FJsonValueObject>(Line));
		}
		Root->SetArrayField(TEXT("lines"), Lines);

		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);

		Results.Add(SpeechPerformPrivate::InvokeStringTool(
			TEXT("/Script/FaceForgeToolset.FaceForgeToolset"), TEXT("CloneFaceBankForLanguage"), Json));
	}

	LocalizeMessage = FText::FromString(
		LocalizeMessage.ToString() + TEXT("\n") + FString::Join(Results, TEXT("\n")));
}

FReply SSpeechLibraryPanel::OnOpenLocalizedBank(FString BankPath)
{
	FocusOn(BankPath, NAME_None);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
