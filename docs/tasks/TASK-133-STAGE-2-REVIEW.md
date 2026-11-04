# TASK-133 stage 2 review: correct the positive fixture in Spec II builds

Date: 2026-10-26. Reviewed commit2a7d5f2. Stage3 is not released yet.

## Verified independently

- Scope is native-only; no launcher/controller/frame-skip/wheel/CoreGS product changes.
- All88 vendored rcheevos files match the reviewed1.2.4 Git blobs.
- Fresh baseline game SHA188fbbf2..., candidatev2005baa879e... and candidateSpecII4eacbe9d...
  match the full hashes in the stage2 report. All four baseline/candidate test-log hashes match.
- Logs record baseline46/47CTest and candidate49/50CTest, with Python710tests/13failures/
  42skips. The failed Python test-name sets match the known baseline set.
- Independently reran the three RA CTests in the v200 candidate:3/3passed.
- Native init/tick/memory-read guards check the actual target. HTTP/overlay production code
  otherwise retains the stable1.2.4 implementation, with the runtime user-agent updated.

## Reproduced blocker: Spec II CTest configuration is inconsistent

The standard achievements_fixture links gt4_achievements. In a GT4_TARGET=spec2 build,
that library is correctly compiled with GT4_TARGET_SPEC2. But the fixture still expects RA
activation in test_clean_shutdown_and_reinit, contradicting the production Spec II guard.

The reviewer built achievements_fixture in the existing Spec II candidate configuration and ran:

    ctest --test-dir O:/gt4recomp-scratch/slice332-rc7-ra-spec2/build -R "^native_achievements$" --output-on-failure

Result:FAILED, native/achievements_fixture.cpp:344, "Runtime should activate with valid configuration".
The guard's rejection messages precede the assertion. This is a fixture/wiring defect, not
evidence that production Spec II achievements should activate.

Build log: C:/Antigravity/gt4-staticrecomp/private/task133-orchestrator-review-20261026/spec2-positive-fixture-build.log.
CTest log: O:/gt4recomp-scratch/slice332-rc7-ra-spec2/build/Testing/Temporary/LastTest.log.
The Spec II fixture executable did not exist before the reviewer built it; the worker had
reported full CTest only for v200. Do not describe this as full testing of both configurations.

## Bounded correction

1. Fix only native/root CMake fixture wiring and the RA fixture code if necessary. Preserve
   production Spec II's strict guard. Never force GT4_TARGET_FIXTURE_V200 into production
   libraries/executables or into the Spec II negative fixture just to make tests pass.
2. Keep actual positive v200 coverage and actual negative Spec II coverage in the test design.
   A separate test-only v200 compilation of the RA code is one valid approach. Changing the
   fixture's header macro alone while linking the Spec II production library is insufficient.
   Do not remove assertions, skip the failing test indiscriminately or weaken target isolation.
3. Rebuild affected fixture targets in BOTH candidate configurations, run all three RA tests
   in both, and run the full candidate CTest configurations after building their registered
   fixtures. Compare Python failures with the known13 names; no new failures or missing tests.
   Check production executable hashes afterward; if unchanged report that, otherwise record
   fresh measured hashes and explain why. No old pin edits or old-output substitutions.
4. Update the stage2 report with the actual two-configuration evidence. Qualify its broader
   claims: inactive hook/no-op tests are not gameplay/visual/performance measurements; clean
   lifecycle tests without leak instrumentation do not prove absence of all leaks. Native
   overlay localization remains pending, as already documented.
5. Keep future-step descriptions subordinate to the owner boundary: Cargo stays numeric1.3.0
   with a separate rc7 label later, and no release upload/draft/publication is authorized.

## Expected result and stop

Deliver one correction commit, updated report, exact commands/logs/hashes and results for both
configurations. Runtime Spec II remains disabled, positive RA tests remain effective, and no
new failures appear beyond the recorded Python debt. Then STOP for Codex review.
Do not start launcher/UI/controllers/packaging or another task. No PR/merge/upload/release,
agent dispatch, game/installer launch, owner-data access or changes to other worktrees.
