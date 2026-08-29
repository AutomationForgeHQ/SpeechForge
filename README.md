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
plugin to one project's way of working. `NP_VoiceOver` is the planned adapter that binds this to
Narrative Pro's dialogue system; it is a separate plugin and does not exist yet.

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

## Voices resolve, they are not named

A line does not name a voice. It **resolves** one, walking four sources and taking the first answer:

1. the line's own `VoiceOverride`
2. **registered external sources**, highest priority first
3. the container's default voice
4. the project default

Step 2 is the point. An adapter plugin implements `ISpeechVoiceSource`, registers at module startup,
and maps a speaker onto a voice from wherever it likes — an NPC definition, a casting table, a
spreadsheet. SpeechForge never learns what it reads from, and deleting it changes nothing except
which voices resolve.

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

`ElevenLabs` ships in this plugin as the first provider, the same way MotionForge ships Uthana.
Additional providers are separate plugins that call `FSpeechForgeModule::RegisterProvider` at module
startup; SpeechForge never learns they exist and deleting one changes nothing.

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

## The model trade-off you cannot default correctly

Measured on a live account, not read:

| | `eleven_v3` | `eleven_multilingual_v2` |
|---|---|---|
| Inline direction (audio tags) | **yes** | no |
| Stitching (`previous_text`) | **rejected**, `unsupported_model` | yes |
| Character limit | 5,000 | 10,000 |
| Stability values | **0.0, 0.5 or 1.0 only** — anything between is a 422 | continuous |

So the two things this pipeline most wants — direction, and prosody that carries across a
conversation — are on **different models**. There is no correct project-wide default; it is a real
per-bank decision. `eleven_v3` is the shipped default because direction is what makes the Direction
field worth having, and a bank that cares more about continuity should be moved to
`eleven_multilingual_v2`.

SpeechForge drops stitching and logs when the model cannot take it, rather than failing the line —
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

## Where things go

| | |
|---|---|
| `/Game/_Generated/Speech/Banks` | speech banks |
| `/Game/_Generated/Speech/Voices` | voice assets |
| `/Game/_Generated/Speech/Sounds` | imported `USoundWave`s |
| `Saved/SpeechForge` | raw provider responses, **kept not deleted** |

Sorted by kind at the point of generation, because a tool that writes everything into one folder
produces something nobody can read after the second run, and tidying it by hand does not hold.

Raw responses are kept because where a seed cannot reproduce a generation, that file and the
provider's own history are the only two copies — and only one of them survives the account closing.

---

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

## Related

- **[SpeechForgeToolset](../SpeechForgeToolset/README.md)** — the same pipeline as MCP tools, plus
  the agent skill. Deleting it changes nothing about how SpeechForge behaves.
- **[SPEECHFORGE_PLAN.md](../../SPEECHFORGE_PLAN.md)** — the design, the reasoning, and the failure
  taxonomy written before any of this was built.
- **[MotionForge](../MotionForge/README.md)** — the same shape, one modality over.
