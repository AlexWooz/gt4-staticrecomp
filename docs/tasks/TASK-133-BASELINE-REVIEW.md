# TASK-133 stage 1 review: complete the baseline before stage 2

Date: 2026-10-26. Reviewer: Codex orchestrator. Decision: stage 2 is not released yet.

## Confirmed independently

- Reviewed source checkpoint0621d7b is clean and its program blobs equal published rc6/5832222.
  No private directory/junction was found in this worktree at review time.
- Orchestrator reran `cargo test --workspace --locked --offline` in this worktree's launcher,
  with GT4_SDL3_ROOT=C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18.
  Actual total across result blocks:559 passed,0 failed,5 ignored.
- Unit-test blocks: gt4controls16/0/0; gt4l-core213/0/0; gt4l-disc6/0/0;
  gt4l-platform47/0/2; GT4Recomp88/0/1; gt4saves6/0/0 (passed/failed/ignored).
  Those blocks are not the full per-crate totals including integration tests.
- Rust log: C:/Antigravity/gt4-staticrecomp/private/task133-orchestrator-review-20261026/cargo-baseline.log
  SHA256:0c3b22ededd926b4e2c463212a1aedff43d41eae594952e8115bc47a0aaaee61.
- Independently reran Python portable-zip module:9 tests passed.
  Log: same review directory, python-portable.log.

## Corrections to the worker's report/contract

1. Do not repeat the reported crate counts as verified numbers. Reconcile them with actual
   logs, distinguishing unit tests, integration tests, ignored tests and overall totals.
   The independent Rust run passed; this review found inaccurate reporting, not Rust failures.
2. Correct persisted curve identifiers to linear, quadratic, cubic, s_curve, smooth_power,
   sublinear, logarithmic. Do not introduce exponential/aggressive identifiers or silently
   reinterpret existing saved controls. Verify source IDs rather than guessing from labels.
3. Use the existing native target contract, gt4::target::active().id / Id::v200 versus spec2,
   selected by the production build. A filename or environment variable is not a sufficient
   native guard. Test the Spec II negative path without accidentally forcing the v200 fixture
   target. Runtime identity checks already provided elsewhere need not be duplicated by guessing.
4. Describe original-game RA functionality as an expected integration result, not an already
   demonstrated capability of the unchanged rc6 baseline. Target-specific behavior is not built yet.

## Required bounded completion of stage 1

- Produce docs/tasks/TASK-133-BASELINE-REPORT.md with exact source/pin/toolchain/build references,
  commands, log paths, results/skips and the corrected proposed account/target/path contract.
- Complete the baseline CTest and full Python suite required by the main integration brief,
  using a matching rc6 native build and explicit GT4_NATIVE_BUILD/assets-root settings.
  An existing build is usable only with source/pin provenance proving it matches the baseline;
  do not silently test a stable1.2.5 or frame-skip binary. Use fresh outputs if a baseline build
  is needed; never overwrite pinned slices or modify product code to make baseline checks pass.
- The9 portable-zip tests alone do not constitute the complete Python/native baseline.
  If a build/input is unavailable, document the exact blocker and stop rather than inventing
  results or expanding into unrelated fixes. Classify actual failures honestly.
- Attach or reference the independent Rust evidence above; no need to rerun unchanged checks
  solely to reproduce the same numbers. Account for differences in command/profile/environment.
- Final output: committed baseline report, actual native/Python logs and corrected contract,
  then stop for orchestrator review. Do not begin stage2 or change runtime/launcher behavior.

## Scope boundary

TASK133 may perform only scoped integration, LOCAL test builds and its own tests after their
review gates. No PR creation/merge, release upload (including drafts), public publication or
follow-on task. No agent dispatch, owner credentials/settings/saves, installer execution,
CoreGS code or frame-skip/wheel integration. This supersedes any draft-upload wording in the
initial brief. This review document does not authorize stage2.
