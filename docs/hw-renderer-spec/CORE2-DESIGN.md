# CoreGS 2.0 core rewrite: design (CORE2)

Status: C1-C4 done (IN-401..IN-404; owner test 12 on candidate 285 passed for picture faults), 2026-10-28. Next: [CORE2-ROADMAP.md](CORE2-ROADMAP.md) (owner decision "Sim, refaz"; ai-memory `decisions/coregs2-scale-only-rendering`). The
rules come from [CORE2-SPEC.md](CORE2-SPEC.md); this file maps them onto our code. Clean room: written by the TASK-027
implementer, who never opens reference sources.

## Why a new core

| Evidence | What it shows |
|---|---|
| Candidate 282, headless El Capitan race at 3x | The picture is 1x in every frame: one-native-pixel stair steps. The compose read a display shadow in 4 of 4 dumps. |
| IN-401 (clear `scaledStale` after the tone carry) | Same picture and same counters. |

In the old core:
- the 1x words are the authority;
- the scaled images are rebuilt from them on every slot load, view switch, eviction, tone run and display.

The new core keeps one image per target at scale and never rebuilds one from 1x memory (CORE2-SPEC 6.4).

## Selection and coexistence

- **Switch.** `GT4_HW_CORE=1|2` selects the core. Its default is 1 until the owner's tests pass, then 2.
  - The test script `build/hwr-play-scale.ps1` sets 2.
  - The replay tool takes `--core 2`.
- **Interface.** Both cores implement a small interface, `hwgs::RendererApi`: the methods `Backend` and
  `hw_writeback_replay` call (GifTransfer, WritePrivileged, ReadFifo, Flush, ComposeFrameGpu, ReadGpuFrame,
  SetDisplayField, ResetGs, ClearVram, Slm, counters, timings and the option setters).
- **Mapping.** `Backend` owns a `std::unique_ptr<RendererApi>`. The old `writeback::Renderer` implements it unchanged.

## What is reused

- **Front end:** `frontend/` (GIF parser, batches, transfer unit, CLUT unit hooks).
- **Memory:** `memory/` (swizzle, `BufferView`, PSM tables, transfers). The CPU shadow memory (SLM) is GS memory: EE
  uploads land in it, and target content reaches it only through readbacks (4.2).
- **Pixel and clut:** `pixel/` (`PixelState`, texel expansion) and `clut/` (palette decode).
- **Draw shaders:** `hw_vertex.vert` and `hw_pixel_uber.frag`, unchanged. They already take the target size, the
  scale, the half-pixel shift and `tex_scale` (IN-374: a texture at scale) as push constants, and do blending, depth,
  alpha test, DATE, dither, FBA and fog on r32ui storage images.
- **Display:** `display/` (layout, merge plan, deinterlace rules); the deinterlace and FXAA kernels come in step 2.
- **Vulkan:** the loader and device table (`hw_gs_vulkan_loader`, `writeback/hw_gs_vulkan_device_table`) and
  presentation (`Backend::end_frame`, present scheduler).
- **Translation:** the batch translation of the old core (vertices, sprite expansion, pixel state, scissor). It is
  copied into the new core and adapted: the target-space clip is scaled, with no 1x clip.

## New modules (`native/hw_gs/core2/`)

| File | Role |
|---|---|
| `core2_renderer.{hpp,cpp}` | CPU side: target list (2), texture lookup (3), coherence (4), display lookup (5), ageing (6), GT4 hook (7.2) |
| `core2_targets.hpp` | Target record (1.2): key, valid / dirty / drawn-since-read rectangles, flags, age |
| `core2_gpu.{hpp,cpp}` | Vulkan: images per target at scale, draw recording with the reused pipelines, texture atlas, compute passes |
| `shaders/core2_patch.comp` | Writes a 1x rectangle (decoded words from memory) into a target at scale, nearest or bilinear (4.1, 2.4b) |
| `shaders/core2_reduce.comp` | Target at scale → 1x words at floor((N + 0.5) s), for readbacks (4.2) and T8-from-target (3.5) |
| `shaders/core2_copy.comp` | Target-to-target copy at scale with translation and format change (2.4a, growth 1.4) |
| `shaders/core2_tone.comp` | GT4 channel-shuffle hook: RGB through the current palette, in place at scale (7.2) |
| `shaders/core2_compose.comp` | Display merge of two circuits read from targets at scale (5.3) |

- **Targets** are r32ui storage images holding packed GS words, as the draw shader expects, for colour and depth
  alike.
- **Depth.** The depth image holds the integer Z word. The shader compares and floors integers, which the TASK-026
  team confirmed is equivalent to the float attachment for Z24 (Q-GSDX-COMPARISON 20).
- **Textures:**
  - **Target hit (3.2):** the target's rectangle is copied into the scaled texel image and sampled with
    `tex_scale = s`.
  - **Memory decode (3.7):** decoded on the CPU from the SLM into the 1x atlas, cached per page version.

## Steps and acceptance

- **C1, skeleton.**
  - `RendererApi`, the switch, the replay `--core 2`, the target list and draws at scale.
  - Display lookup and the compose with 480p present.
  - Acceptance: the race capture (`private/slice242-capture/race480p-10000.gscap`) replays to a picture with
    geometry at scale. `--check-presented` reports no 1x frame for untextured geometry.
- **C2, textures and memory.**
  - Memory decode with page invalidation, target hits, EE uploads as dirty rectangles, readbacks with the point
    reduction, new-target initialisation from overlaps (2.4a, 2.4b).
  - Acceptance: the race capture is textured, and the FIFO hash at `--scale 1` equals the CPU reference's.
- **C3, GT4.**
  - The channel-shuffle hook (7.2), the CT16 display conversion with fine dither (7.4), and auto-flush for
    self-sampling sprites (7.1).
  - Acceptance: race colours match the old core at 1x within dither, with no 1x pixels at 3x.
- **C4, owner test.**
  - A frozen candidate in the game, on the owner baseline (1920x1080 window, 16:9, 3x, 480p race).
- **Then.** Menus at 480i (FFMD offset, deinterlace, double buffers, T4/T8 menus), then CORE2-SPEC step 3, then
  performance and fractional scales.

## Gates that change

- **Retired for core 2.** The old scale tests require the scaled run's SLM to equal the 1x run's: `hw_scale_exact`,
  `hw_scale_display_shadow`, `hw_scale_target_extent`, `hw_scale_fullscreen_passes` and `hw_cache_equals_writeback`.
  Core 1 keeps them.
- **New for core 2:**
  - the FIFO (readback) hash at 1x against the CPU reference;
  - `--check-presented` with zero 1x frames at scale;
  - fixtures for target lookup, dirty rectangles, the reduction rule and the tone hook.
