# Issue intake and local owner reports (2026-10-26)

## Local report: uneven frame pacing in video scenes on i5-7200U

Recorded 2026-10-26; documentation only, without an implementation plan or release target.
The owner wants this report in repository documentation, not the GitHub issue tracker.
Issue #114 was created in error and deleted at the owner's explicit request.

- Hardware: i5-7200U laptop with integrated graphics and a shared 15 W power budget.
  Title-menu background video and the opening movie show uneven frametimes; ordinary
  menus without video have a smooth graph, according to the owner.
- Comparison: video scenes are smooth on the owner's Ryzen 7 5700X / RX 9060 XT desktop.
- Screenshot `Captura de tela 2026-10-26 042303.png`: title background, Vulkan,
  approximately 55 FPS and instantaneous 8.8 ms; uneven history graph with spikes.
- Screenshot `Captura de tela 2026-10-26 042401.png`: opening movie, D3D11,
  approximately 41 FPS and instantaneous 70.4 ms; uneven graph with a pronounced spike.
  Original screenshots remain at `C:/` and were supplied in chat; not committed.
- Overlay readings are snapshots, not averages/percentiles. The different graphics APIs
  do not constitute a controlled comparison. Exact build/settings per image, resolution
  scaling, frame-skipping switch state and video-scene audio behavior are unverified.
  Do not assume the separate racing stress test's 2x setting applies here.
- Keep separate from TASK-082's stale-image/frame-skipping defect. A common cause with
  performance reports #97/#112 is unknown. Expected: consistent video presentation cadence.
- Status: recorded only, root cause unknown; no profiling, task number, implementation
  or release commitment. No agent dispatched.

## Original GitHub intake snapshot

Owner request: register tasks, consolidate duplicates and reuse Claude's existing tasks.
This is an intake snapshot, not an implementation plan. No code, design steps, estimates,
implementation branches or release promises are introduced. No issue comment, closure, label
or milestone was changed. Priorities are candidates for the owner's later selection.

Sources: live open issues/comments; main fa92d05; modern TASKS at frozen rc6 commit
58322229c0bd63a0652b0550c5ad2154bd907039; task/101 at cedd832; CPU-parity entry
9226927 on task/116-117-flare-perf; ai-memory notes/owner-plans-from-issues.

## Complete open-issue mapping

| Issue | Report | Owning task(s) | Intake and priority |
| --- | --- | --- | --- |
| [#112](https://github.com/moesuito/gt4-staticrecomp/issues/112) | Integrated-graphics slowdown | Existing TASK-041; related 082/105/116/117/118 and CPU-parity119@rc6 | Same performance backlog as #97, separate hardware evidence; candidate rc follow-up |
| [#107](https://github.com/moesuito/gt4-staticrecomp/issues/107) | Cannot start the game | TASK-124 | New; high, candidate rc; cause unknown |
| [#104](https://github.com/moesuito/gt4-staticrecomp/issues/104) | Service sound continues | TASK-125 | New; normal, release unassigned |
| [#102](https://github.com/moesuito/gt4-staticrecomp/issues/102) | Home key unavailable to ReShade | TASK-126 | New; normal, release unassigned |
| [#99](https://github.com/moesuito/gt4-staticrecomp/issues/99) | Window flashes and game crashes | TASK-127 | New; high, candidate rc; not a proven duplicate of #89/#107 |
| [#98](https://github.com/moesuito/gt4-staticrecomp/issues/98) | Deleted save returns | Existing TASK-025, related 058 | Save-manager follow-up; high, candidate rc; no new duplicate task |
| [#97](https://github.com/moesuito/gt4-staticrecomp/issues/97) | Low-end hardware performance | Existing TASK-041 and modern performance work | Group with #112 under existing backlog; retain separate reports |
| [#95](https://github.com/moesuito/gt4-staticrecomp/issues/95) | Reuse texture and ISO paths | Existing TASK-053 (textures), TASK-128 (ISO) | No duplicate texture task; ISO request remains under consideration |
| [#93](https://github.com/moesuito/gt4-staticrecomp/issues/93) | No audio device causes exit | TASK-129 | New number for Claude's existing note; high, candidate rc; closed #94 is duplicate |
| [#92](https://github.com/moesuito/gt4-staticrecomp/issues/92) | Split-screen display and 4:3 | Existing TASK-080/079 (4:3), TASK-130 (split display) | Existing aspect scope reused; split concern newly numbered, no release assigned |
| [#89](https://github.com/moesuito/gt4-staticrecomp/issues/89) | Crash on integrated graphics | Existing TASK-105 on 1.3.0 line | Reporter validation/follow-up still needed; no replacement task |
| [#88](https://github.com/moesuito/gt4-staticrecomp/issues/88) | Wheels and FFB | Existing TASK-018 and research 123 | Native DFP remains planned for 1.3.1; adapted input does not complete it |
| [#87](https://github.com/moesuito/gt4-staticrecomp/issues/87) | Cheats request | Existing TASK-057 scope/disposition and owner decision | Remains not planned; no cheat implementation task is created |
| [#86](https://github.com/moesuito/gt4-staticrecomp/issues/86) | PCSX2 card import | Existing TASK-104 on 1.3.0 line | New chain-entry-7937 failure unresolved; high, candidate rc follow-up |

## Reuse and evidence limits

- TASK-104/105 already exist on the modern line, although their complete sections are absent
  from stable main. The previous import fix does not establish that the new #86 error is fixed.
- Claude's 4:3, external-ISO and no-audio notes had no task IDs. Reuse TASK-080 for 4:3,
  TASK-053 for texture paths; newly number only the remaining ISO/audio concerns as 128/129.
- TASK-119 has three historical branch meanings: main RetroAchievements, rc6 CPU parity and
  task/101 player-one controller selection. Qualify the CPU task by branch until integration
  resolves numbering; no task is renumbered or lost here. New IDs 124..130 are reserved.
- Issues #97/#112 share a performance backlog; this does not prove identical bottlenecks.
- Issues #107/#99/#93 remain separate until evidence establishes a shared cause. Consolidate
  references later if confirmed, rather than losing a report based on similar symptoms.
- High candidates are 124, 127, 129 and existing save/import/GPU work 025/104/105, alongside
  relevant performance work. The owner chooses what enters a subsequent 1.3.0 rc.
- No particular rc, fix or implementation is promised, and no active task is interrupted.

## Owner-authorized initial replies (2026-10-26)

The owner explicitly requested replying to open issues without a previous moesuito response.
Rechecked immediately before posting; these five issues still had zero comments. Published
English replies under the verified moesuito account, acknowledged reports and requested missing
version/reproduction details. No fix, completion date, closure or release was promised.

- [#107 reply](https://github.com/moesuito/gt4-staticrecomp/issues/107#issuecomment-6410551717)
- [#104 reply](https://github.com/moesuito/gt4-staticrecomp/issues/104#issuecomment-6410552369)
- [#102 reply](https://github.com/moesuito/gt4-staticrecomp/issues/102#issuecomment-6410552999)
- [#99 reply](https://github.com/moesuito/gt4-staticrecomp/issues/99#issuecomment-6410553643)
- [#98 reply](https://github.com/moesuito/gt4-staticrecomp/issues/98#issuecomment-6410554415)

All five bodies and authors were reread after publication. A fresh open-issue query returned
no issue without a moesuito comment. Follow-up reports on previously answered issues remain
separate work; this does not mean every new comment has received another response.

## Update 2026-11-04 (owner: close what is fixed, answer the control and feature requests)

Closed as fixed, each with a note in the reporter's language:

- For 1.3.0-rc12: #183 and #134 (trigger range, TASK-147), #178 (stick dead zone, TASK-147), #99 (flashing window and hang
  exit, TASK-145 via TASK-127).
- In published pre-releases: #191 (GT Auto bubbles, rc10, as #104), #185 (Spec II replays, rc10), #139 and #179 (GSdx missing,
  Play offers CoreGS 2.0, rc10), #115 (no sound device, rc10), #123 (toggles with a controller, rc8), #132 (portable build,
  rc7), #89 (integrated GPU start, rc5; the reporter races on rc6 in #112).

Answered and left open: #161 (Xbox button icons: not planned, a texture pack can replace them), #88 (wheel and force
feedback: planned after 1.3.0, no date), #128 (European release: not planned). 24 issues stay open.

## Review of the open issues after 1.3.0-rc13 (2026-11-04)

The 18 open issues and their six log bundles were reviewed against what each release fixed; replies were posted in the owner's
name once rc13 was out.

- **Closed:** #87 (cheats, not planned: each code patch must be mapped by hand in a recompiled game), #128 (PAL, not now),
  #186 (duplicate of #189).
- **AMD crashes on CoreGS 2.0** (#187 RX 6700 XT, #188 RX 6600, #189 RX 5700 XT, #190 RX 6700; rc8 to rc10): exit 0xC0000005
  right after a stall, no error line, the shader step unfinished in most crashing launches. Asked to retest on rc13 (rc12 fixed
  a thread-unsafe use of the command pool and swapchain, IN-536) with full logs and `logs\shaders.log`. #190's older
  0xCFFFFFFF closes are the "not responding" bug fixed in rc12 (TASK-145).
- **Needs information:** #107 (0xC0000135, a DLL failed to load before any game log; the game's DLLs all ship with it),
  #126 (no data), #181 (images only).
- **Retest asked:** #112 (CoreGS 2.0 and the frame-rate modes on an i3 UHD), #184 (photo mode: flicker fixed in rc13, frame
  times are TASK-162).
- **Open as requests:** #97 (weak hardware; HD 4000 has no Vulkan 1.2), #88 (TASK-018, TASK-123), #92, #135 (TASK-163),
  #160 (TASK-164, in rc14), #161 (TASK-165).
