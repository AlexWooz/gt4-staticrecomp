# GS stream protocol, version 1.3 (normative)

Status: implemented in TASK-035 (slice191, branch `task/035-gs-stream`), 2026-10-15. Decision
[0070](decisions/0070-pcsx2-gs-separate-process.md), plan [GSDX-PLAN](GSDX-PLAN.md) section C.

The words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119. The header
[`native/gs_stream/gs_stream_protocol.h`](../native/gs_stream/gs_stream_protocol.h) (MIT, C++20, no dependencies) is the
reference for every struct; this document describes the rules around it. Where the two disagree, the header's
`static_assert` layout wins and this document is a bug.

Clean-room note: written from the PlayStation 2 GS register map, our own `.gscap` capture format and the
TASK-034 converter's file-format notes. No PCSX2 or paraLLEl-GS code was read or copied for it.

Version 1.1 (TASK-047, 2026-10-16) changes only the meaning of VSYNC `field` (section 6.1) and adds SYNCV to the
SetGsCrt writes (section 7); layouts are unchanged and a 1.0 producer of this repository sent the same field values.
The 1.1 text was written by a PCSX2-tainted agent of the GS fork task; it states PS2 hardware behavior and our own
producer's timing, and contains no PCSX2 code. History: 1.0 (TASK-035, slice191), 1.1 (TASK-047, slice196), 1.2
(TASK-060/062, slice201), 1.3 (TASK-080, slice212b).

Version 1.2 (TASK-060 and TASK-062, 2026-10-16) adds, without changing any existing layout or meaning: CONTROL kind 6
`cursor` (section 6.1), the record type 11 `OSD_STATS` (section 6.5) and the READY flag 8 `kReadyFlagOsdStats`
(section 6.2). A producer sends them only to a consumer whose READY `version_minor` is 2 or more, so a 1.2 producer
works with a 1.1 consumer (gt4-gs 0.4.0) and a 1.2 consumer with a 1.1 producer (the slice199 game). The 1.2 text
was written by a PCSX2-tainted agent of the GS fork task; it describes our own records only and contains no PCSX2 code.

Version 1.3 (TASK-080, 2026-10-21) adds, without changing any existing layout or meaning: CONTROL kind 7
`display_aspect` (sections 6.1 and 6.7) and the ERROR code 7 `bad_argument` (section 6.2). The consumer's READY
`version_minor` is 3; a producer sends kind 7 only to a consumer whose READY minor is 3 or more, so 1.2 consumers
(gt4-gs 1.0.x) keep working with a 1.3 producer and never receive it. The 1.3 text was written by an agent that is not
PCSX2-tainted, from our own records only.

Version 1.3, P2 amendment (TASK-080 P2, slice217, 2026-10-21): `display_aspect` mode 1 carries a fill target in
`args[1]` (the target aspect x 1e6; 0 = the whole window, the earlier meaning). Before it, `args[1]` was reserved and
sent as 0, so a consumer that ignores it (gt4-gs 1.1.0) keeps the earlier behaviour (fill the whole window), and the
READY minor stays 3. Written by an agent that is not PCSX2-tainted.

## 1. Scope

The protocol carries GS hardware traffic from a **producer** (the game process, which runs the EE, VU and IOP
code) to a **consumer** (a GS renderer in another process), and a small back channel. It is generic PS2 GS data:

- GIF transfers per path (PATH1 VU1 XGKICK, PATH2 VIF1 DIRECT, PATH3 GIF DMA/FIFO), in acceptance order;
- privileged GS register writes (`0x12000000-0x12001FFF`);
- vertical syncs with the field bit;
- local-to-host transfer (FIFO) reads and their replies;
- resets and control messages (window, snapshot, configuration, shutdown).

It carries **no guest code, no guest RAM, no EE/VU/IOP state**. It contains no game-specific semantics; the game
serial in the control block only lets a consumer pick per-game settings.

## 2. Encoding

- All integers are little-endian, fixed width (`uint8/16/32/64`). Structs have no implicit padding (every size
  and offset is checked by `static_assert`).
- Every record starts on a 16-byte boundary: a 16-byte `RecordHeader`, `payload_bytes` bytes of payload, then
  zero padding to the next multiple of 16. Padding bytes MUST be zero (so streams compare byte for byte).
- `RecordHeader.sequence` numbers the records of one direction 1, 2, 3, ... without gaps. Wrap fillers carry
  sequence 0 and do not use a number.
- Text fields are UTF-8 (ASCII in practice), NUL padded; a full-width field has no terminator.

## 3. Session mapping

One named file mapping per session, created by the producer:

- Name: `Local\<prefix>-<producer pid>-<128-bit random, 32 lowercase hex digits>`; the prefix is `gt4gs` in the
  game. The name MUST NOT already exist (the producer refuses `ERROR_ALREADY_EXISTS`).
- Default security descriptor (same user, same session).
- Layout (offsets are in the control block; a consumer MUST read them from there):

| Region | Default size | Offset rule |
|---|---|---|
| Control block | 4,096 bytes | 0 |
| Command ring (producer -> consumer) | 32 MiB | `command_offset`, page aligned |
| Response area (readback replies, snapshots) | 4 MiB + 64 KiB | `response_offset`, page aligned |
| Event ring (consumer -> producer) | 256 KiB | `event_offset`, page aligned |

Ring capacities MUST be multiples of 4,096 bytes. The response capacity MUST be a multiple of 16.

### 3.1 Control block (`ControlBlock`, 4,096 bytes)

| Offset | Field | Writer | Notes |
|---|---|---|---|
| 0 | `magic` u32 = `0x54535347` ("GSST") | producer | |
| 4 | `version_major` u16 = 1, `version_minor` u16 (3 since version 1.3) | producer | |
| 8 | `control_block_bytes` u32 = 4096, `record_header_bytes` u32 = 16 | producer | layout check |
| 16 | `producer_pid`, `consumer_pid` u32 | each side | consumer writes its pid at the handshake |
| 24 | `state` u32 (atomic bits) | both | 1 producer ready, 2 consumer ready, 4 fatal, 8 exiting |
| 28 | `frames_in_flight_limit` u32 | producer | 1..3 (default 2) |
| 32 | `serial` char[16] | producer | for example `SCUS-97328` |
| 48 | `mapping_bytes`, then offset/capacity of command, response and event regions (u64 each) | producer | |
| 128 | `command_write` u64 | producer | own 64-byte line |
| 192 | `command_read` u64 | consumer | own 64-byte line |
| 256 | `event_write` u64 | consumer | |
| 320 | `event_read` u64 | producer | |
| 384 | `data_waiter`, `space_waiter`, `response_waiter`, `event_waiter` u32 | the sleeping side | section 5 |
| 448 | `vsyncs_posted` (producer), `vsyncs_done` (consumer) u64 | | frame pacing |
| 512 | `readback_request` (producer), `readback_done`, `readback_bytes` (consumer) u64; `readback_status` u32 at 536 | | section 6.4 |
| 576 | `producer_heartbeat`, `consumer_heartbeat` u64 | each side | informational |

Shared fields are plain integers accessed with atomic operations (`std::atomic_ref`); position stores and waiter
loads MUST be sequentially consistent (section 5).

## 4. Rings

Both rings are single-producer/single-consumer byte rings with the discipline of `native/mtvu_ring.hpp`:

- Positions are monotonic 64-bit byte counters; the byte index is `position % capacity`.
- The writer writes the whole record, then publishes `write = old + filler + size`. The reader reads `write`,
  consumes records, then publishes `read`.
- A record that does not fit before the end of the buffer is preceded by a **wrap filler**: a header with type 0
  whose `payload_bytes` makes it run exactly to the end. Readers skip fillers; fillers never reach a file.
- The largest record is `capacity / 2` bytes (header included), so a filler plus a record always fit into an
  empty ring. Larger data is **split**: every part but the last carries `kRecordFlagMore`; the parts are
  consecutive records of the same type. For GIF each part repeats `GifPayload` with its own `bytes`; a consumer
  MAY feed the parts to its GIF parser one by one (the GIF byte stream is the same) or join them.
- A producer that finds the ring full MUST wait (back-pressure); it MUST NOT drop or reorder records.

## 5. Wake-ups and waits

Four named auto-reset events: `<name>-data` (consumer waits for commands), `<name>-space` (producer waits for ring
space or frames in flight), `<name>-response` (producer waits for a readback reply), `<name>-event` (producer waits
for an event record).

A side that must wait:
1. spins for a short time (30 us in the implementation), re-checking its condition;
2. stores 1 in its waiter flag (sequentially consistent), re-checks the condition, and if still false sleeps on
   `WaitForMultipleObjects({event, peer process})` in slices of at most 100 ms;
3. stores 0 in the flag after every wake-up and re-checks. A wake-up with nothing to do (a stale signalled event)
   is normal and MUST be tolerated.

The other side, after publishing a position (sequentially consistent store), loads the matching waiter flag and
signals the event only when it is 1. This Dekker-style pairing loses no wake-up.

Producer wake batching (TASK-041, slice198; no layout or record change, so the version stays 1.1): for GIF and PRIV
records the producer MAY delay the data wake-up of a sleeping consumer until it has posted a number of record bytes
since the last wake-up (the game: `GT4_GS_WAKE_BYTES`, default 256 KiB, 0 = every record). It MUST signal at once for
every other record type (VSYNC, READ_FIFO, RESET, CONTROL) and before it waits itself (ring space). A sleeping
consumer therefore sees batched records at the latest at the next threshold, VSYNC, readback or space wait; its
100 ms sleep slice bounds the delay in any case. Measured on the headless race schedule: about 320 event signals
per frame without batching, 11-14 with it.

Every sleep MUST also wait on the peer **process handle**; when it is signalled the wait ends with an error (the
peer died). A wait also ends when `state` has the fatal bit. Bounded waits (handshake, shutdown) end with a
timeout error.

## 6. Records

### 6.1 Producer -> consumer (command ring), in stream order

| Type | Value | Payload | Meaning |
|---|---|---|---|
| GIF | 1 | `GifPayload {u8 path; u8 reserved[3]; u32 flags; u32 bytes; u32 reserved}` + `bytes` of GIF data (multiple of 16) | One accepted GIF transfer chunk of one path. The bytes are exactly what the GIF received on that path (tags and data). The consumer keeps one GIF tag parser per path. `flags` bit 0 `kGifFlagSignalsApplied`: the producer already applied SIGNAL/FINISH/LABEL to its own CSR/SIGLBLID copy. |
| PRIV | 2 | `PrivPayload {u32 address; u32 reserved; u64 value}` | 64-bit write to a privileged register, address 8-byte aligned in `0x12000000-0x12001FF8`. The consumer stores it in its own 8 KiB block. CSR bit 9 (RESET) is a GS reset request; CSR bits 0-4 written as 1 clear interrupt flags. |
| VSYNC | 3 | `VsyncPayload {u64 frame; u8 field; u8 reserved[3]; u32 flags}` | Start of a vertical blank: every earlier record belongs to the frame that ends here. `frame` counts from 0. When `flags` bit 0 is set, `field` is the **display field** of the frame that ends here: the CSR FIELD bit (bit 13, 1 = odd) a consumer MUST use when it presents that frame, i.e. the field the game prepared that frame's display for. The consumer uses it as is (no shift by one VSYNC). Without bit 0 the consumer alternates the field. (1.0 described it as "the CSR FIELD bit the producer reports to the EE after this VSync"; see section 7 for why both are the same value for our producer.) |
| READ_FIFO | 4 | `ReadFifoPayload {u32 qwords; u32 reserved; u64 request_id}` | Local-to-host transfer read (the TRXDIR = 1 setup came earlier in the stream). Section 6.4. |
| RESET | 5 | `ResetPayload {u32 kind; u32 path_mask; u64 reserved}` | kind 1: GS hard reset (power-on state, local memory cleared); kind 2: GIF path parsers in `path_mask` (bit N = path N) return to "waiting for a tag". |
| CONTROL | 6 | `ControlPayload {u32 kind; u32 reserved; u64 args[3]}` | kind 1 shutdown; 2 attach_window (native parent window handle, width, height); 3 resize (width, height); 4 reload_config; 5 snapshot (id, width, height; 0 = native); 6 cursor (version 1.2: flags, hide delay in ms 1..60000, 0; flag bit 0 `kCursorFlagAutoHide`): the mouse cursor policy for the consumer's window over the picture (section 6.6); 7 display_aspect (version 1.3: mode, 0, 0): how the consumer fits the picture into its window (section 6.7). |
| STATE_SAVE, STATE_LOAD, AUX_FRAME_BEGIN, AUX_FRAME_END | 7-10 | reserved | Not used in 1.0 to 1.2. A consumer answers ERROR `unsupported_record` (not fatal) and ignores them. |
| OSD_STATS | 11 | `OsdStatsPayload` (64 bytes, section 6.5) | Version 1.2: producer performance numbers for the consumer's on-screen display. Sent only to a consumer that set `kReadyFlagOsdStats`. Not GS traffic: it never changes the picture and is never written to a `.gscap` file. A payload larger than 64 bytes (a later minor version) keeps the 64-byte layout at its start. |

Any other type is a protocol error: the consumer posts a fatal ERROR `bad_record` and exits.

### 6.2 Consumer -> producer (event ring)

| Type | Value | Payload |
|---|---|---|
| READY | 0x81 | `ReadyPayload {u16 version_major, version_minor; u32 consumer_pid; u32 flags; u32 reserved; char consumer[32]; char renderer[32]; char version[64]}`. Flags: 1 readbacks are zero-filled (no VRAM), 2 the consumer presents frames, 4 the consumer records the stream, 8 (version 1.2) the consumer draws a performance overlay and wants OSD_STATS records. |
| STATS | 0x82 | `StatsPayload {u64 vsyncs, records, bytes, readbacks, readbacks_zero_filled, busy_us, wait_us, reserved}`, cumulative since READY; posted at least every 500 VSYNCs and at shutdown. |
| ERROR | 0x83 | `ErrorPayload {u32 code; u32 fatal; char text[248]}`; codes 1 version mismatch, 2 layout mismatch, 3 bad record, 4 unsupported record, 5 device failure, 6 internal, 7 bad argument (version 1.3: a known record or CONTROL kind with an argument outside its range; not fatal). |
| SNAPSHOT_DONE | 0x84 | `SnapshotDonePayload {u64 id; u32 width, height; u64 response_offset; u64 bytes}` (RGBA8 in the response area). |
| WINDOW | 0x85 | `WindowPayload {u32 state; ...}`: 1 lost, 2 restored. |
| EXITING | 0x86 | `ExitingPayload {u32 exit_code; ...}`; last record before the consumer exits. |

### 6.3 Ordering and pacing

- Records are executed in sequence order. The producer posts a record at the program point where the hardware
  event happens (section 7), so the stream order is the EE-observable order.
- After the producer posts VSYNC it increments `vsyncs_posted`; it then waits while
  `vsyncs_posted - vsyncs_done > frames_in_flight_limit`. The consumer increments `vsyncs_done` when it has
  processed a VSYNC record (for a renderer: when the frame was handed to presentation).

### 6.4 Readback round trip

1. The producer stores `readback_request = id` (previous + 1), then posts `READ_FIFO {qwords, id}` and waits.
2. The consumer executes every earlier record, writes `qwords * 16` bytes at the start of the response area,
   stores `readback_bytes` and `readback_status` (0 ok, 1 zero-filled, 2 failed), then stores
   `readback_done = id` and wakes the producer.
3. The producer copies the reply. A request larger than the response area MUST be split by the producer into
   several READ_FIFO records (the game splits at `response_capacity / 16` qwords).

### 6.5 OSD_STATS (version 1.2)

`OsdStatsPayload` (all u32 unless noted; times in microseconds; per-frame values are averages over the interval):

| Offset | Field | Meaning |
|---|---|---|
| 0 | `valid` | bits: 1 frame times, 2 main thread CPU, 4 worker busy, 8 stream costs, 16 worker wait, 32 critical path |
| 4 | `frames` | VSYNCs in the interval |
| 8 | `interval_us` (u64) | wall time of the interval |
| 16, 20 | `frame_time_avg_us`, `frame_time_max_us` | mean and longest time between two VSYNC posts |
| 24 | `main_thread_cpu_us` | CPU time of the producer thread that posts the stream (for the game: the EE thread) |
| 28 | `worker_busy_us` | busy time of a producer worker thread whose output the main thread waits for (the game: VU1 worker) |
| 32, 36, 40 | `stream_write_us`, `readback_wait_us`, `backpressure_us` | main thread time writing records, waiting for READ_FIFO replies, waiting for ring space or frames in flight |
| 44 | `worker_wait_us` | main thread time waiting for the worker |
| 48, 52 | `critical_path_us`, `frame_budget_us` | main thread CPU plus every wait above; the frame period the producer paces to |
| 56 | reserved (2 x u32) | zero |

A field is meaningful only when its `valid` bit is set. The producer posts one record about every 0.5 s of wall time,
right after a VSYNC record, so a consumer updates its display about twice per second. The records go to the command
ring only (not to a capture file, not to the stream hash), with their own sequence number like every record.

### 6.6 Cursor (version 1.2)

The producer owns the top-level window; a consumer's child window covers its client area (the picture). Mouse
messages over the picture go to the consumer's window thread, so only the consumer can change the cursor there.
CONTROL `cursor` hands it the producer's policy: with `kCursorFlagAutoHide` the consumer hides the cursor while it is
over its window and has not moved for the delay, shows it again on any movement, and never changes it anywhere else;
without the flag (or without the record) it never changes the cursor. The producer applies the same rule to its own
client area, so the two processes agree.

### 6.7 Display aspect (version 1.3)

`ControlPayload {u32 kind = 7; u32 reserved; u64 args[3]}`:

| Argument | Meaning |
|---|---|
| `args[0]` mode | 0 = game aspect: the aspect ratio of the consumer's settings file (16:9 by default) inside the window, pillarbox or letterbox, the behaviour before 1.3; 1 = fill: the picture is stretched to the fill rectangle (`args[1]`) |
| `args[1]` fill target | mode 1 only (P2 amendment, slice217): the target aspect (width / height) x 1e6 of the fill rectangle, the largest rectangle of that aspect centred in the window, with black bars on the two other sides (letterbox when the target is wider than the window, pillarbox when narrower). 0 = the whole window. Example: a custom ratio 2.44 in a 1920x1080 window is 2440000, a 1920x787 band at y = 146. The producer sends 0 with mode 0 and a value in 500000..8000000 (or 0) with mode 1 |
| `args[2]` | reserved: the producer sends 0, the consumer ignores it |

Rules:

- The consumer applies the mode from the **next presented frame**; it stays until the next display_aspect. Every
  session starts in mode 0.
- Any other mode is answered with a non-fatal ERROR `bad_argument` (code 7, new in 1.3); the consumer keeps its
  current mode and the stream continues. So is a mode 1 `args[1]` other than 0 or 500000..8000000.
- A record with the same mode and a different `args[1]` is a change (the producer sends it, for example when the window
  ratio changes in auto mode); the consumer applies the new rectangle from the next presented frame.
- The fill rectangle never stretches the picture to a window of another ratio: that was the 2026-10-21 owner report
  (Custom 2.44 on a 1920x1080 monitor stretched to 16:9 by gt4-gs 1.1.0, which fills the whole window). A consumer that
  predates the amendment ignores `args[1]` and fills the whole window; that is correct only when the target equals the
  window's ratio (auto mode).
- The consumer's READY `version_minor` is 3. The producer sends kind 7 only when the consumer's READY minor is 3 or
  more; a 1.2 consumer never receives it and always presents the game aspect.
- The producer (the game) sends a record only when the mode changes, right after a VSYNC record. Its policy (TASK-080,
  `native/display_aspect.hpp`): fill while the race view is shown at a display ratio other than 16:9 (the race camera
  projection was widened or heightened for that ratio); game aspect in menus, FMVs and photo mode. It never sends a
  record while the display ratio is 16:9, so a 16:9 player's stream is unchanged.
- Like CONTROL cursor, the record is part of the stream (it reaches a `.gscap` capture of a live session); it does not
  change GS state.

## 7. Producer rules (the game, `native/gs_stream/gs_stream_client.cpp`)

- `GT4_RENDERER` unset or `donor`: the donor renderer runs and every hook returns at once (byte-identical donor
  output). `pcsx2`: the stream renderer; no donor Vulkan device is created. Other values stop the process.
- GIF: one record per chunk at the GIF render point (`gif::render`), with the real path number. With MTVU the render
  point is the EE journal drain, so the worker's PATH1/PATH2 output and the EE's PATH3 keep their program order. The
  donor GIF path parser still runs (for `busy()` and counters) with a sink that only applies SIGNAL/FINISH/LABEL.
- SIGNAL/FINISH/LABEL, CSR, IMR and SIGLBLID stay in the game process: PATH2/PATH3 signals are applied on the EE at
  acceptance (slice172); PATH1 signals at the render point. EE reads of CSR/IMR/SIGLBLID are answered locally.
- PRIV: every privileged write is posted after the donor stored its own copy, at the same stream position as on a
  single thread: with `GT4_MTVU_PRIV_ASYNC=0` the write is a blocking MTVU sync (earlier GIF data is already in the
  ring); with `GT4_MTVU_PRIV_ASYNC=1` (default since slice198) it is queued in the MTVU journal behind the outstanding
  VU1 worker output and posted by the journal drain, so the bytes are the same without waiting for the worker
  (checked: identical stream hashes on the headless menus and race schedules). SetGsCrt posts the SMODE1 and SMODE2 values the BIOS would write and,
  for the analog modes NTSC (0x02) and PAL (0x03), SYNCV with the GS User's Manual vertical timings (NTSC
  `0x00C7800601A01801`: VS 6, VDP 480, VBPE 6, VBP 26, VFPE 6, VFP 1; PAL `0x00A9000502101401`: VS 5, VDP 576,
  VBPE 5, VBP 33, VFPE 5, VFP 1), in the order SMODE1, SYNCV, SMODE2 (version 1.1). A GS uses SYNCV VFP bit 0
  together with a non-zero SMODE1 CMOD to know that the output really alternates fields; without it a 480i
  field-mode picture is shown one field at a time, without deinterlacing. Other modes post no SYNCV (unchanged).
- VSYNC: posted at the start of the vertical blank, after the producer toggled its field bit; `field` = the new
  CSR FIELD value. On a PS2 the FIELD bit flips about 3.5 scanlines after the vertical blank starts, so a game's
  INTC VBLANK_S handler still reads the field that was just displayed. The donor runtime flips it at the same moment
  it raises VBLANK_S, so the game's VBLANK_S handler (GT4: `0x004AB430`, registered with AddIntcHandler cause 2,
  reads CSR bit 13) already reads the next field, one field earlier than on hardware. The frame the game then
  prepares (DISPFB, half-line offset) is shown at the next VSYNC; on hardware the field displayed at that point is
  the opposite of what the handler read, which is the field the donor reports at that next VSYNC. The new field
  value is therefore also the display field of the frame that ends at this VSYNC (section 6.1, version 1.1).
- READ_FIFO: posted from `gs::read_image` (FIFO read or GIF DMA channel 1 reverse transfer), after the MTVU download
  sync; the EE waits for the reply.
- TASK-116 step 0 (P0 gate): `GT4_GS_READBACK_LOG=1` (a test switch, off by default) prints one `[gs-readback]` line per
  READ_FIFO reply (frame, guest destination, qwords, status, EE wait, nonzero bytes, first 16 bytes, FNV-1a 64 of the
  reply, whether it repeated the previous one) and a per-destination summary at exit; it also prints the
  `HWDownloadMode` of the ini named by `GT4_GS_CONFIG`. It only reads the reply after the client copied it: no record
  changes, so the stream hash is unchanged (checked against the release 1.3.0 night hash, 2026-10-25).
- TASK-116 step 1 (rc6): with Performance mode on (the gt4-gs ini's `HWDownloadMode` 2, read from `GT4_GS_CONFIG`) the
  128-qword flare readback is answered locally: the client copies the previous reply (one frame old) into the guest
  buffer and queues the READ_FIFO behind the journal (new `Readback` entry, `GsSink::readback`), so the record still lands
  at its stream position and the GS process still answers it (one record per readback, ids unchanged; the night record
  counts equal the pre-rc6 Performance-mode run). `sync("download")` then only closes the VIF1 batch and is counted as
  `download_deferred`; the blocking drain runs only for readbacks that need accurate data (car icons, loading picture,
  thumbnail, photo) and for the first flare readback of a scene, through `drain_download`. `GT4_GS_READBACK_LOCAL=0`
  restores the pre-rc6 behaviour; with the VU1 worker off (MTVU), in donor mode or without the ini the local answer is
  not used and every readback is served as before. The F8 report prints the local answers per interval.
- RESET: kind 1 at the donor GS reset, kind 2 (paths 1-3) at a GIF reset.
- Presentation: none in the game; the consumer presents. CONTROL attach_window and resize carry the game window.
- Shutdown: at the end of the run the producer posts CONTROL shutdown, waits up to 3 s for the consumer to exit,
  then ends it through the job.
- Version 1.2 (slice201): after CONTROL attach_window, the game posts CONTROL cursor with its `GT4_CURSOR_AUTOHIDE`
  policy (default on, 3,000 ms) when the consumer's READY minor is 2 or more. When READY also has `kReadyFlagOsdStats`,
  the game posts OSD_STATS every 0.5 s after a VSYNC: EE thread CPU (GetThreadTimes), VU1 worker busy time (MTVU
  worker accounting), and from the performance accounting (on in every launcher run, `GT4_PERF=1`) the exclusive
  times of `gs_ipc_write`, `gs_ipc_readback_wait`, `gs_ipc_backpressure` and the MTVU waits; the critical path is the
  F8 summary formula (EE CPU plus those waits). Cost on the EE thread (measured, slice201): 0.3 us per VSYNC and
  2-3 us per record.
- Version 1.3 (slice212b): after a VSYNC, when the display aspect mode wanted by the game
  (`set_display_aspect_mode`, driven by the TASK-080 scene policy) differs from the last mode sent, the game posts
  CONTROL display_aspect to a consumer whose READY minor is 3 or more. A 1.2 consumer is never sent it; the game logs
  once that the picture keeps the game's aspect.
- P2 amendment (slice217): with mode 1 the game sends the race projection ratio as the fill target, `args[1]` =
  round(ratio x 1e6) (`aspect_millionths`, `native/gs_stream/gs_stream_client.hpp`); a change of the target alone is a
  new record. CoreGS 1.1 applies the same rectangle in-process (`presentation_rect_ratio`).

## 8. Consumer rules

1. Open the mapping by name, validate `magic`, `version_major` (MUST equal its own), `control_block_bytes`,
   `record_header_bytes`, region offsets and sizes inside `mapping_bytes`, `frames_in_flight_limit` 1..3, a
   non-empty serial and `producer_pid` equal to `--parent`. On failure post ERROR (fatal) if the event ring is
   usable, and exit with a non-zero code.
2. Open the producer process for SYNCHRONIZE and use it as the peer of every wait.
3. Write `consumer_pid`, set the consumer-ready state bit, post READY.
4. Process records in order (sections 6.1-6.6); release each record after its payload is no longer needed. A
   consumer that does not draw an overlay never sets `kReadyFlagOsdStats` and therefore never receives OSD_STATS.
5. On CONTROL shutdown: post STATS, set the exiting bit, post EXITING, exit 0. On producer death: exit (the game is
   gone).

## 9. Process lifecycle (game side)

- The game spawns the consumer named by `GT4_GS_PROCESS`, suspended, with
  `"<exe>" --ipc <session name> --parent <producer pid>`, assigns it to a new Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (so it never outlives the game) and resumes it. Standard handles are
  inherited, so the consumer's log lines reach the game's log.
- Handshake: the first event MUST be READY (or ERROR) within 20 s; otherwise the game prints a clear message and
  stops before running guest code. A consumer that exits during the handshake is reported with its exit code.
- `GT4_GS_FRAMES_IN_FLIGHT` = 1, 2 (default) or 3.
- Consumer death during the session: the game prints `GS renderer stopped (exit code X)`, drops later records,
  answers readbacks with zeros and halts the guest through the normal exit path (memory card flushed).

## 10. Stream file (`.gscap` version 2)

"The protocol written to a file": a 64-byte `FileHeader` (`magic "GSSTREAM"`, `file_version 2`, protocol major and
minor, `header_bytes 64`, `record_header_bytes 16`, `flags 0`, `serial[16]`, `first_frame`, 16 reserved bytes),
then the producer -> consumer records exactly as in the command ring (no wrap fillers). Written by:

- the game with `GT4_GS_CAPTURE=<file>` (optional `GT4_GS_CAPTURE_FRAMES=first:last`), in either renderer mode;
  the transport's final CONTROL shutdown is not part of the capture;
- `tools/gs_stream_record` (`--output`, or `GT4_GS_RECORD_OUTPUT`), byte for byte from the ring.

`tools/gscap_to_gsdump/gscap_to_gsdump.py` (TASK-034, extended here) converts version 1 and version 2 files to a GS
dump. Version 1 (`GT4_PGS_CAPTURE`, task/012) has no header and single-letter records.

## 11. Versioning

- A change that breaks the layout or the meaning of an existing record bumps `kVersionMajor`; consumers refuse a
  different major.
- New record types, new flag bits and new control kinds bump `kVersionMinor`. A consumer that sees an unknown
  type below 0x80 answers ERROR `unsupported_record` and skips it only if the type is in the reserved list;
  otherwise the stream is malformed. A producer therefore sends a newer record type or control kind only to a
  consumer whose READY minor version has it (1.2: CONTROL cursor and OSD_STATS; 1.3: CONTROL display_aspect).
- `.gscap` files carry the protocol version of their records.

## 12. Implementation and tests (slice191)

| Item | File |
|---|---|
| Protocol header | `native/gs_stream/gs_stream_protocol.h` |
| Transport (mapping, rings, events, peer waits, Job Object spawn) | `native/gs_stream/gs_stream_ipc.{hpp,cpp}` |
| `.gscap` v2 writer/reader | `native/gs_stream/gs_stream_file.{hpp,cpp}` |
| Game-side client and hooks | `native/gs_stream/gs_stream_client.{hpp,cpp}`; splices in `scripts/prepare_external_vulkan.py --gs-stream-hooks` (gs.cpp) and `scripts/prepare_perf_runtime.py --gs-stream` (gif.cpp, hw.cpp) |
| Test consumer (recorder / null consumer) | `tools/gs_stream_record` |
| CTest | `native_gs_stream_ipc` (10^6 random records, wrap, back-pressure, split, readback, frames in flight, spurious wakes, fatal flag, child-process handshake, handshake timeout, peer death both ways); `native_gs_stream_client` (donor capture == pcsx2 capture == recorder file, refusals, consumer death; 1.2: OSD_STATS values, a 1.2 consumer that asks for them receives about two per second and the CONTROL cursor policy, and the capture holds no OSD_STATS; 1.3: a 1.3 consumer receives CONTROL display_aspect once per change right after the VSYNC and a 1.2 consumer receives none) |
| Python | `tests/python/test_gs_stream.py`, version 2 cases in `tests/python/test_gscap_to_gsdump.py` |

## 13. Open points for the consumer side (TASK-036)

- The GS dump replayer path mapping: the TASK-034 converter writes every GIF chunk with the dump's path byte 3; a
  live consumer has the real path in each record and SHOULD feed PATH1/2/3 to the matching GIF path of its GS.
- Readbacks: answer READ_FIFO from the GS's own transfer FIFO after executing every earlier record (TASK-037).
- CSR handling on the consumer: the producer owns CSR for the EE; the consumer's copy only needs FIELD (the VSYNC
  `field`, the display field since version 1.1) and the RESET bit.
- Window: CONTROL attach_window gives the parent HWND; the consumer creates a child window and posts WINDOW events.
- Snapshot replies use the response area; the game side does not request snapshots yet (headless frame dumps in
  pcsx2 mode are a TASK-036 item).
