# CoreGS 2.0: which stage is the real bottleneck (TASK-215, 2026-11-11)

Question: on the CPU-bound scenes, is the limiter the GS worker's own work, the VU1 worker, or the EE? **No optimization
and no new counter this round**: everything below comes from the last reported 600-VBlank interval of runs already in
`build/evidence/` (`*_base_*` for Spec II, `churn-*` for license/night), with the same trace tooling. Labels C/H/U.

## 1. The pipeline per VBlank (ms; `wall` is the same interval's `[core2] wall`; shares are of that wall)

| scene | wall | GS worker (share) | EE dispatch (share) | VU1 worker busy (share) | GPU busy | mtvu_wait | gpu_wait | drain | read_fifo | vsync_wait |
|---|---|---|---|---|---|---|---|---|---|---|
| license B-1 1x | 8.58 | **7.67 (89 %)** | 6.46 (75 %) | 3.67 (43 %) | 1.28 | 0.230 | 0.05 | 0.00 | 0.00 | 1.74 |
| night race 3x | 6.90 | **6.70 (97 %)** | 3.11 (45 %) | 3.23 (47 %) | 2.26 | 0.039 | 0.08 | 0.00 | 0.00 | 1.74 |
| Opera Paris 3x | 6.60 | **6.44 (98 %)** | 3.71 (56 %) | 2.31 (35 %) | 1.38 | 0.000 | 0.35 | 0.00 | 0.00 | 1.24 |
| S.S. Route 5 3x | 7.78 | **7.61 (98 %)** | 3.91 (50 %) | 2.48 (32 %) | 2.09 | 0.000 | 0.19 | 0.00 | 0.00 | 2.35 |
| Tsukuba Wet 3x | 8.57 | **8.40 (98 %)** | 3.44 (40 %) | 2.26 (26 %) | 2.36 | 0.000 | 0.29 | 0.00 | 0.00 | 2.60 |
| George V rally 3x | 9.42 | **9.22 (98 %)** | 3.70 (39 %) | 3.41 (36 %) | 2.52 | 0.000 | 0.16 | 0.00 | 0.00 | 3.64 |
| George V reference 3x | 13.27 | **13.00 (98 %)** | 4.54 (34 %) | 5.65 (43 %) | 3.69 | 0.000 | 0.13 | 0.00 | 0.00 | 6.38 |

EE, VU1 and GS run concurrently, so their shares overlap; the GS worker is the single longest stage in every scene and the
EE's `vsync_wait` (1.2-6.4 ms) is the idle the EE has left after its own work.

**VU1 worker** (`[mtvu] worker ms/frame`): busy 2.3-5.6 ms (26-47 % of the frame), of which `vu1_program` 2.1-5.3 and
`output_path12` 0.13-0.36; idle 0.4-4.4 ms on five of seven scenes, **0.085 ms (George V rally) and 0.168 ms (reference)** -
there the VU1 has almost no slack, but the GS still never waits on it.

**GS waits by source:** VU1 output `mtvu_wait` **0.000-0.230 ms**; GPU submission/readback `gpu_wait` **0.05-0.35 ms**
(by reason: flare 0.00-0.19, before-write 0.00-0.35, clut 0.00-0.07, keep/drain/read_fifo 0.00); submission drain
`drain_wait` 0.00; LOCAL->HOST `read_fifo` 0.00. **The GS worker is busy, not waiting.**

## 2. GS stage split, same intervals (exclusive ms/VBlank)

| scene | gif_parse | batch_own | draw | clut | swizzle | translate | texture | readback | GS total |
|---|---|---|---|---|---|---|---|---|---|
| license 1x | **3.36** | 0.63 | 0.94 | 0.48 | 0.48 | 0.45 | 0.19 | 0.08 | 7.67 |
| night 3x | 1.75 | 0.80 | 1.33 | 0.16 | 0.82 | 0.55 | 0.29 | 0.30 | 6.70 |
| Opera 3x | 2.29 | 0.76 | 0.79 | 0.26 | 0.75 | 0.39 | 0.19 | 0.51 | 6.44 |
| S.S. Route 5 3x | 2.59 | 0.84 | 1.15 | 0.34 | 0.68 | 0.49 | 0.25 | 0.45 | 7.61 |
| Tsukuba Wet 3x | 2.75 | 0.96 | 1.36 | 0.41 | 0.72 | 0.63 | 0.28 | 0.50 | 8.40 |
| George V rally 3x | 3.42 | 1.00 | 1.35 | 0.46 | 0.76 | 0.64 | 0.29 | 0.60 | 9.22 |
| George V reference 3x | 4.76 | 1.36 | 2.35 | 0.59 | 0.78 | 1.12 | 0.39 | 0.79 | 13.00 |

## 3. VU1 AOT coverage: complete (C)

`[perf] vu1_aot` in all seven runs: **`interpreted_calls=0`** against 8.9M-35.9M `compiled_calls`, `budget_resumes=0`,
`identity_rejects=0`. There is **no interpreter fallback on the hot path**. (`vu0_aot interpreted_calls` is large because
VU0 runs in macro mode inside the translated EE code - `[perf] vu_scope=... VU0_macro_in_EE` - not a fallback.)

## 4. Verdict

- **The GS worker's own work is the bottleneck: 89-98 % of the frame in every scene**, and it is *busy* - its waits are
  0.00-0.35 ms in total (VU1 0.00-0.23, GPU 0.05-0.35, drain and read_fifo 0.00).
- **The VU1 is not a hidden bottleneck**: 26-47 % of the frame, idle on five scenes, near-saturated only on George V
  (0.085/0.168 ms idle) where the GS still never waits on it. AOT is complete, so there is nothing to gain there.
- **The EE is the third stage** (34-75 %); on the license it is nearly co-dominant (6.46 against the GS's 7.67), so the
  license is the one scene where an EE-side change could matter.
- The GPU has slack everywhere (busy 1.3-3.7 ms, idle per drain 1.1-3.2 ms).

## 5. The three aggressive options this points to

| option | evidence | expected impact | smallest prototype |
|---|---|---|---|
| **(a) multi-threaded GS pipeline** (GIF front end on its own thread, bounded queue to translate/draw) | the GS is 6.4-13.0 ms of a 6.6-13.3 ms frame; its two halves are `gif_parse` 1.75-4.76 and the rest (draw+batch+translate+clut+swizzle) 4.1-8.2 | overlap of the smaller half: **1.5-3 ms** (H), the only option that touches the bottleneck itself | one scene (license), front end thread + ring, switch off by default; 3-5 d |
| (b) VU1 AOT / coverage | already **0 interpreted calls** in every run | **0** | none - closed |
| (c) GPU offload of texture/decode | `texture` 0.19-0.39 + `swizzle` 0.68-0.82 + `clut` 0.16-0.59 = **1.0-1.8 ms (13-20 % of the GS)**; the GLM already decodes some textures on the GPU | **1.0-1.8 ms** (C for the stage sizes, H for what is removable) | move the remaining CPU swizzle of uploads into a compute pass behind a switch; 2-4 d |

**Recommendation:** (a) is the only option aimed at the measured bottleneck, but it is the invasive one (the GS state
machine and the writeback/readback order); (c) is smaller and safe but capped at ~1.5 ms. If the owner wants one aggressive
step, (a) with the license as the prototype scene and the identity gate already in place.
