# TASK-133 stage 3 review 3: finish account-race evidence

Date: 2026-10-27. Workspace C:/Antigravity/gt4-wt-133-rc7-ra-controls; review base 56ae641. Read the stage 3 brief and reviews 1 and 2. Complete only the remaining items below, commit and push, then stop. No controls, packaging, PRs, releases, real accounts, game launch or cleanup.

## Independent review

The focused `retroachievements_network_race_tests_with_real_tcp_server` passed independently. However, its two refresh scenarios use `ra_status_with` and a sleeping closure, not the TCP server, contrary to the user-facing description that all scenarios used real network. Login scenarios use real TCP but rely on 20/80 ms sleeps rather than confirming request arrival before mutation; this is susceptible to scheduling races. Fake `mode_fail` returns HTTP 200 with a JSON error, not HTTP 401. The failed-login scenario asserts the error but does not assert the persisted account remains unchanged. `ra_status_at` swallows network errors using `.ok()`, so do not claim abort error propagation without proof.

The committed `cargo-test-workspace.log` contains 34 `test result` blocks totaling 574 passed and 5 ignored, not 569. Inspect and report the actual total and test command; never adjust logs or counts to match claims. No assumption of an unrecorded run.

## Required work

1. Replace timing-only coordination in login and refresh concurrency tests with bounded channels/barriers: confirm the fake server has received the exact request and hold its response until logout, newer login or target switch has actually completed. Exercise the real `ra_login_at` and `ra_status_at` network paths for login versus logout, login versus newer login, refresh versus logout and refresh versus target switch. Assert both returned state and persisted credentials/score/game view as applicable. Use fake data; ensure the server cannot hang tests indefinitely.
2. Verify failed login and interrupted connection preserve previous persisted state, and accurately report whether status returns an error or a view. Distinguish HTTP 200 service errors from genuine HTTP 401; test and report whichever cases are actually executed. Do not claim network cancellation where none exists.
3. Reconcile the Cargo totals from all logged blocks. Update the report's remaining overstatements, including actual test methodology. Preserve uncertainty about the prior full CTest timeouts and audio/ring failures; no claim of full green or gameplay acceptance.
4. Repeat focused tests and relevant Cargo checks serially; retain exact commands and logs with genuine counts, errors and skips. Do not alter unrelated tests to obtain a pass. Provide a short report of what was proven and what remains untested.

Stop after commit/push for Codex review. Do not proceed to stage 4.
