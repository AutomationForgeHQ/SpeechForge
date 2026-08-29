# SpeechForge setup

Everything here is a human action. None of it can be done by a tool, and two of the steps are
deliberately impossible for one — see [Why there is no tool for this](#why-there-is-no-tool-for-this).

Ten minutes, once.

---

## 1. Pick the right ElevenLabs product

ElevenLabs sells three things that look like the same subscription at the same price. **Two of them
are wrong for this.**

| Product | Bills | Verdict |
|---|---|---|
| **ElevenCreative** | a shared **credit** pool — speech, sound effects, voice design | **This one.** Credits are the same pool whether generated on the website or through the API, so a Creative subscription already includes API access |
| **ElevenAgents** | **call minutes**, explicitly *not* the shared credit pool | Wrong product. Same price, but it buys real-time conversational agent minutes — workflow builder, RAG, telephony — and no asset generation at all |
| **ElevenAPI** | pay-as-you-go per 1,000 characters | Not a third subscription. It is the usage-billing mode for the same API |

**Creator ($22/month) is the floor.** Not for the credits — pure pay-as-you-go would be *cheaper* per
character — but because the free tier has **no commercial rights at all**, and whether raw
pay-as-you-go without a subscription carries them is not stated anywhere. For a game that ships, that
is not a question to be relaxed about.

Creator gives roughly 131,000 credits a month. At 1 credit per character on the quality models, that
is about **1,200 lines**, or 2,400 on the fast models at half a credit each.

### You probably do not need Pro

The docs say 44.1 kHz output is gated to Pro ($99). True, and the conclusion everyone draws from it is
wrong. Probed directly against a live Creator account:

| Requested | Result |
|---|---|
| `wav_48000` | **allowed** |
| `wav_32000` / `wav_24000` / `wav_22050` | allowed |
| `wav_44100` | **refused** — *"only available on the Pro tier and above"* |
| `pcm_44100` | refused, same |

**44.1 kHz is gated and 48 kHz is not.** No document says this. 48 kHz is what game audio engines
want anyway, so Creator already reaches the best practical format and the Pro upgrade buys this
pipeline nothing. `RequestedSampleRate` defaults to 48000 for that reason.

---

## 2. Create an API key with only what it needs

Developers → API Keys → create, with **restricted access**. These are the only endpoints SpeechForge
calls:

| What SpeechForge calls | Permission to enable |
|---|---|
| `POST /v1/text-to-speech/{voice}/with-timestamps` | **Text to Speech** |
| `GET /v1/voices` | **Voices — read only** |
| `GET /v1/models` | **Models — read** |
| `GET /v1/user`, `/v1/user/subscription` | **User — read** |
| `GET /v1/history`, `/v1/history/{id}/audio` | **History — read** |

**Do not grant Voices *write*.** This is a design decision, not an omission. `ISpeechProvider` has no
`CreateVoice` and no `DeleteVoice`, because a key that can create a voice can generally delete one —
and deleting a voice loses every line it ever spoke, unrecoverably. Cast voices in the web interface
and paste the id.

**Not needed:** Dubbing, Studio/Projects, Sound Effects, Music, Speech-to-Text, Agents, Workspace
admin, Billing, Pronunciation dictionaries.

**Set a monthly credit quota on the key.** It is a hard ceiling that no bug in this plugin can exceed,
and it is the one protection that does not depend on the cost estimator being right.

**Skip IP allowlisting.** Dev machines and dynamic addresses will cost you an afternoon.

---

## 3. Give the key to the editor

**Project Settings → Plugins → SpeechForge → API Key.** Paste and commit the field.

The value goes straight into the OS credential vault and the field blanks itself. It is `Transient`
and carries no `config` specifier, so it never reaches an ini — what persists is a vault entry
outside the project directory, which therefore cannot be copied, committed, or zipped along with the
project.

Check it took:

```
SpeechForge.CredentialStatus
SpeechForge.TestConnection      → "Connected. Tier 'creator', N of 131000 credits used…"
```

For CI or a headless run, set `SPEECHFORGE_ELEVENLABS_KEY` instead. The environment is checked before
the vault, so it wins without anything being configured.

> Windows has a real vault backend. macOS and Linux are environment-variable only until a
> Keychain/libsecret backend is written — which still keeps the key out of the project folder.

---

## 4. Cast a voice — and not a stock one

`List Provider Voices` shows what the account holds. A fresh account holds only **premade** voices,
and every one of them is flagged `bIsPremade` for a reason:

**Stock voices belong to the provider and can be retired.** ElevenLabs has announced exactly that.
A VO library built on one dies with it, and there is no migration — the takes are simply gone.

So before authoring anything real, use ElevenLabs' own interface to either:

- **Design a voice** from a written description — recreatable later if you keep the prompt, or
- **Clone a voice** from samples — Creator includes both instant and professional cloning, with 30
  voice slots and 1 professional clone.

Then create the pairing in the editor, recording honestly how the voice was made:

```
Create Speech Voice
  SpeakerId        Alexis
  ProviderVoiceId  <from List Provider Voices>
  Provenance       Designed | Cloned      ← not Unknown, and not Premade if you can help it
```

Provenance is not bookkeeping. It is what makes it possible to answer, three thousand lines later,
*which of our voices can we recreate?*

Premade is fine for prototyping and SpeechForge will warn you once per voice, in the log, at the
moment it is still cheap to change your mind.

---

## 5. First bank

```
Create Or Update Speech Bank
  BankName          SB_AirlockScene
  DefaultSpeakerId  Alexis
  Lines
    LineId ALX_001   Text "Seal it. Now."   Direction "urgent"
```

Then **price it before generating**:

```
Estimate Speech Cost   → lineCount, billedCharacters, estimatedCost
Generate Speech
```

`CostPerThousandCharacters` in Project Settings turns characters into money. It defaults to 0.10,
which is the quality-model rate; the fast models are half that. Nothing can detect this — only the
account knows.

---

## Why there is no tool for this

Two operations here are deliberately unavailable to an agent, and both for the same reason: they are
the ones where speed converts a small mistake into an irreversible one.

**Nothing can write a credential.** An agent that can put secrets into the OS vault is a liability
with no matching benefit, and signing in is a human action performed once.

**Nothing can create or delete a voice on the provider.** Casting is a human decision, and the
permission that would allow creating a voice generally allows deleting one — which takes every line
that voice ever spoke with it.

Everything else is a tool, on purpose. If a capability only exists behind a button, an agent cannot
use it, and a pipeline that needs a human to click things is not a pipeline.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `No API key` | Nothing in the vault or the environment. Step 3 |
| `Output format 'wav_44100' is only available on the Pro tier` | `RequestedSampleRate` was set to 44100. Use 48000 — it is allowed and better |
| `Providing previous_text is not yet supported with the 'eleven_v3' model` | Should no longer happen; stitching is now asked per model. If it does, the model list in the provider needs updating |
| A 422 mentioning `stability` | Some models take stability only at discrete steps. The provider snaps it; a 422 means a model it does not know about |
| Nothing generates and nothing errors | Every line was current, in flight, or graduated. Check `Get Speech Line Status` — that is the pipeline working |
| Batch runs but is very slow | Concurrency exceeded queues rather than erroring. Check `MaxConcurrentRequests` against the account tier |
