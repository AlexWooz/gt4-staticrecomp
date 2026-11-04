# TASK-133 stage 3: RetroAchievements in the modern launcher

Date: 2026-10-26. Stage2 correction9b814c0 accepted for this bounded continuation.
Workspace C:/Antigravity/gt4-wt-133-rc7-ra-controls, branch task/133-rc7-ra-controls.
Read AGENTS.md, the main integration brief, stage2 report and this file in full.

## Accepted review evidence and limits

Codex independently repeated all3 RA tests in BOTH configurations (6/6passed), checked
reported log/game hashes, and inspected generated production compile/link rules. Production
Spec II links gt4_achievements without GT4_TARGET_FIXTURE_V200 or test-only RA libraries.
v200 game SHA1c6fdca28c4e2b11f17b5e6615817fc39b44fff3f94e37de6c712c848a811f84;
SpecII SHA9c61cfd27909c756a076ac40c4053ad5cd3c825a64309cce8e2533df99d91d13.
Full CTest logs report49/50 in each configuration, only the known Python contract failure.
Python still has13knownfailures,42skips forv200 (SpecII CTest Python reports55skips).
This is not an all-green release or gameplay/hardware/real-account acceptance.

## Starting inputs and ownership

Preserve the accepted native target/cancellation/fixture behavior. Port launcher RA semantics
from released1.2.4/ad46df6, not its legacy files wholesale. The current modern launcher remains
the rc6 architecture: two games, per-game data/saves, installer/portable, version picker,
channels, language settings and performance behavior. No control-curve port in this stage.
No agents, other worktree edits, game/installer launches, owner config/credentials/saves,
release uploads/drafts/publication, PR creation/merge or follow-on work are authorized.

## 1. Account model, storage and modern launch contract

- Port the reviewed retroachievements.rs model/API and required UserDirs/DataDirs methods.
  Keep the separate retroachievements.toml file compatible with1.2.4/1.2.5; do not invent a
  new settings section or credential format. Resolve installed/portable roots through existing
  UserDirs/BuildKind APIs, using only temporary directories/fake credentials in tests.
- Account settings are shared. Resolve the badge/cache path explicitly under original GT4's
  modern game-data layout. Switching games must not erase credentials or shared preferences.
- Adapt commands.rs prepare_play/PlayInputs, state.rs, commands.list, main.rs and capabilities
  together. Use the current environment-scrubbing path, then add RA configuration for games::GT4.
  For Spec II force RA off after inherited GT4_RA_* values are removed; do not send its process
  credentials, original-game hash or enabled RA options. Native target guard stays intact.
- Retain the1.2.4 account revision/stale response protections. Delayed refresh/login cannot
  overwrite logout/newer credentials or a newer selected-game UI. Keep network work outside
  state locks/UI blocking paths, using the existing async conventions.
- Do not put passwords/tokens in logs, debug export bundles, console output or process argv.
  Reuse the existing persistence contract; do not claim encryption or secure-vault storage that
  the code does not implement. No live RetroAchievements account/service calls for testing.

## 2. Localized modern account/achievement UI

- Integrate the stable achievements page/tab and necessary models/dev mocks into the modern
  modular settings UI. Preserve navigation, focus, language switching and current layout.
- Display account/login/logout and modes for original GT4, using existing reviewed semantics.
  Spec II clearly shows unsupported/disabled achievements and never presents the original
  game's achievement set as Spec II's. Shared account management may remain available, but
  opening the Spec II view must not fetch/load an original-game overview for that target.
- Add all owned user-facing strings to en-US.json and pt-BR.json with equal keys/placeholders.
  Do not copy hardcoded legacy UI text or remove modern settings/pages. Never log credentials
  in development request recordings/screenshots. Only obviously fake accounts in fixtures.

## 3. Native overlay language bridge (only native change allowed here)

Define one explicit new interface: GT4_RA_LANGUAGE=en-US|pt-BR, sent by the launcher using
the existing resolved language (state::language_of / Language). Native configuration accepts
those two values and falls back to en-US for missing/unknown values. This is a new contract,
not an interface already present inrc6. The launcher scrubs inherited values before setting it.

Translate application-owned native toast headings/messages with a small internal catalog.
Keep service-provided achievement titles/descriptions/usernames as received; do not fabricate
translations of service content or alter achievement data. Preserve formatting placeholders,
numbers and fallback behavior. No arbitrary dictionaries/paths from untrusted environment.
Only native achievements/overlay configuration/string helpers and focused fixtures may change;
do not change target isolation, memory interpretation, HTTP semantics, pacing or game logic.

## 4. Required evidence

- Rust workspace/contracts, recording actual counts/skips. Add delayed-request fake-server
  controls for logout/refresh/login/target-switch races and failing/cancelled account calls.
- Tests of1.2.5-compatible RA config loading in temporary installed and portable layouts,
  cache location, target switching, both launch plans and hostile inherited RA environment.
- Launcher SpecII plan contains no credentials/original-game RA hash and keeps RA disabled;
  native negative guard tests still pass in both configurations.
- en-US/pt-BR key/placeholder parity plus focus/navigation tests for both languages, original
  and Spec II views, signed-out/signed-in/error states; no JS errors. Capture local previews
  with fake data to show account state, unsupported Spec II and both languages.
- Native language parser/catalog tests for both locales and unknown/missing fallback. Rebuild
  changed native targets in fresh outputs and repeat affected tests for both configurations.
  Report changed hashes; never repin an old manifest to accept changed binaries.
- No changes to controls or wheel/frame-skip code. Known13Python failures remain named debt,
  not permission to hide new failures. Test compatibility of diagnostic exports with RA secrets.

## Expected final output and stop

Deliver docs/tasks/TASK-133-STAGE-3-REPORT.md plus committed integration and local evidence:
exact source/build references, commands, logs, hashes, UI preview paths, results/skips/failures
and limitations. Describe account persistence/cache/language contracts and retained safeguards.
This is an integrated RA test slice, not a packaged rc7 or proof of real achievement unlock.

STOP for Codex review. Controller curves, final version labels/notes/installer/portable kit
are later stages. Do not create a PR, upload any release/draft, publish, run owner gameplay,
or continue automatically. Source commits/pushes for review use the existing owner identity.
