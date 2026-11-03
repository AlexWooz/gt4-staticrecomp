# 0076: the game window keeps a one-second responsiveness timer

Date: 2026-11-03. TASK-145. The owner asked for the fix to be documented so that no agent removes it, with the reason.

## Decision

Right after the game window is created, the game starts a timer that sends the window a message every second
(`native/window_heartbeat.hpp`, `interval_ms = 1000`). The window procedure answers the timer's `WM_TIMER` and does
nothing else with it.

**Keep it.** Do not remove it, make it slower than one second, filter the message loop (`PeekMessageW(&message, nullptr,
0, 0, PM_REMOVE)` must keep reading every message), or replace it with something else, unless the test at the end of this
page passes on an NVIDIA RTX PC. `GT4_WINDOW_HEARTBEAT=0` turns the timer off for an A/B only.

## Why

- **How Windows sees a hang.** Windows takes a window as "not responding" when the thread that owns it has not read a
  message for about 5 seconds.
- **Empty checks do not count.** The game thread calls `PeekMessage` on every VBlank, but a call that finds the queue
  empty does not count as a read. On the owner's laptop, `IsHungAppWindow` returned TRUE for the game window 0 to 16 ms
  after one of those calls (High confidence: measured, see Evidence).
- **The queue stays empty.** This happens in fullscreen with a gamepad: SDL3 and XInput send no window messages, and
  nobody touches the mouse or keyboard.
- **The visible effect.** Windows declared the window hung after about 5 seconds. On NVIDIA RTX PCs the shell and
  `dwm.exe` then did two things:
  - for a frame, they put another window, the taskbar or the white "Ghost" window in front of the borderless fullscreen
    window;
  - they restored the game window with an asynchronous `SetWindowPos` (flags `0xd803` and `0xc043`: an internal state
    change, asynchronous, show window, no move, no size).

  Reading that restore reset the clock, so the cycle repeated every 360 VBlanks (6 seconds).
- **What players saw:**
  - a one-frame flash of the desktop, the taskbar or the window behind the game;
  - a late frame (24-86 ms);
  - after Alt+Tab, a white "Not responding" window while the game kept running behind it.

  It showed on CoreGS 2.0 and on GSdx (Vulkan, Direct3D 11 and Direct3D 12) alike, because the window belongs to the game,
  not to the renderer.
- **The fix.** A timer message every second is a real read. Windows sees the thread reading its queue, and none of the
  above happens.
- **Costs and limits:**
  - It costs one message per second.
  - If the game thread really stops, the timer messages wait unread, and Windows still reports the hang, which is
    correct.

## Evidence

Measured on 2026-11-03 on the owner's laptop: Spec II, RTX 3060 Laptop GPU, one 3440x1440 144 Hz monitor on the RTX,
CoreGS 2.0 and GSdx, plain test builds.

- **Before the fix:**
  - In every fullscreen session (15 sessions in the log folder), another process changed the window's position every 360
    VBlanks.
  - With the `[window] watch` diagnostics, Windows marked the window NOT RESPONDING at VBlank 1006, while its messages had
    been read 0 ms before.
  - After an Alt+Tab, Windows marked the window hung 5.7 seconds after it lost focus. A `dwm.exe` Ghost window then
    covered it for 15.6 seconds, until the owner clicked back.
- **With the timer:**
  - A full lap, 10,017 VBlanks, with no NOT RESPONDING, no position change from elsewhere and no size message with the
    same size. The remaining late frames were irregular, from the laptop's thermal throttling.
  - The game stayed in the background for 22 seconds after an Alt+Tab and came back normally.
  - The owner then tested GSdx and saw no flash.
- **Ruled out on the way:**
  - Code inside the game process: an import-table log of `SetWindowPos`, `ShowWindow`, `ShowWindowAsync` and related
    calls in all 112 loaded modules found none on the game window.
  - SDL3: in 3.4.18 its joystick thread is on by default, and unplugging the controller changed nothing.
  - Other programs and hardware: the NVIDIA overlay, Discord, MSI Afterburner, Moonlight and the integrated GPU.
  - Plain test windows: a GDI window and Vulkan and Direct3D 11 test windows on the same laptop, which all render on their
    window thread, had no events.
  - Two attempts that changed nothing and were removed: Vulkan full-screen exclusive set to DISALLOWED, and dropping
    `SWP_FRAMECHANGED` from foreign window requests.

## Why only NVIDIA RTX

Hypothesis, not measured on other cards. Windows' hang marking is probably the same on any GPU; what differs is whether it
shows. The owner's NVIDIA control panel setting for the Vulkan present method changed how often the flash showed: "Prefer
native" every 6 seconds, "Prefer layered on DXGI swapchain" less often. This points at the way the NVIDIA driver presents
a borderless window that covers the monitor. When the shell re-stacks a hung window, that presentation path shows the
change for a frame. A composed presentation, on other cards, hides it. With the timer, the window is never marked hung,
so the question no longer matters for the game.

## Kept with it (TASK-145)

- **Size messages:** a `WM_SIZE` with an unchanged client size is not passed on to the renderer. Each one used to rebuild
  the swapchain, and every rebuild shows what lies behind the window for a frame.
- **CoreGS 2.0 swapchain:**
  - a swapchain that reports `VK_SUBOPTIMAL_KHR` at the same surface size is kept;
  - each rebuild logs its reason (`[hw] swapchain rebuilt: ...`).
- **Diagnostics,** with `GT4_WINDOW_WATCH=1`, or a test build compiled with `GT4_TEST_WINDOW_WATCH=1`, since the launcher
  does not pass the environment on:
  - the hang watcher (`[window] watch`), with the game thread's stack when it stops reading for a second;
  - activation and position-change logs.

## Test before any change

On an NVIDIA RTX PC, in borderless fullscreen, with a gamepad only:

1. Spend two minutes in the menu and in a race.
2. Alt+Tab away for 10 seconds or more, then come back.
3. Check the log of a build with `GT4_TEST_WINDOW_WATCH=1`. It must show:
   - no "NOT RESPONDING";
   - no position change from another program;
   - no Ghost window.

CTest `native_window_heartbeat` and `tests/python/test_window_heartbeat.py` must pass.
