# SpeechForge

Every released version of SpeechForge, newest first. A release publishes **one** section of this
file — the one whose heading matches its tag — as its release notes; for an `open` plugin those
notes are posted to Discord `#releases` automatically. Write for someone who installs the plugin,
not for the commit log.

Headings are `## <x.y.z> — <date>`. Use `Added` / `Changed` / `Fixed` / `Compatibility` /
`Known issues`, only the ones that apply.

## 0.2.2 — 2026-09-11

### Added
- **Authoring Language Code** (default `en`). A bank carries no language of its own, and leaving a
  dub's source language to detection — which guesses from a few seconds of audio — dubbed fluent
  nonsense whenever it guessed wrong. Every dub now states its source language explicitly.
- Two seams a localisation add-on can fill: a bank can describe where each line's audio came from,
  and can produce a whole bank its own way. The Speech Library's Origin column and its Generate All
  button defer to them, so a specialised bank behaves correctly in the panel with no change here.

### Fixed
- Speech bank queries now search subclasses. Without that, a specialised bank was invisible to the
  localisation status and to the line-home index — present in the project, absent from the UI.

## 0.2.1 — 2026-09-08
- Packaging fix: the release now carries everything the register allows. `BuildPlugin`'s filter excludes `Config/` and every `public_extra` path, so earlier zips shipped without them.
- `Config/ForgeMachine.json` reaches an installed copy for the first time, so the hub's Keys and Runners pages are no longer empty for it.

## 0.2.0 — 2026-09-07
- Extracted ElevenLabs and Uthana out of the core plugins, so the cores name no vendor
- Speakers, voice profiles, and the three-page Speech Library shipped, then grew to five pages
  (Ingest, Cast, Write, Perform, Localize)
- Casting made human-readable and controllable: audition, discovered actions, a stage
- Sessions and takes: recording stops deciding, the director chooses via a take ledger
- `ConvertSpeechLine` — re-voice a line without rewriting it (speech-to-speech)
- Cost estimation extended to the conversion pipeline ("price the pipeline's other meter")
- Subtitle-worthiness ("words-unverified") asked of every line, not just tool-fixable ones
- Fixed: pressing Use gave a different voice than expected, and billed for it
- Fixed: solving a face bank could overwrite a merged face
- Face bank now links to its speech bank by id instead of guessing
- Tools menu consolidated into one Automation Forge section
- Every Forge asset now has a factory; double-click opens the panel that owns it
- Localisation shipped: a sibling bank per language, joined to everything by line id
- Dubbing shipped: a dub records which recording it was made from
- All plugin links repointed to kovati.dev

## 0.1.1 — 2026-08-29
- Relicensed Apache-2.0; a release now publishes its source
- Keys moved to Editor Preferences and consolidated under ForgeKeys, then again onto the hub
- Every plugin descriptor made to agree with its release tag and credit its author
- Shared-plugin dependency made optional at build time, not just runtime

## 0.1.0 — 2026-08-28
- Initial release: text to a sound asset, with timings
- Runpod provisioning fix after an API change; settings relocated
- Output roots reworked so generated assets move independently of their sources
- Curated editor palette, organised by pipeline step
- Long-running solve wait proven against a multi-minute job
- Cost-before-commit pricing, and single-line banks supported
