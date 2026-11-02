# TASK-082: unlocked FPS, where the study stopped (2026-11-02)

**Paused by the owner.** What shipped is the 30 FPS mode. The goal to come back to: drawing fully separate from the physics, which stay at 60 Hz, at any framerate below and above 60, with a basic interpolation between the states. Background: `docs/NATIVE-VARIABLE-FPS-STUDY.md` and ai-memory `decisions/native-variable-fps-plan`.

Branch `task/082-coregs2-frame-skip`, on top of `task/027-coregs2`.

## What exists and works

| Piece | Where | State |
|---|---|---|
| A frame drawn or left out, with the right flip | `native/frame_skip.{hpp,cpp}`, IN-524 | Done, both games |
| 30 FPS mode | launcher `thirty_fps_mode`, `GT4_FRAME_SKIP_FORCE=2` | Shipped, off by default |
| Any framerate below 60 | `GT4_FRAME_SKIP=1`, `Policy` | Works, not offered |
| Presentation thread with even spacing | `present_paced`, `hw_present_spacing.hpp`, IN-525 | Used by both |
| Render time forced, for the study | `GT4_FRAME_RENDER_T`, capsule `slice376-frame-render-license-guard-runtime` | Off by default |
| Frame by frame sheet and simulation check | `scripts/coregs2_bench/frame-skip-suite*.sh`, `frame_sheet.py` | Done |
| Game speed in real time | `frame_skip_speed.py` | Done |

## Measured: any framerate with even presentation

George V Paris, the owner's lap, 144 Hz, candidate before the 30 FPS mode, one run each, `GT4_FRAME_SKIP=1`. `build/evidence/fs52`, `fs50`, `owner12`.

| | Game speed | Frames drawn per second | Change from one interval to the next | Presented p5 / p50 / p95 |
|---|---|---|---|---|
| 12x, the policy of candidate 375 | 100.0% | 34.3 | 18.6 ms | - |
| 12x, cost based policy and thread | 100.0% | 42.4 | 4.4 ms | 18.1 / 24.9 / 28.4 |
| 10x | 100.0% | 54.7 | 0.8 ms | 16.5 / 17.9 / 23.9 |
| 6x | 100.0% | 60.0, nothing left out | 0.2 ms | 16.4 / 16.7 / 16.9 |
| 12x, always 1 in 2 | 100.0% | 30.0 | 0.2 ms | flat |

**Why it was not offered:** the graph is much better, but the game still moves in steps of 16.7 ms between pictures. At 42 frames per second, a picture shows 1 or 2 steps of game time at about the same interval, so the motion still judders. Only interpolation solves that.

## The interpolation study, where it stopped

**Hypothesis to test first:** the game's render time `t`. `render_world(buf, t)` takes it in `$f12`, always 0 today, and the race camera interpolates two stored poses with `t != 0` (`func_00379818`). Whether the cars use it is Unknown.

**The test:** `GT4_FRAME_RENDER_T=<t>`, hook at `frame_render_world` in `game_target.hpp` (v2.00 0x33DEA8, Spec II 0x1EA240, by masked instruction search). `GT4_FRAME_SKIP_FORCE=1` is needed so the hook is on.

**Result, and it is not an answer yet:** on Spec II, time trial, VBlank 3210, t = 0.5, 1 and -1 gave frames identical to t = 0, car and scene. So either the hook is not reached or `t` does nothing there.

**The next step, not done:**
- Spec II has a `jal` to its `render_world` from 0x26EA8C, which v2.00 does not have. The time trial's class (vtable 0x6BA5A8) may draw through another function at vt+0x348. Read that slot, count the hook's hits, and repeat on v2.00, where only the vtables reach it.
- If `t` moves the camera and not the cars, the cars need states kept by the host, per step (position and orientation of each `Automobile`), and fed to the car pose call.

**The rest of the plan (Hypothesis, from the study):**
1. **Extra pictures without an ELF patch:** the study planned to patch the main loop. A new idea: from a dispatch hook on the last idle functor, call the guest functions directly, on the EE thread: the packet toggle `func_001077F8`, render functors, `func_00107840`, idle functors, and the two page words with `func_00105608`, with no VBlank wait. To be checked.
2. **Nothing the game can see may change in an extra picture:** the render counter `+0xE32C`, the history byte `+0xCF5A`, the flare read, `DMAsafe`. The guest clock must not advance.
3. **Cost:** the limit above 60. At 6x, GS 11 ms and GPU 11 ms per picture, so about 90 FPS at best on this GPU; more at lower scales. The EE's part of a picture was never measured apart.
4. **Latency:** interpolating adds up to 16.7 ms.

**Not tried:** the image route, TASK-045 and FSR 3 style frame generation. CoreGS 2.0 has no motion vectors.

## Why the menus stay at 60 (measured, candidate 376's code)

The owner asked for 30 FPS everywhere, showroom included, on weak PCs. Two ways were tried on the owner's drive and neither works. The menus run in 480i.

- **Leaving out the drawing, as in races (`build/evidence/menu30`):** the menus keep state in their drawing. A dialog came out wrong (a dark panel at its right, the text shifted), animations ran slower, and the frames were up to 8 VBlanks or more behind the reference (`sheet-f`, `sheet-g`: of 293 frames, 96 and 9 ok). The recorded drive reached its race, and the race clock differed from VBlank 2170 on, so a menu's timing changed what came next.
- **The game's own 30 Hz mode, the flip cycle of movies (`build/evidence/menu30b`):** writing 2 to `D_006186E4` outside races, from the start of the game: 1,701 of 1,701 dumped frames black up to VBlank 4000. Not looked into. It writes it during the boot too, which may be why. Even working, the whole menu would run at half speed, since menu timers count frames.

**So the launcher's choice applies to races.** What is left for the showroom is lighter drawing itself, or finding what in the menus depends on drawing. The code of these two tests is not kept; it is in this file's commit message history only as this description: a gate open everywhere in `Controller::decide`, and `write_word(flipCycle, inRace ? 1 : 2)`.

## Traps, already paid for

- A drawing list runs a frame after it is built: the frame after a frame left out is the one that must not flip (IN-524).
- A wait on the GS worker looks like lateness and raises the mean: it locked at 20 frames per second. A picture that could not be shown must still count as an arrival, or it locks at 30.
- A lateness policy swings, since the GS worker is frames behind the decision.
- `frametime.csv`'s `vblank` counts presentations. Game speed comes from the frame skip log's wall clock.
- One-queue devices (probably the HD 620, to be checked) have no presentation thread: the wait stays on the GS worker, with room left (`g_one_queue_spacing`). Never tested on one.
- On the owner's display, shown times round to the 144 Hz grid: an even presentation still shows as 20.8 and 27.8 ms without a variable refresh rate display.
