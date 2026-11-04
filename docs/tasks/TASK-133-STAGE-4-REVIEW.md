# TASK-133 stage 4 review: close controller parity evidence

Date 2026-10-27. Worktree C:/Antigravity/gt4-wt-133-rc7-ra-controls. Review base 990eaca. Read AGENTS.md and TASK-133-STAGE-4-CONTROLLERS.md. Remain in this worktree; no packaging, PR, release, real-game launch, physical-device testing, other worktree changes or subsequent stages.

## Independent checks

Codex verified the two game SHA256 hashes and repeated native_controls, native_audio_input and all three RA tests for each target: 10/10 passed. Reconciled Cargo log: 578 passed, 5 ignored in 34 blocks. Full CTest logs show 49/50 in both targets: 13 Python failures, 42 skipped v200 and 55 skipped Spec II. Those named failures concern gs_live and gs_settings as established in the stage 3 baseline, not historical missing slice executables or aspect-profile checks as claimed in stage 4 report section 6. Fix this attribution using actual failure names and evidence; report no unsupported causes.

## Required work

1. The plan calls for demonstrable native/launcher curve parity. Existing native test verifies a zero-deadzone middle vector, endpoints and monotonicity, while UI checks use four zero-deadzone middle vectors; the Rust crate stores curve identifiers but does not implement the shaping math. Correct the report's claim of bit-exact C++/Rust/web math and its table accordingly. Add a meaningful comparison between actual C++ native output and JavaScript preview for all seven curves across full trigger byte range and representative nonzero deadzones, including defaults and boundaries. Use shared vectors or a reproducible comparison against fixture outputs, not duplicated unverified expected formulas. Report any rounding differences and fix actual discrepancies if found. Test analog stick signed endpoints and neutral using native code; do not claim Rust computes formulas if it only serializes selections.
2. Preserve the 1.2.5 semantics and original user configs, target separation, RA behavior, and all test assertions. Provide exact commands, retained logs, source/build references and honest limitations. Confirm physical controllers were not tested; SDL virtual devices demonstrate synthetic input only.
3. Repeat affected checks in both target builds, plus the comparison and locale/UI tests as applicable. If code changes affect native builds, produce fresh candidate outputs without overwriting pinned or prior slices, and report hashes. If only tests/docs change use existing candidates where appropriate. Document 13 Python failures accurately; do not weaken tests.

Deliver updated stage 4 report and evidence, commit and push under owner identity, then stop for Codex review. No next stage.
