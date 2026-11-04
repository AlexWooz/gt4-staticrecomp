# CoreGS 2.0 presentation on a device with one queue (TASK-174)

2026-11-04. GitHub issue #204: Steam Deck under Proton (RADV), 1.3.0-rc14, CoreGS 2.0. Code in
`native/hw_gs/hw_gs_backend.cpp`, `hw_queue_lock.hpp` and `hw_shared_present.hpp`. Labels: [C] Confirmed, [H] Hypothesis,
[U] Unknown.

## Problem

- [C] The Deck's log says `[hw] presentation queue: the rendering queue`: RADV exposes one queue in its graphics family, so
  the IN-516 path (a second queue of the family copies the picture and presents) is not available.
- [C] On that path the GS thread itself acquires the swapchain image with an unbounded wait and presents. Whatever the
  display costs (a full FIFO queue, a compositor) falls on the GS thread.
- [H] On a device whose GS thread is already near its 16.7 ms budget, those waits push drawn frames over the budget (the
  issue's report). Not reproduced on a Deck.

## Paths and switches

| Path (`present_path`) | When | Who acquires and presents |
|---|---|---|
| `own_queue` (IN-516) | the family has 2 or more queues | the GS thread, on the second queue; with frames left out, the presentation thread (TASK-082) |
| `shared_queue` (TASK-174) | the family has 1 queue (default) | the presentation thread, on the rendering queue |
| `rendering_queue` | `GT4_HW_PRESENT_QUEUE=0`, or 1 queue with `GT4_HW_PRESENT_SHARED_QUEUE=0` | the GS thread, on the rendering queue (the path of before TASK-174, unchanged) |

- `GT4_HW_PRESENT_SHARED_QUEUE=0`: on a device with one queue, keep the old `rendering_queue` path (A/B).
- `GT4_HW_PRESENT_FORCE_ONE_QUEUE=1` (diagnostic): treat the graphics family as having one queue (one queue is created), so a
  desktop runs the Deck's path.
- `GT4_HW_PRESENT_QUEUE=0`: the `rendering_queue` path on any device (unchanged meaning).
- `GT4_HW_PRESENT_SHARED_WAIT=<n>` (diagnostic, off by default): see "Known limits".
- The chosen path is on the existing line, for example
  `[hw] presentation queue: the rendering queue, shared with the presentation thread (TASK-174) (graphics family 0 with 1 queue)`.

## Design of the shared path

GS thread (`present_on_shared_queue`), per presentation:

1. Under the pacer mutex, `hand_over_verdict` decides whether the picture can be drawn. It is not drawn, and counted as not
   shown, when the presentation thread is behind:
   - not spaced: it holds 2 pictures (queued plus busy), IN-516's two presentations;
   - spaced (frames left out, TASK-082): 2 pictures wait for their place, as on the second queue;
   - or the slot is still held by it.
2. The slot's fence (the copy of its last picture into a swapchain image) is checked with timeout 0. If the copy has not run on
   the GPU yet, the frame is not shown. The fence is only checked on a slot the presentation thread no longer holds, so the two
   threads never use one fence at the same time.
3. The GS thread draws the frame into the slot's window-sized picture on the rendering queue (`draw_picture`; it signals
   `picture_ready`) and hands the slot over.

Nothing on the GS thread waits for the display: there is no acquire, and every fence wait there is timeout 0 or on GPU work
that a signalled fence already covers.

Presentation thread (`pacer_worker`, `show_paced_picture`, shared with TASK-082):

1. A spaced picture waits for its place (`PresentSpacing`). Others go at once.
2. It acquires a swapchain image (100 ms limit, outside the queue lock).
3. It records the copy into `show_commands` (`show_pool`, TASK-146: never the GS thread's pool).
4. It submits the copy, then presents, each under the queue lock.
5. When no image comes, it submits a release that waits on `picture_ready`, so every binary semaphore signal gets its wait. That
   picture is counted as not shown (timeout) or failed (out of date).
6. Out of date and suboptimal results set the rebuild flag (TASK-145), as on the other paths.

Frames left out (TASK-082/146): on the shared path the spacing runs on the presentation thread, as on the second queue, so
`has_presentation_thread()` is true and the stream sink no longer sleeps on the GS thread for it.

## The lock rule

Vulkan requires external synchronization of a `VkQueue`: no two threads may be inside a call on it at the same time. On the
shared path every call below holds `QueueLock` (`hw_queue_lock.hpp`), from any thread. On the other paths the lock is not used
(each queue has one thread at a time) and the calls are the same as before.

| Call | Thread | Where |
|---|---|---|
| `vkQueueSubmit` (renderer frames, mid-frame submissions, readbacks) | GS | `core2/core2_gpu.cpp` `submit`, `writeback/hw_gs_writeback_gpu.cpp` (two), through `VulkanHandles::queueLock` |
| `vkQueueSubmit` (picture drawing) | GS | `draw_picture` via `submit_to` |
| `vkQueueSubmit` (copy, release) | presentation | `show_paced_picture` via `submit_to` |
| `vkQueuePresentKHR` | presentation | `present_locked` (queue lock, then the swapchain mutex) |
| `vkDeviceWaitIdle` (uses every queue) | GS or EE, presentation thread idle | `device_wait_idle`: rebuild, `wait_idle`, `detach_window`, shutdown; the drawer's `destroy` |

- There is no `vkQueueWaitIdle` in the backend.
- `vkAcquireNextImageKHR` is not a queue call: it holds only the swapchain mutex (TASK-146), so the GS thread's submissions go
  on while the presentation thread waits for an image.
- Lock order: queue lock, then swapchain mutex. Nothing takes them the other way.
- The rule is checked by the validation layer's thread checks (`hw_smoke_shared_queue_thread_check`). With the lock removed on
  purpose, the same run reported 4 `VkQueue` THREADING ERRORs in 600 frames; with it, 0.

Order rules:

- `pacer_drain()` runs before every swapchain rebuild, window detach and `wait_idle` (the latter is new: `vkDeviceWaitIdle`
  must not overlap the presentation thread's calls).
- Shutdown joins the presentation thread (`pacer_close`, which shows what is still queued) before `vkDeviceWaitIdle` and before
  anything it uses is destroyed.

## Counters

- The `[hw] presentation` line, per `GT4_HW_PROFILE_EVERY` interval and once for the session (`session`), and the same numbers
  in `hw_gs_smoke`:
  - `path`;
  - `requests`, `shown`, `not_shown`, `failed`, `left_out` (IN-521); `requests` is the sum of the other four;
  - per presentation: `gs_present_ms` (the GS thread's presentation step), `gs_wait_ms` (its fence waits, acquire, submit and
    present calls there), `gs_lock_wait_ms` (its wait for the queue lock, the renderer's submissions included),
    `presenter_lock_wait_ms`, `presenter_queue_ms`, `presenter_display_wait_ms` (acquire and the optional shown wait);
  - session maxima: `gs_lock_longest_ms`, `presenter_longest_ms`.
- For the Deck report, compare `gs_present_ms`, `gs_wait_ms` and `gs_lock_longest_ms` with `GT4_HW_PRESENT_SHARED_QUEUE=0`.

## Measurements (desktop, hidden window)

[C] RX 9060 XT, Windows, 144 Hz monitor, `hw_gs_smoke --window --display-640`, `GT4_HW_SCALE=2`,
`GT4_HW_PRESENT_FORCE_ONE_QUEUE=1`. Per presentation, GS thread. The hidden window is not the game's window: these numbers show
where the waits fall, not in-game frame times.

| Frames | Path | GS present step | GS waits | Shown |
|---|---|---|---|---|
| 60 fps, 360 | shared | 0.064 ms | 0.025 ms (lock longest 0.001 ms) | 360 |
| 60 fps, 360 | rendering queue (old) | 0.322 ms | 0.276 ms | 360 |
| 60 fps, 360 | own queue (IN-516) | 0.415 ms | 0.365 ms | 360 |
| 144 fps, 1,200 | rendering queue (old) | 8.48 ms | 8.44 ms | 1,200 |
| 144 fps, 1,200 | shared | 0.076 ms | 0.054 ms, lock 0.75 ms on average, longest 19.9 ms | 1,056 |

## Known limits

- [C] One queue still serializes the CPU calls. On this desktop's Windows driver, `vkQueuePresentKHR` in FIFO blocks 6 to
  22 ms per present when frames come faster than the display takes them. It holds the queue lock meanwhile, so the GS
  thread's next submission waits as long (`gs_lock_longest_ms`, `presenter_longest_ms`). At 60 fps on 144 Hz it was rare: the
  longest lock wait was 0.001, 0.001 and 5.2 ms in three runs.
- [H] Mesa's X11 swapchain (RADV) presents from a thread of its own in FIFO and waits in the acquire, which the presentation
  thread now takes. Its present would then not block. To check on a Deck with the counters above.
- [C] `GT4_HW_PRESENT_SHARED_WAIT=<n>` has the presentation thread wait, outside the lock, until at most n - 1 of its
  presentations are on their way to the display. It needs the shown times of VK_KHR_present_wait2 and has a 50 ms limit. At
  144 frames per second on 144 Hz:
  - n = 1 bounded the GS thread's longest lock wait at 1.2 ms but showed 589 of 1,200 frames (1,056 without);
  - n = 2 bounded it at 2.8 ms and showed 798.
  - A presentation took 12 to 14 ms to be shown in this window (two refreshes), so at 60 Hz n = 1 could hold 30 frames a
    second. It stays off until measured on a real 60 Hz display.
- [H] One queue also serializes the GPU work. A copy waiting for its swapchain image holds back what the GS thread submits
  after it (the IN-516 finding on a virtual display). The CPU-side fix cannot remove that; the GS frame time shows it.
- [C] `hw_smoke_resize` on this desktop takes 30 to 600 ms per toggle on every path, including the default one. Its regular
  expression does not check the smoke's exit code, so the 100 ms limit is not enforced. `hw_smoke_resize_shared_queue` does
  the same and checks the accounting.

## Tests

- `hw_queue_lock` (CPU only):
  - the lock keeps 3 threads out of a fake queue at once;
  - the wait is charged to the waiting caller;
  - `hand_over_verdict`, case by case;
  - a 1,500-frame model of the protocol on a fake queue and display, with frames left out on and off. It checks that no
    picture is drawn over before its copy, that each semaphore is signalled and waited once, that every presentation is
    accounted for, and that the GS thread never waits for the display.
- `hw_smoke_window_shared_queue`: real swapchain on a hidden window, 300 presentations.
- `hw_smoke_window_shared_queue_frames_left_out`: 600 presentations, frames left out switching on and off, uneven arrivals,
  4x.
- `hw_smoke_shared_queue_thread_check` and `hw_smoke_present_thread_check`: the same stress under the validation layer's thread
  checks, on the shared path and on this device's own path.
- `hw_smoke_resize_shared_queue`: 8 swapchain rebuilds with the presentation thread.
- `hw_smoke_window_shared_queue_off`, `hw_smoke_window_present_queue_off`: the old path stays as it was.
