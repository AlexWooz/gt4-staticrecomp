# TASK-133 stage 2: fresh baseline and native RetroAchievements

Date: 2026-10-26. Codex review of7b7eb35 releases this bounded stage only.
Read AGENTS.md, the main TASK133 brief and this file in full.
Workspace C:/Antigravity/gt4-wt-133-rc7-ra-controls; branch task/133-rc7-ra-controls.

## Review disposition

Published rc6 distribution copies and original source are verified. Rust559passed/5ignored
and the13 independently reproduced Python failures are established. CTest46/47 came from a
mutable scratch directory, not a fully traceable fresh build. Its game exe mismatches the
release pin. Do not treat it as the published game or assume every fixture's provenance.
The ninja log proves rebuild actions, not the exact source change causing a specific hash.
Correct the report's root-cause wording to UNKNOWN unless independently established.

## 1. Fresh baseline before product edits

Reserve fresh output on O: (C: was down to about1GB at review); no old slice overwrite,
private junction or old pin changes. Build unchanged task133 program source (exact rc6 tag
58322229c0bd63a0652b0550c5ad2154bd907039) using immutable inputs. Record source, configuration,
toolchain, cache, commands, logs and hashes. Run baseline CTest/full Python with explicit
GT4_NATIVE_BUILD/assets root, proving which source tree CTest executes.
A fresh rebuilt baseline may have a new hash: label it accurately, retaining the verified
published distribution as release reference. Do not copy an exe to fake matching pins.
Only the same13 known Python expectation failures may remain; if new failures or unavailable
inputs occur, stop before integration. No unrelated fixes or weakened tests. Unchanged Rust
checks need not be repeated merely for their old totals.

## 2. Native RA integration only

After the baseline gate succeeds, port released1.2.4/ad46df6 native achievements.*, HTTP,
overlay, fixtures, vendored MIT rcheevos and licenses. Verify vendor provenance. Adapt root/
native CMake and native/window.cpp lifecycle hooks semantically, preserving rc6 pacing,
audio/input, VU/MTVU journal/readback order and performance-mode behavior.
Use gt4::target::active().id / Id::v200, not filenames/environment, as the native target guard.
Spec II must not initialize/evaluate RA memory, issue requests or create overlays even with
forged GT4_RA_* values. Its negative fixture must not force the v200 fixture override.
Preserve cancelable WinHTTP shutdown, disabled/invalid configuration and safe reinitialization.
Use temporary fake credentials and loopback/fake request sinks, never owner credentials/data.
Identify native overlay language requirements for the later launcher phase from actual code;
do not claim untranslated strings complete or invent an existing language interface.
Allowed product edits: native RA/vendor/license files, native fixtures, CMake registration and
native/window.cpp. Progress/report docs allowed. No launcher Rust/JS/UI, controller integration,
version/installer changes, wheel/frame-skip code, CoreGS code or unrelated runtime changes.

## 3. Candidate checks and expected output

Use fresh candidate output separate from baseline/old builds. Build both production targets
affected by shared native code. Any new pin must have a fresh path and measured hashes.
Run native/appropriate Python checks against actual candidate outputs. Record exact totals,
skips, same known13 failures if present, and all new failures; never call a failing suite green.
Required fixture evidence: v200 configured through fake inputs; disabled/invalid config;
Spec II with forged RA environment has no requests/memory evaluation/overlay; genuinely delayed
loopback HTTP cancellation; clean shutdown/reinit; normal behavior retained when RA inactive.
Do not claim real account unlock, physical gameplay or launcher login/logout from native mocks.

Deliver docs/tasks/TASK-133-STAGE-2-REPORT.md, committed source/docs, and local build/test
artifacts with paths, source commits, targets, commands, logs and hashes. Separate fresh
baseline/candidate and measured/expected results. List remaining launcher/localization/control
work. STOP for Codex review. No next stage, full rc7 packaging, PR creation/merge, release
upload (draft included), publication, agent dispatch, game/installer execution or owner-data
mutation. Source commits/pushes for review follow existing owner identity/workflow only.
