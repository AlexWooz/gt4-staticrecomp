# TASK-133 stage 3 review and correction gate

Date: 2026-10-27. Workspace: C:/Antigravity/gt4-wt-133-rc7-ra-controls. Review base: 2766152.

Stage 3 is not accepted yet. Correct the items below in this worktree, then stop for independent review. Read AGENTS.md and TASK-133-STAGE-3-LAUNCHER-RA.md. The latter's boundaries and evidence requirements remain in force. Do not begin stage 4, packaging, PRs or releases. Use fake accounts and isolated temporary data only.

## Findings

1. The four committed UI screenshots do not establish the claims in section 5.3 of the report. `achievements-en-us-spec2.png` is nearly blank; `achievements-pt-br-gt4.png` is missing text; `achievements-en-us-gt4.png` shows a signed-out account; and `achievements-pt-br-spec2.png` shows a signed-out GT4 page without the unsupported banner. Reproduce and fix the capture workflow and any real defects, without assuming that a screenshot defect proves the application itself is broken. Capture correctly rendered, fully loaded previews for GT4 signed-in and signed-out and Spec II unsupported, in both languages, using only synthetic data. Verify target and language in the rendered view; capture JS errors and exact reproducible commands.
2. The report describes the Spec II full CTest failure as the same known Python debt, but its own section 6.2 says the test timed out at 120 seconds. A timeout does not establish identical failures. Run and retain a complete Spec II Python suite against the candidate build, compare individual failures and skips to the baseline, and report the CTest timeout independently. Diagnose any extra failures or timeouts; do not weaken tests.
3. The stage 3 report lacks exact execution commands and preserved per-run logs for Cargo, CTest, UI and Python. Supply durable log paths, hashes, actual counts and test environment. Do not invent missing evidence; rerun where required. The original stage brief called for fresh native build outputs, but slices 331 and 332 were reused; document that limitation and produce fresh outputs for both targets as required, keeping existing outputs intact. Verify both production binaries and affected native fixtures.
4. The tests in flow_audit inspect `PlayInputs.extra`; the core tests separately check `removed_inherited`. Add an end-to-end test of the actual launch-plan composition with hostile inherited RA values for both games, including case variations on Windows, and demonstrate removals plus final effective values without launching the game or printing secrets. Inspect and cover any diagnostic/log path that can expose the token, including debug representations of launch plans.
5. Provide evidence for all requested fake-server delayed login, logout, refresh and target-switch races, failed/cancelled calls, installed and portable 1.2.5-compatible config, and diagnostic export exclusions. Existing closure-based race tests and a mocked UI are not proof of all these cases. Explicitly identify coverage gaps and correct them.
6. Do not relax unrelated pre-existing assertions to obtain a passing UI check. The stage 3 change to the General settings order assertion accepts an extra `launcher_gpu` row; establish whether it is expected on this baseline and justify with evidence, or restore the original assertion and fix the underlying cause.
7. Correct cleanup reporting: the receipt reports 3,833,790,678 bytes in 7,400 files, while the report claims 4,115,225,584 bytes. The receipt attributes 8,417,783,808 bytes net free-space increase to the interval, which exceeds the copied source size. Identify what is actually established by the receipt and what other activity might have affected free space; do not claim that moving this cache alone caused the entire increase. No further cleanup is authorized here.

## Reviewer checks already made

HEAD 2766152 was clean at review. Both reported native game hashes matched their files; the three native achievements CTests passed again in each configuration (6/6). The relocation receipt reports verified identical and complete, the source path is absent and destination exists; this does not independently reproduce the historical copy verification. No gameplay or live RA account was tested.

## Deliverable

Update the stage 3 report honestly, retain reproducible evidence and logs, commit and push the focused corrections under the owner identity, report the exact commit and remaining limitations, then STOP for Codex review. Do not claim completion of the rc7 or begin subsequent work.
