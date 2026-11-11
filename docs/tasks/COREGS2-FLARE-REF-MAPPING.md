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

## 4. Status

Step 1 is delivered (the table above). **Step 2 was not started**, per the instruction to stop when the mapping does not
carry the design: the REF → word mapping is proven, but the per-invocation marker cannot attribute a draw to a light, so
the shader variant has no source for its word index. The diagnostics stay in the tree, off by default
(`GT4_FLARE_REF_HIDE`, `GT4_CORE2_FLARE_TRACE_DRAWS`), and the default path was verified byte-identical
(night 3x VBlank 9,600: 1/1 frames identical with the diagnostics added).
