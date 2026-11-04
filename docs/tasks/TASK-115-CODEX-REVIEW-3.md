# TASK-115 review 3: cancellation fix and remaining blockers

Review of `817c72d`, 2026-10-27. The named-pipe handle now uses overlapped I/O. Contrary to the worker report, the original timeout path still waited at most one second and ignored completion. Codex changed both paths to wait for `GetOverlappedResult` after `CancelIoEx` before releasing the stack OVERLAPPED, event and buffer. This prevents premature release; it does not prove bounded worker termination if cancellation cannot complete.

Independent check: `cargo test --locked --offline -p gt4l-tauri discord_presence` passed 8/8 (Windows; target `O:/gt4recomp-scratch/task133-cargo-target-v2`). Tests include two synthetic Windows pipes; this is not live Discord validation. The real timeout test covers an idle read, not stalled writes or delayed cancellation completion.

Release blockers remain: activity acknowledgement is ignored; connection errors are confused with idle timeout; ping handling is incomplete; stop is signalled by a separately scheduled thread rather than at the action boundary; old-session clear can race a later session; launcher-exit cleanup is detached; fake reconnect and generation tests do not verify the claimed conditions. The UI checks exercise mocks, and gameplay and actual Discord are untested. No installer was produced. Do not treat this checkpoint as feature approval or a ready rc7 build.
