# TASK-115 rc7 Discord Rich Presence - Stage Report

Date: 2026-10-27
Branch: task/133-rc7-ra-controls
Worktree: C:/Antigravity/gt4-wt-133-rc7-ra-controls
Base: ef3487d

## Implemented behavior

- Launcher-owned Discord Rich Presence for Gran Turismo 4 and Gran Turismo 4 Spec II.
- Public Discord application id `1564620020796100719`; asset key `gt4-discord-avatar`.
- One persistent on/off toggle in Settings > General: `discord_rich_presence`, default on.
- Presence starts after a verified game process start, using the game identity captured at launch and a real session start timestamp.
- Presence stops on process exit/crash, stop request, disable, or launcher exit; old callbacks cannot resurrect an ended session.
- Re-enable during a running game restores presence using the original session timestamp.
- Discord absent/closed/slow/unreachable never blocks launch or gameplay; bounded retries with backoff.

## Transport and dependency

- Minimal implementation over the Discord local IPC named pipes (`\\.\pipe\discord-ipc-0..9`) using the already available `windows` 0.62 crate (`Win32_Foundation`, `Win32_Storage_FileSystem`, `Win32_System_IO`).
- No new external crate and no credential/OAuth. MIT-compatible.

## Privacy

- Payload contains only the target display name, `GT4Recomp`, the session start timestamp and the asset key.
- No mode, track, car, save, account, path or metric is sent. No messages are sent.

## Validation

- Rust: `cargo test -p gt4l-tauri` (discord presence tests 3/3, flow_audit 10/10) and `cargo test -p gt4l-core` pass.
- UI: focus-check en-US 215/215, pt-BR 222/222, nav-test 209/209.
- Locale key/placeholder parity maintained for the new setting.

## Validation evidence

Commands and counts (serial, worktree root):

- `cargo test -p gt4l-tauri` (discord presence): 6 passed, 0 failed, 0 ignored.
- `cargo test -p gt4l-tauri` (flow_audit): 10 passed, 1 ignored (real discs), 0 failed.
- `cargo test -p gt4l-core`: 215 + 16 + 3 + 5 + 11 + 21 + 5 + 5 + 20 + 21 + 9 + 7 + 0 = 338? (all suites passed, 0 failed).
- UI focus-check en-US: 215/215; pt-BR: 222/222; nav-test: 209/209.
- Second review: `cargo test -p gt4l-tauri discord_presence` = 8 passed (6 manager + 2 Windows pipe), 0 failed. Windows pipe tests use a synthetic named pipe, not discord-ipc-0..9.

Test layers:
- Unit: payload generation, transport trait fake.
- Manager: absent Discord, disable/reenable clear + timestamp, in-flight cancellation, reconnect, generation fence.
- Integration: settings command path and flow_audit.
- UI: focus/navigation in both languages using mock data plus Rust table order check.

## Remaining limitations

- Live Discord client verification is pending owner authorization; mocked tests do not prove real Discord behavior.
- Windows Game Bar / GPU-driver recognition remains future TASK-115 scope; this task does not mark TASK-115 complete.
