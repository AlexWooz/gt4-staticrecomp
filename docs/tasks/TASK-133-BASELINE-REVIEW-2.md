# TASK-133 stage 1 review 2: resolve native artifact provenance

Date: 2026-10-26. Reviewed worker report21afc52. Decision: stage2 remains on hold.

## Accepted evidence

- Only a documentation file was added. Program blobs still equal published rc6/5832222.
- Both supplied log hashes match the files: CTest095e3a03e723ef73db2fe11c8496a946af35fde206e9cad241fed5910488fe7f,
  Pythonab638172071273f1be430b8c423103306493dd3c540dd43b447dda4e29393d59.
- Logs record CTest46/47 and Python710 total,13failures,42skips. The orchestrator independently
  reran test_gs_settings (72 tests,12failures) and test_gs_live (14 tests,1failure,2skips),
  reproducing those same baseline failures without any integration code changes.
- Rust559passed/5ignored remains supported by the earlier independent complete run.
- Corrected curve IDs and compile-time native target guard are the right proposed contract.

## Blocking discrepancy: the tested build executable does not match the claimed pin

Independent SHA256 at review time:

| Artifact | Actual SHA256 | Pin result |
| --- | --- | --- |
| O:/gt4recomp-scratch/slice300-rc6-v200/build/gt4vulkan.exe | 3e15166ade27309b90f62be91477dcae532b2ffbde2eceb1b0591055f29e1503 | Does NOT match slice303's6682777e... or slice300's15100642... |
| C:/Antigravity/gt4-staticrecomp/private/slice306-dist-130-rc6b/gt4.exe | 6682777e1a6c141d3f32aa54265d609a4926a55f0c19c45d84c51a2e39970945 | Matches published rc6 slice303 |
| O:/gt4recomp-scratch/slice301-rc6-spec2/build/gt4vulkan-spec2.exe | e841615236ec9107e0d0580d877ebc5ffa3afae9644aed1196f12a58f89df173 | Matches slice304 |
| C:/Antigravity/gt4-staticrecomp/private/slice306-dist-130-rc6b/gt4-spec2.exe | e841615236ec9107e0d0580d877ebc5ffa3afae9644aed1196f12a58f89df173 | Matches slice304 |

The published runtime copies are intact. This discrepancy does not prove the code is broken,
but the report's assertion that the slice300 build executable matches slice303 is false.
The cause of the build-directory drift is UNKNOWN. No file was replaced during this review.

CMakeCache names C:/Antigravity/gt4-wt-116-117-perf as the build source, not the task133
worktree. Its checked branch5f0d3d6 differs from publishedrc6 only in TASKS/journal, and its
working tree was clean. This supports source equivalence but does not establish which source
and configuration produced every existing fixture/executable in the mutable build directory.
CTest's embedded Python command also points to the116-117worktree; document this distinction.

## Bounded corrections required

1. Correct the report with actual file paths, measured hashes and VERIFIED/UNKNOWN labels.
   Use the verified distribution copies as the published-runtime reference. Do not change old
   pins, overwrite old build/distribution files, or copy a matching exe over the mismatched one.
2. Establish provenance of the native fixture build separately from the published game binary.
   Cite actual build/configuration records and source identity. If that cannot be established,
   prepare a fresh rc6 baseline build from the task133 source with fresh outputs, immutable
   inputs and full logs, then run its native baseline checks. A fresh rebuild may have a new
   binary hash; report it as a rebuilt baseline, not the published artifact. No integration
   or unrelated behavior fixes during this step. Stop with a precise blocker if inputs fail.
3. Correct toolchain provenance. Current measurements: clang-cl22.1.3, ninja1.13.2,
   cargo1.99.0(5f94df4782026-08-27). CMake's recorded compiler version is22.1.3.
   Do not present LLVM19.x/Ninja1.12.1 as measured versions; distinguish build-time evidence
   from tools installed today. Replace unmeasured values with UNKNOWN.
4. Make the Rust integration-block table complete or label it as examples. The listed blocks
   total138, while the full integration count is183; do not claim that the partial list is complete.
5. End with one corrected provenance/baseline report and supporting logs, then stop. The13
   existing Python failures remain recorded debt, not an all-green result. Do not weaken or
   change expectations in this provenance-only step. No need to repeat unchanged Rust/portable
   checks just to reproduce prior evidence. Stage2 still needs orchestrator approval.

Reviewer outputs are in C:/Antigravity/gt4-staticrecomp/private/task133-orchestrator-review-20261026/:
python-settings-baseline.log and python-live-baseline.log. Review changed no runtime code.
The existing no-PR/no-upload/no-release/no-next-task boundary remains in effect.
