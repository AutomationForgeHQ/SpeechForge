# SpeechForge

Write a line, generate it with an AI speech provider, and get a `USoundWave` in the project with
character-level timing data attached to it.

```
cast a voice once  →  author lines  →  price  →  generate  →  import, with timings
```

**Status: 0.1 — verified end to end against a live ElevenLabs Creator account on 2026-08-11.** Three
lines across two speakers generated, imported at 48 kHz, and checked by reading the data back rather
than the log. See [What is verified, and what is not](#what-is-verified-and-what-is-not).

Setup is a separate page: **[SETUP.md](SETUP.md)** — the account, the API key, and exactly which
permissions it needs.

---

## What this is not

SpeechForge produces a `USoundWave` and stops there. It deliberately does **not** know about dialogue
trees, subtitles, montages, facial animation, or any gameplay framework.

Those are conventions belonging to whatever consumes the audio, and baking them in would tie this
plugin to one project's way of working. `NP_VoiceOver` is the adapter that binds this to Narrative
Pro's dialogue system — a separate plugin: it harvests dialogues into banks and seeds speaker
sheets, and SpeechForge never learns it exists. Narrative is one terminal use case, not the frame;
the same contracts serve barks, game dialogue, or lines for a short film.

The test for what belongs here is one question: *would a project with no dialogue system still want
this?* Text in, sound asset out, with timings — yes. Everything else, no.

---

## The two asset shapes

Both are supported and nothing downstream can tell them apart.

| | |
|---|---|
| **Speech Bank** | many lines in one asset — a scene, a character's barks, a quest. **The default**, because voice-over is bulk: a game has thousands of lines and no review step worth stopping for |
| **Speech Line** | one line in its own asset, for a UI confirmation or a system announcement, where a bank is ceremony around one string |

Both implement `ISpeechLineSource`, and every tool addresses a line as
`FSpeechLineHandle { AssetPath, LineId }`. **That interface is the expansion seam** — a third
container (a dialogue asset read directly by an adapter, a CSV importer, a localisation table) joins
by implementing it, and the subsystem, the cost estimator and the tools never learn it exists.

---

## The one thing to understand

**The script and the direction are separate fields, and they must stay separate.**

`Text` is what the player reads *and* what is spoken. `Direction` is how it should be performed —
`whispers`, `sarcastic`, `exhausted` — and it is merged into the request at the last moment, by the
provider, only if that provider can use it.

Put direction in the text and the subtitle reads the stage direction out loud on screen. The audio is
perfect, the asset is correct, the log is clean, and only somebody looking at the screen ever finds
it. This is the failure mode this plugin is most carefully built against, and it bites twice — see
[the alignment note](#alignment-is-re-based-onto-the-subtitle) for the second time.

---

## Speakers, profiles, and how a voice resolves

The order of work is the screenwriter's: **cast, then write, then produce.** Before a scene, its
speakers are defined; then lines are written carrying a speaker id; then generation and recording
run on top. Two assets carry the first step:

- **`USpeechSpeaker`** — the character sheet: the speaker id lines carry, a display name, casting
  notes, the voice profile they speak in, and an `ExternalBindings` map where adapter plugins
  record what this speaker is elsewhere ("NarrativePro" → an NPC definition). The set of speaker
  assets *is* the cast list — there is no separate mapping table.
- **`USpeechVoiceProfile`** — the instrument: one provider's voice preset, plus model, settings and
  provenance, named for the sound and never for a character. Two speakers can share one, and
  because the profile owns the provider, one project mixes providers freely.

A line does not name a voice. It **resolves** one, walking four sources and taking the first answer:

1. the line's own `VoiceOverride` — the by-hand exception
2. **the speaker's sheet**, matched on `SpeakerId` through the asset registry
3. the container's default profile
4. the project default

Adapters do not compete in this walk — that is a deliberate change from an earlier design that had
a pluggable resolver seam. An adapter **seeds and links** speaker sheets instead (the NP_VoiceOver
harvest creates one per dialogue speaker, with the NPC bound), so a speaker's voice has exactly one
home, and deleting the adapter leaves dormant links rather than re-cast lines.

Every resolution carries a `SourceDescription` saying which step answered, because when a line comes
out in the wrong voice, *why did it resolve to that* is the only useful question and "open four
assets and guess" is not an acceptable answer.

### Resolve first, then hash

The content hash is taken over **the resolved request** — the text as actually sent, the concrete
voice id, the model, the settings, the seed — and never over the reference to a voice asset.

This is the rule the whole multi-source design turns on. Hash the reference instead and repointing a
speaker inside some other plugin changes no hash here, so every line still reports itself current
while being voiced by the wrong character, with nothing anywhere saying so.

It also makes one thing self-correcting: a provider that ignores `Direction` never puts it in the
request, so editing direction correctly leaves lines current on that provider and marks them stale on
one that supports tags. That falls out of doing it in the right order.

---

## Staleness, and why it is computed

A line is **stale** when the hash of what it would send now differs from the hash it was generated
from. It is never stored — storing it would mean invalidating every line whenever any voice asset
anywhere changed, and getting that wrong is silent by construction.

Voice settings are in the hash, so retuning one voice marks its whole cast stale. That is correct and
it is expensive. Three things make it bearable, and none of them is fudging the hash:

- **Nothing regenerates implicitly.** Stale is a priced report; spending is a decision.
- **The report says what changed** — `"stability 0.50 -> 0.35"`, not "the hash differs" — because the
  resolution the audio was made with is stored alongside it.
- **`Accept Current Audio`** re-stamps the hash and records the line as `Accepted` rather than
  `Generated`, for when a tweak does not warrant the money and the re-rolled performances.

**Regeneration is not free even when it is cheap.** These models do not reproduce a take. Measured on
this project: the same line, same voice, same settings, regenerated, came back **2.880s and then
2.400s** — a twenty percent difference in length, never mind delivery. That is why only stale lines
regenerate, ever.

**One line's inputs live in another asset.** A dub is derived from a line in the source-language
bank, and that line can be re-recorded afterwards without anything on the dub moving — same audio,
same hashes, same text. So a dub also stores which recording it was made from
(`DubbedFromAudioHash`), and the status pass compares it against the source line's hash today. This
is the only staleness axis that reads a second asset, and it is cheap because the comparison is
against a field the source already maintains rather than a re-read of its audio. An empty hash on
either side reads as current: a question that cannot be asked is not a fault. See
[LOCALIZATION.md](LOCALIZATION.md).

---

## Graduation is an axis, not a status

Two independent facts about a line, deliberately not collapsed into one enum:

- **Status** — where it is in the pipeline: `Draft`, `Generating`, `Generated`, `Failed`
- **Origin** — where its audio came from: `Generated`, `Accepted`, `Edited`, `Recorded`

The reason is the intersection:

| | Origin `Generated` | Origin `Recorded` |
|---|---|---|
| current | fine | fine |
| **stale** | regenerate for a cent | **the script changed under a recorded line** |

That bottom-right cell is the only report here with real money attached — it scopes a pickup session
exactly — and it would be inexpressible if graduation were a status value, because a graduated line
could not also be stale.

Four mechanisms hold it together:

1. **The generated take is never unlinked.** `Sound` is what the game plays; `GeneratedSound` keeps
   the generated take permanently, including after `Recorded`. A recorded take can be rejected the
   same afternoon, and the generated one is the reference the actor was directed against.
2. **Editing is detected, not declared.** The hash of the imported audio is stored, so a file
   levelled in a DAW is found rather than silently overwritten weeks later.
3. **Regeneration refuses to repoint.** A line whose origin is not `Generated` is skipped — and
   `Force` does **not** override this. Verified: forcing a whole bank regenerated the generated lines
   and left the recorded one untouched.
4. **Nothing is ever deleted.** Not superstition: the production brief that makes graduation cheap
   needs the generated audio as the reference read.

---

## Alignment is re-based onto the subtitle

Every generation uses the timestamped endpoint, which costs the same as plain synthesis and returns
per-character start and end times. Never the plain one.

The timings are attached to the `USoundWave` as a `USpeechAlignmentUserData`, so a facial-animation
pass or a shot-authoring tool can read them **without knowing SpeechForge exists**. Two independent
consumers is why that is asset user data and not just a field on the line.

**The provider aligns against the string it was sent, which has the direction tag on the front.** So
`[exhausted] Holding is not...` comes back with twelve characters of timing corresponding to nothing
on screen — the same bug as §the-one-thing-to-understand, walking in through a side door, and off by
a different amount for every line that carries direction.

SpeechForge trims that prefix and re-bases, so `Alignment.Text` is exactly the subtitle. Absolute
times are kept, so `CharacterStartSeconds[0]` becomes the honest answer to *when does this line start
speaking* — 0.342s in the example above, because the model spends real audio on the tag.

If the returned text is *not* the display text with a prefix, nothing is guessed: the alignment is
flagged `bFromNormalizedText` and a warning says it must not be used to position subtitles.

---

## Providers are a capability tier, read not assumed

SpeechForge ships **no** providers. Each one is a separate plugin that calls
`FSpeechForgeModule::RegisterProvider` at module startup; SpeechForge never learns their names and
deleting one changes nothing. ElevenLabs lives in
[SpeechForgeElevenLabs](../SpeechForgeElevenLabs/README.md) - it was a folder inside this plugin
until 2026-09-01, and extracting it is what removed the last vendor name from the core's defaults.
With no default provider set, the sole registered provider is used; with several registered,
generation asks for one to be named rather than guessing, because providers bill different accounts.

Nothing above the provider line assumes anything. It asks `GetCaps`:

| Capability | Why the pipeline needs it |
|---|---|
| `NativeSampleRate` | 48000 here. **Not a global setting** |
| `bIsMetered`, `BillingUnit` | a free provider must estimate zero, not a lesser wrong number |
| `bSupportsSeed` **and** `bSeedIsBestEffort` | the second is the honest one — a provider that seeds approximately must not be able to claim reproducibility |
| `bSupportsAlignment` | most open models return audio only |
| `bSupportsRemoteHistory` | decides whether a lost take is recoverable or gone |
| `MaxConcurrentRequests` | a property of the account, not the code. Exceeding it **queues rather than errors** |
| `bNeedsCredential`, `SetupHint` | what a human has to do, which no agent can do for them |

**Stitching is asked per model, not per provider** — `SupportsStitchingForModel`. That is not
over-engineering; see below.

---

## Per-model quirks live with the provider

Which model takes inline direction, which one stitches, what a character limit is, which stability
values a model actually accepts - all of it is a provider's private knowledge, asked through
`GetCaps` and `SupportsStitchingForModel` rather than assumed. The measured ElevenLabs answers,
including the trade-off that has no correct project-wide default, are in
[SpeechForgeElevenLabs](../SpeechForgeElevenLabs/README.md).

SpeechForge drops stitching and logs when the model cannot take it, rather than failing the line -
and because stitching is not part of the content hash, dropping it marks nothing stale.

---

## What is verified, and what is not

Verified live on 2026-08-11 against an ElevenLabs Creator account:

- **The whole path**, three lines across two speakers: cast → author → price → generate → import.
- **48 kHz WAV end to end.** And the gate is not where anyone would guess — `wav_44100` and
  `pcm_44100` are refused on Creator with an explicit tier error, `wav_48000` is allowed. See
  [SETUP.md](SETUP.md).
- **Alignment against the engine's own decoded asset: `delta 0.0000s`,** on every line. Two sources
  that share no code — the provider's JSON and `USoundWave::Duration`.
- **Alignment re-based onto the subtitle**: 40 timings for 40 characters, read back off the asset.
- **Cost estimation is exact**: 3 lines, 120 characters, $0.012, computed with no network call.
- **Idempotency**: a second generate reported 0 eligible and 3 skipped, and billed nothing.
- **Skip-if-current**: a batch of 3 with one already current billed 98 characters, not 120.
- **Staleness with a readable reason**, and the stale-plus-recorded pickup report.
- **Graduation survives `Force`**: forcing a whole bank left the recorded line untouched.
- **Errors are passed through verbatim** — the provider's own message named the model and the field.

**Not verified, and honestly so:**

- **Re-fetch from history is written and unproven.** The lookup walks the 100 most recent history
  items to map a request id to a history item id; that has not been exercised against a real one.
- **Concurrency above one in flight has not actually been observed.** The limiter is written and the
  batch reported "up to 5 at a time", but three short lines settle too fast to prove the semaphore.
- **`DetectEditedAudio` has never seen a genuinely hand-edited file.** The comparison reads the
  sound's editor-only payload, which may not be byte-identical to the file that was imported — if
  that is so it will report false positives, and the first real test will say.
- **Stitching has not been heard.** It was rejected by the default model on the first attempt and the
  fix was to stop sending it. Nothing has yet generated a stitched conversation on a model that
  accepts it.
- **Seeds have never been exercised.**
- **Everything measured so far is one provider, one account, one tier, and English.**

---

## Localisation - a sibling bank per language

Localising is three ordinary steps, none of which teaches anything downstream a new word:

1. **Translate.** `LocalizeSpeechBank` turns `SB_Scene` into `SB_Scene_DE` beside it: the same line
   ids, speakers, direction and overrides, only the text translated. A line re-translates when its
   *source* text moves - each localised line carries the hash of the source text it was translated
   from - and `GetSpeechLocalizationStatus` counts current / stale / missing per language from the
   registry. Translation goes through its own provider seam (`ISpeechTranslationProvider`): the
   core ships **Pseudo** (keyless pseudo-localisation, for proving the pipeline), and the
   `SpeechForgeDeepL` add-on registers **DeepL**, whose untranslated `context` field carries "video
   game dialogue" plus the bank description into every request.
2. **Generate.** The localised bank generates like any other - the same speakers resolve the same
   cast voices, the multilingual models speak whatever language the text is in, and the sounds land
   in a per-language folder (`Sounds/DE/SW_<LineId>`), which is what stops German from overwriting
   the English audio that shares its asset name.
3. **Solve faces from the new audio.** A face bank prepared from the localised speech bank solves
   from the localised sounds exactly like any other bank. Give it a per-language output path so its
   baked `AS_Face_<ClipId>` assets do not collide with the originals'.

**Recorded lines can be dubbed instead of re-read.** `DubSpeechLine` on a localised line sends the
source line's recording through the speech provider's dubbing capability (ElevenLabs
`/v1/dubbing`): the actor's voice, pacing and pauses survive into the target language, the line
graduates to Recorded, and - deliberately - it reports **words-unverified**, because the dub's
spoken wording is the dubbing service's own translation, not the subtitle's. Billed by the minute
at a multiple of synthesis, and the key needs the **dubbing permission**. Because a dub keeps
roughly the source's timing, a captured face's video layer stays aligned under the re-solved mouth -
which is the whole reason the face pipeline stores layers instead of flattening
(see `FaceForge/GRADUATION.md` §4).

Localising a translation is refused - translate the authoring bank, or every error compounds.

---

## Where things go

| | |
|---|---|
| `/Game/_Generated/Speech/Banks` | speech banks |
| `/Game/_Generated/Speech/Voices` | voice assets |
| `/Game/_Generated/Speech/Sounds` | imported `USoundWave`s |
| `/Game/_Generated/Speech/Sounds/<LANG>` | a localised bank's sounds, e.g. `DE` |
| `Saved/SpeechForge` | raw provider responses, **kept not deleted** |

Sorted by kind at the point of generation, because a tool that writes everything into one folder
produces something nobody can read after the second run, and tidying it by hand does not hold.

Raw responses are kept because where a seed cannot reproduce a generation, that file and the
provider's own history are the only two copies — and only one of them survives the account closing.

---

## The Speech Library

**Tools > Automation Forge > Speech Library.** Pages behind one switcher, in the order the work
actually happens. The **Ingest** page leads, and exists only when an installed plugin ships an
ingestion method:

- **Ingest** — where the lines come from, and everything about staying in step with it: the
  bank's source and drift status, ingestion methods, and every discovered action tagged onto this
  page. A toolset function tagged
  `meta = (SpeechIngest = "Label", SpeechIngestClass = "/Script/Module.Class")` — two string
  parameters: source asset path, then bank path — becomes a method row: a picker filtered to that
  class (Blueprints are matched by native parent, which is what a Narrative dialogue is) and a
  button. A `SpeechLibraryAction` additionally tagged `SpeechLibraryPage = "Ingest"` moves its
  button here from the Produce bar — NP_VoiceOver's Re-harvest from Dialogue does, sitting beside
  the drift warning it answers. The harvest stamps the bank's source (`SourceAdapter` +
  `SourceAssetPath`, a plain path — no dialogue class is ever linked), so Re-harvest works from
  then on without picking anything, and the picker shows the remembered source across editor
  restarts when the method's `SpeechIngestAdapter` matches the stamp. Ingest is idempotent for
  the same source — line ids are the source's own, so re-ingesting updates lines and keeps paid
  audio. Ingesting a **different** source into a stamped bank asks first: replace (clear, then
  ingest), merge, or cancel.
- **Cast** — who is in the scene, and what they sound like. The cast list (every speaker sheet,
  the current bank's people first, ids without a sheet one click from getting one), the project's
  voice profiles with used-by counts, and the provider browser: fetch the account's voices,
  audition any, then **Save as Profile** to keep one or **Cast** to hand it straight to the
  selected speaker. Re-casting marks the speaker's lines stale; the report says so, nothing
  regenerates by itself.
- **Write** — the lines. Text and direction edit in place (double-click), the speaker cell is a
  dropdown over the cast, and the voice cell names the resolved profile and opens the exception
  flow: voice this one line differently, or clear the override and return to the speaker's voice.
  **Delete Selected** removes lines (multi-select), **Clear All Lines** empties the bank keeping
  its cast, language and source stamp — both confirm first, and neither touches generated audio
  assets: a bank entry is a reference, deleting content is a human's call in the Content Browser.
  (`RemoveSpeechLines` / `ClearSpeechBank` are the same operations as tools.)
- **Produce** — the pipeline table (status, origin, staleness, duration), **Generate Selected**
  with the exact cost beside the button before anything spends, **Generate All** (the whole bank,
  idempotent — missing and stale lines generate, current ones cost nothing, a second press finds
  nothing to do), **Re-generate Selected** (force, behind a costed confirmation that also says
  how many lines are current and re-paid for; recorded, edited and accepted lines are never
  overwritten even by force), **Dub from Source** on a localised bank (the alternative to
  generating a line: carry the source language's performance across instead of re-reading the
  translated text — see [LOCALIZATION.md](LOCALIZATION.md)), and the **discovered actions**:
  any registered toolset function tagged `meta = (SpeechLibraryAction = "Label")` appears in the
  action bar and receives the selection as JSON — installing a plugin adds buttons with no edit
  to SpeechForge, and a third party gets the same seam we use.
- **Perform** — lines become bodies. Appears only when a plugin tags an action onto it
  (`SpeechLibraryPage = "Perform"`; FaceForge's Create/Update Face Bank is the canonical one).
  Top: **To the game** — the delivery buttons, present only when something is installed that has
  somewhere to deliver to. An action may add `SpeechLibraryGroup = "Dialogue"` to land here rather
  than in the recording flow, because the page holds three kinds of verb — build the faces, record
  a performance, hand the finished thing to the game — and an apply button filed under recording
  reads as part of recording. NP_VoiceOver ships *Apply Voice to Dialogue* and *Apply Voice and
  Faces to Dialogue*; neither takes a dialogue path, because the bank's source stamp names one.
  Then: the face banks serving this bank, each one click from the Face Bank panel. Middle: the
  lines with their **Rig** (the speaker sheet's manually settable `RigTarget` — the contract that
  works with no game framework installed; "no rig" is fine for voice, fatal for faces), **Face**
  (the line's clip status in the linked face bank, matched by line id), and **Sessions** (every
  recording session containing the line). Bottom, when PerformanceForge is present: the session
  fixtures, Plan Session/Record Selected, and the sessions covering this bank — found through the
  session asset's registry-searchable `BankPaths` list (a session spans banks when one artist
  records one character across scenes; older unstamped sessions are found the slow way once and
  restamped). Foreign panels are opened by reflection, never by linking.

Audition is everywhere audio is: a play glyph per line and per browsed voice, which becomes a stop
glyph while playing — the playing row tints so it is findable from across the table, a second
click stops it, and the state reverts on its own when the audio ends.

Afterwards the same rows say what each line became: `Recorded` origin for a performed line, and
stale-plus-Recorded when the script later changes under it - the pickup-session report.

## Console commands

There are no buttons on the settings page, and not for want of trying: `UFUNCTION(CallInEditor)` does
not render on a `UDeveloperSettings` page, because the details customization discards archetype
objects and a settings panel edits the CDO. A control that silently does nothing is worse than none.

```
SpeechForge.CredentialStatus     is a key available, and where is it read from
SpeechForge.TestConnection       one cheap authenticated call; reports tier and remaining credits
SpeechForge.ClearKey             forget the stored key
```

---

## Keys, and the machine they live on

`Config/ForgeMachine.json` declares the API keys this plugin wants: what each one
is for, where to get one, the Windows Credential Manager entry it lives in, and
the environment variable consulted when the vault has nothing.

Two surfaces read it. The editor's Keys page — **Tools ▸ Automation Forge ▸
Keys** — and the Automation Forge hub, which is a separate application and can
therefore set a key before an editor is open. They address the same vault entry,
so a key set in either is set for both, for every project on the machine.

The plugin's own settings page still sets the same key, and works with neither of
the other two installed. That is the point of the declaration being data: nothing
here depends on anything else being present.

## Related

- **[SpeechForgeToolset](../SpeechForgeToolset/README.md)** — the same pipeline as MCP tools, plus
  the agent skill. Deleting it changes nothing about how SpeechForge behaves.
- **[SPEECHFORGE_PLAN.md](../../SPEECHFORGE_PLAN.md)** — the design, the reasoning, and the failure
  taxonomy written before any of this was built.
- **[MotionForge](../MotionForge/README.md)** — the same shape, one modality over.
