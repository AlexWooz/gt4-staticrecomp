# CoreGS 2.0: the flare REF → table word → sprite mapping, and why the per-invocation marker does not carry a light

TASK-215 S4, measured 2026-11-11 on the night race (Hong Kong Reverse) at 3x, VBlank 9,600, the same recording and
VBlank as the earlier forced-word frames. Diagnostics only: `GT4_HW_FLARE_PATCH_TRACE` (REF packet, its four words, the
next MSCAL), `GT4_FLARE_REF_HIDE=<i>` or `=<i,j>` (zeroes the n-th REF packet's 16 bytes in the VU1's view only, after the
table copy, so the game's own readback keeps the real values), `GT4_CORE2_FLARE_TRACE_DRAWS` (every draw of the flare job
with its rectangle). Nothing here is enabled by default.

## 1. The mapping is proven: two REFs per light, one table word per light

Frames at VBlank 9,600, all compared against the all-visible frame (`GT4_FLARE_TABLE_FORCE=255`):

| test | pixels gone | box (3x frame) |
|---|---|---|
| table word 0 = 0 | 117 | x 934..960, y 239..262 |
| table word 1 = 0 | 117 | x 908..936, y 240..269 |
| table word 2 = 0 | 0 | - |
| table word 3 = 0 | 0 | - |
| REF 0 hidden | 61 | x 936..959, y 245..262 (outer part of word 0) |
| REF 1 hidden | 76 | x 943..951, y 248..258 (inner part of word 0) |
| REF 2 hidden | 68 | x 911..933, y 241..260 (outer part of word 1) |
| REF 3 hidden | 76 | x 916..925, y 248..258 (inner part of word 1) |
| **REF {0,1} hidden vs word 0** | 118 vs 117 | x 918..960 vs x 934..960: **3 px only in one, 2 in the other** |
| **REF {2,3} hidden vs word 1** | 118 vs 117 | x 908..948 vs x 908..936: **2 px only in one, 1 in the other** |
| control: REF {0,1} vs word 1 | - | 115 / 114 px different (disjoint) |
| control: REF {2,3} vs word 0 | - | 116 / 115 px different (disjoint) |

So, proven:

| REF index | table word | flare cluster (3x) | GS pixels | part |
|---|---|---|---|---|
| 0 | **0** | x 934..960, y 239..262 | ~311..320, 80..87 | outer mesh |
| 1 | **0** | x 943..951, y 248..258 | ~314..317, 83..86 | inner mesh |
| 2 | **1** | x 908..936, y 240..269 | ~303..312, 80..90 | outer mesh |
| 3 | **1** | x 916..925, y 248..258 | ~305..308, 83..86 | inner mesh |
| 4..n-1 | unused / other lights | - | - | - |

The 2-3 px residual between a REF pair and its word (out of ~117) is the only mismatch; the cross-pair controls are fully
disjoint. The four words are per light, and each light is fed by **two** REFs (an outer and an inner mesh).

**The REF count is not fixed.** Over one run (1,892 flare jobs): 2 REFs in 163 jobs, 4 in 932, 6 in 89, 8 in 708 - always
even, i.e. 1 to 4 visible lights. A design with eight fixed slots or one marker per eight REFs is wrong.

## 2. The structure that blocks "one marker per invocation"

`GT4_HW_FLARE_PATCH_TRACE` on the same frame (4 REFs):

```
[mtvu] flare patch job=769514 packet=206 qwc=1 at=0 mscals=0 first=00000000 ref=0 words=ff0000fd,ff0000fd,90327b25,a2327f28 next_packet=542 next_code=14 next_addr=00008014
[mtvu] flare patch job=769514 packet=216 qwc=1 at=0 mscals=0 first=00000000 ref=1 words=ff0000fd,ff0000fd,90327b25,a2327f28 next_packet=542 next_code=14 next_addr=00008014
[mtvu] flare patch job=769514 packet=234 qwc=1 at=0 mscals=0 first=00000000 ref=2 words=ff0000fd,ff0000fd,90327b25,a2327f28 next_packet=542 next_code=14 next_addr=00008014
[mtvu] flare patch job=769514 packet=244 qwc=1 at=0 mscals=0 first=00000000 ref=3 words=ff0000fd,ff0000fd,90327b25,a2327f28 next_packet=542 next_code=14 next_addr=00008014
```

- Each REF is a one-qword packet with no MSCAL inside (as measured 2026-11-10).
- **All four REFs of the frame are consumed by the same MSCALF** at packet 542: there is no per-REF and no per-light
  invocation to mark.
- Each REF writes the same 16 bytes (`ff0000fd, ff0000fd, …`) into a **different VU1 window**; the light index lives in
  the window's address, which never appears in the draw stream.
- The "flare" is **not one quad per light**: `GT4_CORE2_FLARE_TRACE_DRAWS` shows ~250 tiny triangles per batch (2-3 px
  each) around the two clusters, and the two lights' meshes **interleave in draw order** (≈14-draw groups alternating
  between the light-0 cluster at x≈312 and the light-1 cluster at x≈306, batch 1 of the job).

Consequence: a `FlareLight{index}` marker placed before the invocation's PATH1 records would tag **both lights' draws with
one index**, and the draw order cannot separate them. The Step-2 design as written cannot be exact.

## 3. What would unblock it

1. **A per-draw light index from the VU1.** Either a VU1 program patch that writes the light index into a vertex field the
   pipeline does not read today (e.g. the free byte at `GpuVertex` offset 29, next to `fog`), or an XGKICK/marker record
   per light inside the program. Both need the microcode documented (TASK-026 style) and are a code patch, not a data copy.
2. **Re-examine whether the GPU multiply is needed at all.** P1 showed the flare's pixels follow the table value linearly,
   and the values reach the VU1 through these REF windows (the patch copies them from the GS table), so **the VU1 already
   applies the visibility**. The 5.77 / 9.85 ms cost at 10x/12x is the **EE readback wait**, not a missing GPU multiply.
   The cheaper experiment is to answer that read without a GPU round trip (the `zeroRead_` path already exists for hidden
   flares) and check what the game does with the answer - that is a different, smaller change than a shader variant, and
   it must be priced before anyone writes shader code.

## 4. Path 2 priced: attack the EE readback round trip (2026-11-11, docs only, no code)

**Question.** Can the 5.77 / 9.85 ms `flare` wait at 10x / 12x be removed without the per-light GPU modulation of path
1, by making the 2 KiB read cheaper or unnecessary?

**Answer: no exact same-frame mechanism exists.** The read's data is a **GPU occlusion result**, and the EE makes no
decision from it, so there is nothing to replace it with on the CPU without changing the answer.

### 4.1 What the read is for

- **C** - the 2 KiB (128 qword) read is a GS local-to-host transfer of a 64x8 CT32 rectangle, answered by
  `Backend::read_transfer_fifo` (`native/hw_gs/hw_gs_backend.cpp:1901`) → `Renderer::ReadFifo`
  (`native/hw_gs/core2/core2_renderer.cpp:5525`) → `Readback` (`:2844-2908`) → `Gpu::Reduce` (`core2_gpu.cpp`, reduce
  compute pass) → `submit(true)`.
- **C** - the table's content is produced by the **GS**: each light's depth-tested square is reduced by rasterisation to
  one visibility texel (owner-confirmed finding, `decisions/gs-readback-plan.md`: "The GS reduces each light's
  depth-tested square to one visibility pixel in a 64x8 table. The 2 KiB table is copied back into memory. A DMA REF tag
  `0x30000001` points VU1 at it, and VU1 scales the flare sprite in the same frame. **The EE never reads the pixels.**").
- **H** - the EE's only use of the bytes is transport: it hands them to the VU1 through the REF windows, which the VIF
  patch already short-circuits (`apply_table_patches`, `native/mtvu_runtime.cpp:227`, IN-522, and the deferred read at
  `hw_gs_stream_sink.cpp:1045-1050`).
- **Unknown** - whether any EE code reads the table for a second purpose. The decomp tree at
  `private/slice151-research/gt4-decomp` has no libgraph read path (only `pgluTexImage.cpp` mentions the layouts), and no
  run has traced a consumer. This does not block path 2: the read must be exact regardless of who consumes it.

### 4.2 The existing zero/hidden machinery cannot be reused in normal mode

- **C** - hidden flares (`hideFlares_`, `core2_renderer.cpp:589-593`; `GT4_GS_HIDE_FLARES`, `GT4_CORE2_PERF_FLARES`,
  `GT4_CORE2_PERFORMANCE`) set `zeroRead_` at `:5540-5542`, **skip** the readback there and zero the bytes at
  `:5570-5574`; `flare_table_answered_at_once()` (`hw_gs_stream_sink.cpp:978-996`, IN-477) answers zeros **before** the GS
  worker reaches the read, so the EE never waits (10-16 ms per VBlank saved on the i5-7200U, per its comment).
- **C** - the normal-mode "answer late" path (`read_flare_table`, `hw_gs_stream_sink.cpp:1045-1064`, IN-514) returns the
  last finished table past a deadline; the owner already rejected that class of answer for normal mode (decision 0073;
  `gotchas/flare-readback-one-frame-lag.md`: a lamp hidden by a wall keeps its flare for one frame).
- **Therefore the stop condition in the brief applies**: every way to avoid the wait while keeping the value is either
  hidden (zeros) or one frame late (the previous reply), and both are rejected. Path 2 is priced here, not implemented.

### 4.3 Why the wait cannot be narrowed instead

- **C** - `Gpu::Reduce` appends its command buffer after `flush_draws()`/`full_barrier()` and calls `submit(true)`
  (`core2_gpu.cpp`); `submit` submits on the single queue and waits its fence. A Vulkan fence signals only after **all
  earlier submissions on that queue**, so waiting on the reduce waits for the frame's draws - that is the 5.77 ms at 10x.
- **C** - `Readback` only walks targets whose `drawn` area overlaps the read span (`core2_renderer.cpp:2844-2868`), so no
  redundant reduction is being done: the wait is the occlusion result itself.
- **H** - a fence limited to the submission that produced the table would not help: the table's squares are depth-tested
  against the scene depth, so they are drawn after the depth writes; at 10x that is most of the frame's GPU work.
- **C** - the only same-frame way to keep the value without the round trip is for the GPU to consume it: that is path 1
  (the shader samples the 64x8 target), and it needs the per-draw light index that the VU1 does not currently carry
  (section 2).

### 4.4 Picture gate (for whichever route is chosen)

Night 3x and 6x, Arcade, and the 10x/12x stress points; forced-word cross-check at three VBlanks (words 0/1 hidden, the
all-visible reference) to prove the shader reads the same texel the CPU path would; flare pixels within 1 LSB, every other
pixel byte-identical; `flare_readbacks` = 0 for the GPU route. **Risk:** the table is an occlusion result sampled late in
the frame; any sampling point that differs from the reduce changes the frame the visibility belongs to (the accepted
one-frame artefact of Performance mode is exactly that error made visible).

### 4.5 Cost/benefit

| route | wait removed at 10x / 12x | wait removed at 3x / 6x | effort | exact |
|---|---|---|---|---|
| path 2 as specified (cheaper read) | **0** - no mechanism | 0 | 0 d (does not exist) | n/a |
| path 2 variants (zeros, previous frame) | 5.77 / 9.85 | 0.15 / 0.95 | < 0.5 d | **no** (decision 0073) |
| **path 1** (VU1 light index + shader) | 5.77 / 9.85 | 0.15 / 0.95 | 3-5 d, needs the microcode study | yes, with the gate |

**Recommendation:** do **not** spend on path 2; it has no exact implementation. The only exact same-frame route to the
5.77 / 9.85 ms is path 1, and its first step is a **microcode study of the flare loop** (find a free vertex field the VU1
can write, or an XGKICK marker per light) - not a patch, and not a shader, until that mapping exists. If the owner prefers
not to touch VU1 code, the honest alternative is to keep the readback: 0.15 / 0.95 ms at 3x / 6x is the price of exact
flare occlusion, and it only becomes 5.77 / 9.85 ms at 10x / 12x, which are diagnostic points.

## 5. Status

Step 1 is delivered (the table above) and **path 2 is priced (section 4): it has no exact same-frame mechanism**, because
the table is a GPU occlusion result and the read must drain the frame's draws; zeros and one-frame-late answers are
decision 0073's rejected trade-off. **Step 2 was not started**, per the instruction to stop when the mapping does not
carry the design: the REF → word mapping is proven, but the per-invocation marker cannot attribute a draw to a light, so
the shader variant has no source for its word index. The diagnostics stay in the tree, off by default
(`GT4_FLARE_REF_HIDE`, `GT4_CORE2_FLARE_TRACE_DRAWS`), and the default path was verified byte-identical
(night 3x VBlank 9,600: 1/1 frames identical with the diagnostics added).
