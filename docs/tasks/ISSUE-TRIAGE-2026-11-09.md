# Open-issue triage, 2026-11-09

Scope: the open issues on `moesuito/gt4-staticrecomp` after the 1.3.0-rc16 test build. The owner asked for a first
reply to everyone (done, 21 replies) and a plan to fix the cluster later. This document is the plan; it assigns no task
numbers yet and promises no dates.

## How to read this

- **P0**: crash, data loss, or a blocked game path. Needs a log and a reproduction first.
- **P1**: performance or input problem that makes the game unpleasant.
- **P2**: visual/UX defect or a small documentation fix.
- **P3**: feature request or a known limitation to record, not schedule.

Every report is asked for the same minimum: launcher version, Windows version, GPU + driver, renderer (GSdx or
CoreGS 2.0), render scale, and the zip from **Settings > About > Export logs**. Where relevant, the reply also asks for
`logs\shaders.log` and an A/B with the other renderer. No fix is claimed without a reproduction on the current rc16.

## The open set

The repository has **35 open issues** on 2026-11-09. The table covers the **30 most recent**; the five older ones
(#88 wheel/FFB, #92 split-screen 4:3, #97 low-end performance, #107 startup failure, #112 integrated-graphics slowdown)
already have owner replies and sit in the wheel/aspect/performance backlog. No new reply is needed there.

| # | Title (short) | Kind | Priority | Current state / next step |
|---|---|---|---|---|
| 222 | 30 fps menus and 1080i | request | P2 | 30 fps menus are not a supported path; the 1080i row is being removed because the recomp is fixed at 480p. Record and close as not planned, or keep open if the user reports a real menu slowdown with logs. |
| 221 | Sun slowdown | performance | P1 | Known flare readback. GSdx unsynchronized is the workaround; the native renderer path should remove the readback. Needs a measured A/B and a GPU/renderer log. |
| 220 | Analog steering sensitivity | input | P1 | The launcher has stick deadzone and trigger curves, not stick sensitivity/curve. Implement a left-stick response curve/sensitivity in the Controls page. |
| 219 | Stutter at standing starts | performance | P1 | Event-start streaming/audio or shader/pipeline compile. Needs GSdx vs CoreGS 2.0 and cold/warm A/B, plus the log zip. |
| 218 | A-Spec in Spec II / crash after race | crash | P0 | The title and body disagree; ask for version/GPU/renderer and reproduce on rc16. Log attached. |
| 217 | Online public lobbies | feature | P3 | Not supported in 1.x; the PS2 online service is gone. Record as a LAN/online launcher idea, not scheduled. |
| 216 | No sound | audio | P1 | Empty report. Needs device, Windows version, game log and whether both games are affected. |
| 215 | Crash after credits | crash | P0 | Black screen with menu sounds. Needs version/GPU/renderer and a fresh log; inspect the attached zip. |
| 213 | Split-screen second pad | input | P1 | The launcher maps Player 1; add Player 2 mapping/selection and verify the game's 2P mode. Needs controller details. |
| 212 | Crash leaving GT Mode (rc15 Spec II) | crash | P0 | `missing code at 0x00000000` means an indirect call to a null/untranslated address. Reproduce on rc16; inspect coverage and add a guard/report. |
| 211 | Ghost car | known | P3 | Known PS2 divider/decryption limitation; owner replied. Keep open until the exact divider work is scheduled. |
| 210 | Portable `.ini` ignored with `.bat` | docs/UX | P2 | The `.ini` is read by the launcher, not `gt4.exe`; document it and give the equivalent flags. WebView2 requirement must be in the portable docs. |
| 209 | Photo Travel flashing (rc14 Spec II) | visual | P2 | Retry on rc16; photo-mode readback/flash work. Needs GPU/renderer and a fresh log. |
| 208 | New/delete save (rc14 Spec II) | data loss | P0 | Fixed for rc15 (cross-disk move with copy + verify). Ask the reporter to confirm on rc16, then close. |
| 206 | 360 chase camera | input/camera | P2 | Compare with the original PS2 camera; confirm version/controller; a clip would help. |
| 204 | Steam Deck/Proton | performance/platform | P1 | Not a supported 1.x target; Linux is a later phase. GSdx/1x is the test; keep the logs for the Linux work. |
| 202 | Crash Spec II + CoreGS 2.0 | crash | P0 | Needs version/GPU/renderer; rc16 A/B with GSdx; inspect the attached log. |
| 201 | Crash on first load (CoreGS 2.0) | crash | P0 | GPU/driver startup path; needs GPU/driver and a GSdx A/B; inspect the attached log. |
| 199 | Crash leaving garage | crash | P0 | Coverage/state path; needs version/GPU/renderer and a fresh log on rc16. |
| 198 | Crash in game + shader timing | crash | P0 | Let the shader step finish once; rc16 retry; needs `logs\shaders.log` and the log zip. |
| 196 | Crash at launch (rc13, RU) | crash | P0 | Update to rc16; fresh logs; RX 5700 XT/Vulkan. Replied in Russian. |
| 190, 189, 188, 187, 184 | Crashes/photo slowdown (older reports) | crash/perf | P0/P1 | Owner already replied with the rc12/rc13 guidance; wait for the reporters' fresh logs or close if stale. |
| 161 | Xbox button prompts | feature | P3 | Texture-pack workaround exists; built-in option recorded, not scheduled. |
| 160 | Save/memory-card prompt | support | P2 | Answered (Cross/Z on the create-data prompt); close if the reporter confirms. |
| 135 | Wind sound / 480p color range | feature | P3 | Game audio and GS color range; recorded, no date. |
| 126 | Performance/480p/controller follow-up | support | P2 | 480p is fixed; 4:3/16:9 is planned; clarify whether "original controller" means PS2 prompts or mapping. |

## Work packages, in order

1. **Crash triage and null-dispatch guard (P0).** Collect the attached zips, reproduce on rc16, and add a diagnostic
   guard for `missing code at 0x00000000` with the caller/return address. Then fix the reachable paths. Covers #212,
   #215, #218, #199, #198, #201, #202, #196. This is the first package after rc17 because a crash with a null dispatch
   is the worst user experience.
2. **CoreGS 2.0 stability (P0).** Startup/exit paths on specific GPUs (#201, #202, #199, #198). A/B with GSdx; collect
   driver versions; do not weaken the renderer gates.
3. **Analog stick sensitivity/curve (P1).** Add a left-stick response curve and sensitivity to the Controls page,
   persisted in `controls.toml`, with the same UI language as the trigger curves. Test PS4 and Xbox pads. (#220)
4. **Split-screen Player 2 (P1).** Map a second pad, expose it in the launcher, and verify the game's 2P mode. (#213)
5. **Sun glare readback (P1).** Measure the GSdx unsynchronized workaround vs CoreGS 2.0; the native path should use
   GPU-side flare visibility. (#221)
6. **Race-start stutter/audio (P1).** A/B cold/warm, GSdx/CoreGS, with the audio and pacing counters; fix the event-start
   path if it is streaming or shader compilation. (#219)
7. **Photo mode/Travel (P2).** Reproduce the flashing on rc16, then the readback/flash work; keep the exact photo bytes
   on the faithful renderer. (#209, #184)
8. **Portable docs and settings (P2).** Document WebView2 and the launcher-vs-exe `.ini` behaviour; add the equivalent
   flags if the report is confirmed. (#210)
9. **Issue hygiene (P2/P3).** Close the fixed/answered ones after confirmation (#208, #160), record the not-planned
   ones (#222, #217, #135, #161), and keep #211 open until the divider work is scheduled.

## Triage rules

- One reply per report with the minimum data request; no promises of dates.
- Do not close a bug without a confirmation or a documented reason.
- A fix needs a reproduction on rc16 and an A/B against the other renderer where the renderer is suspect.
- Keep the public wording plain; the technical detail goes in the repository docs, not in the issue thread.
- The CoreGS 3.0 work moved to the other notebook; issue triage and the TASK-213 settings work stay on this machine.
