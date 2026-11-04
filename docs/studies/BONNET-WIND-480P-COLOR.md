> **Study by Luan Antony**, from pull request #171 (issue #135), imported on 2026-11-04 at the owner's word.
> Task numbers inside are the author's own numbering (TASK-140); in our TASKS.md this study belongs to TASK-163.
> It is a study, not a decision: anything it proposes is checked again before it is built.

# TASK-140 — Hood Camera Wind Noise Attenuation & 480p Full Color Range Study

**Issue:** [#135](https://github.com/moesuito/gt4-staticrecomp/issues/135) — *"Option to disable Wind sound in Bonnet Cam - Full Range of color on 480p"*  
**Date:** 2026-11-01  
**Status:** Study & Architecture Specification  
**Clean-room Status:** Fully compliant (No PCSX2-tainted code or private emulator sources opened; GT4 decompilation symbols and SCUS-973.28 ELF structures used as references)

---

## 1. Executive Summary & Problem Formulation

In GitHub Issue #135, user `shadowcla02-cloud` reported two distinct audio-visual concerns affecting gameplay comfort and image quality in GT4Recomp:

1. **Acoustic Fatigue from Bonnet/Hood Camera Wind Rush:**  
   When driving at high velocities (above 180–200 km/h) using the bonnet/roof camera, the game plays a continuous, high-amplitude aerodynamic wind noise synthesis/sample loop. This wind sound frequently drowns out engine audio, tire squeal, and ambient opponent sounds, leading to player fatigue during extended high-speed driving sessions (e.g., Le Mans Sarthe, Special Stage Route X/Test Course, and Nürburgring Nordschleife).

2. **Perceived Color Gamut and Banding Degradation in 480p Progressive Mode:**  
   When progressive scan (480p / 525p) mode is engaged via GT4 Options or `--force-480p`, the perceived color depth and dynamic range appear visibly degraded compared to the 480i interlaced mode. Players observe noticeable posterization, stepped gradients in skies and shadows, and reduced color vibrancy.

This technical study investigates the root causes of both phenomena in the guest engine and host rendering/audio pipeline, and outlines concrete, safe architectural solutions for future implementation.

---

## 2. Technical Investigation: Bonnet Camera Wind Noise

### 2.1 GT4 Sound Architecture & Camera-Coupled Audio
Gran Turismo 4 features a sophisticated multi-bus sound engine partitioned across the Emotion Engine (sound event dispatch, dynamic spatialization, physics-to-audio parameter mapping) and the IOP/SPU2 (sample decompression, pitch modulation, voice allocation, reverb, and multichannel mixing).

In race mode, audio processing is orchestrated by the following core modules:
- `RaceCarSound` (`0x003918C8`–`0x00392FC8`): Manages vehicle-centric sound effects, including engine RPM pitch mapping, induction/exhaust samples, transmission whine, and aerodynamic flow.
- `RaceCourseSound` (`0x00398B58`–`0x00399300`): Manages trackside environmental audio, echo surfaces, and tunnel reverbs.
- `RaceCarSound::visual_` (`0x006213E8`) and `RaceCarSound::visual_volume_` (`0x006213DC`): Store the active camera perspective index and its associated external master attenuation level.
- `RaceCarSound::dive_` (`0x006213EC`) and `RaceCarSound::setDive` (`0x00391E50`): Modulate voice volumes and low-pass filtering based on cockpit dampening or external exposure.

### 2.2 Aerodynamic Wind Noise Mechanism
The wind effect is synthesized via a dedicated SPU2 looped noise/air sample whose pitch and master gain are dynamically driven by:
$$\text{Gain}_{\text{wind}} = f(v_{\text{car}}) \times K_{\text{camera}}$$
where:
- $v_{\text{car}}$ is the car's forward linear velocity along its longitudinal axis.
- $K_{\text{camera}}$ is the camera perspective exposure coefficient:
  - Behind/Chase Cam ($K \approx 0.25$–$0.35$): Wind is subordinate to exterior engine exhaust notes.
  - Bumper/Bonnet Cam ($K \approx 0.85$–$1.00$): The microphone position simulates direct turbulent airflow over the front fascia and cowl, producing heavy broadband noise.

### 2.3 Proposed Remediation Paths

#### Path A: Guest EE Memory/Instruction Hook (Recommended)
By intercepting the camera evaluation routine in `RaceCarSound__setDive` (`0x00391E50`) or zeroing/scaling `RaceCarSound::visual_volume_` specifically when the camera index denotes bonnet/roof view:
- The wind voice volume can be multiplied by a configurable factor $\alpha \in [0.0, 1.0]$.
- When set to `0.0`, the aerodynamic wind voice is completely muted while preserving all mechanical car sounds (engine, turbo blow-off, transmission, tire grip/loss).
- Implementation is lightweight, deterministic, and can be activated via host environment variable `GT4_BONNET_WIND_VOLUME=0` or launcher configuration.

#### Path B: SPU2 Voice-Level Host Filter
In the SPU2 core emulation (`host_io.cpp` / donor SPU2 runtime):
- Detect the specific SPU2 voice assigned to the looping wind noise sample (by inspecting ADPCM sample header address or channel pitch characteristic).
- Attenuate that specific voice during audio mixing.
- *Disadvantage:* Identifying the dynamic voice allocation across car classes and race types is brittle compared to patching the EE gain calculation at the source.

---

## 3. Technical Investigation: 480p Color Precision & Gamut

### 3.1 Framebuffer Format Discrepancy: PSMCT24 vs PSMCT16
The core root cause for the visual difference between 480i and 480p lies directly in Polyphony Digital's VRAM allocation strategy on the PlayStation 2 hardware:

| Mode | Scan Mode | Native Resolution | Color Format | Bits per Channel | Total Colors | VRAM Footprint |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **480i (Menus & Races)** | Interlaced Field | $640 \times 224$ | **PSMCT24** (`psm = 1`) | 8-bit RGB ($8:8:8$) | 16,777,216 | `0x69000` words |
| **480p (Progressive Scan)** | Progressive Frame | $640 \times 448$ | **PSMCT16** (`psm = 2`) | 5-bit RGB ($5:5:5:1$) | 32,768 | `0x69000` words |
| **480p Photo / Movie Return** | Progressive Frame | $640 \times 448$ | **PSMCT24** (`psm = 1`) | 8-bit RGB ($8:8:8$) | 16,777,216 | `0x8C000` words |

#### Why did the developers use PSMCT16 in 480p?
The PS2 Graphics Synthesizer has only **4 MiB of embedded VRAM** (1,048,576 words).
- When rendering 480i fields, the vertical resolution is halved to 224 lines. Two $640 \times 224$ 24-bit buffers require exactly:
  $$2 \times (640 \times 224 \times 3\text{ bytes}) = 860,160\text{ bytes} = 215,040\text{ words} \approx 0x69000\text{ words}$$
- When switching to 480p progressive, the frame height doubles to 448 lines. To maintain the exact same VRAM footprint ($0x69000$ words) so as not to reduce the VRAM available for track textures, car textures, and Z-buffers, Polyphony Digital **halved the pixel depth from 24-bit to 16-bit** (`PSMCT16`).

### 3.2 Consequences on Modern Displays
1. **Color Quantization & Banding:**  
   In 16-bit color, each RGB channel has only $2^5 = 32$ quantization steps, compared to $2^8 = 256$ steps in 24-bit mode. Smooth gradients (such as sky tones at sunset, twilight lighting, tarmac reflections, and car paint specular highlights) collapse into visible discrete color bands.
2. **Missing CRT Temporal & Spatial Blurring:**  
   On standard-definition CRT monitors, phosphor persistence and analog scanline defocus naturally softened 16-bit color stepping and hardware dither patterns (`DTHE`). Modern high-contrast LCD and OLED monitors display every quantized pixel with pin-sharp precision, making the banding severe and jarring.
3. **Limited Dynamic Range (BT.601 Studio Swing vs Full Range):**  
   Standard DTV 480p component video specifications historically followed ITU-R BT.601 with limited luminance ranges ($[16, 235]$). In contrast, the PC Vulkan presentation pipeline operates in full $[0, 255]$ dynamic range (`VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`). Uncompensated mapping between limited-range assets and full-range displays accentuates crushed blacks and blown highlights.

---

## 4. Proposed Architectural Solutions for 480p Color

### Approach 1: 480p PSMCT32 / 24-Bit True-Color Framebuffer Upgrade (Engine Mod)
In GT4's CRT mode selection (`func_00436640` / `MOption.applyRaceDisplayMode`), race display index 1 requests:
```text
func_001074D0(&D_006184D0, psm=2, row=6, w=640, h=448)
```
If patched to request `psm=1` (PSMCT24/PSMCT32):
- The game will call `func_00106EE8(state, 1, 640, 448, 0)`, allocating full 24-bit $640 \times 448$ display buffers (identical to the layout already proven in Photo Pause mode).
- Footprint expands from `0x69000` to `0x8C000` words.
- *Feasibility Assessment:* On host renderers (Vulkan / GSdx / CoreGS 2.0), emulated GS VRAM can easily accommodate the additional `0x23000` words without memory exhaustion, eliminating color banding at the source!

### Approach 2: Host-Side Post-Processing Debanding & Dither Filter
Within the Vulkan presentation blit (`native/native_vulkan.cpp`) or GSdx post-processing shader:
- Apply a temporal or spatial debanding filter (similar to mpv `deband` or reshade deband).
- Reconstruct smooth color gradients from the 16-bit quantized image before final presentation to the swapchain.

### Approach 3: Swapchain Dynamic Range & Tone Curve Alignment
Ensure swapchain blit shader applies correct color space expansion without clipping $[16, 235]$ broadcast values into crushed shadows.

---

## 5. Phased Implementation Roadmap (TASK-140)

- **Phase 1 — Bonnet Camera Wind Attenuation:**
  - Locate `RaceCarSound` wind gain calculation in reconstructed ELF.
  - Implement configurable gain factor (`GT4_BONNET_WIND_VOLUME`).
  - Add Launcher setting: *"Hood Camera Wind Audio: Default / 50% / Muted"*.

- **Phase 2 — 480p True-Color PSMCT32 Experimentation:**
  - Build candidate ELF patch testing `psm = 1` for CRT row 6.
  - Verify texture allocation stability during 6-car AI races on complex tracks (Tokyo R246, Nürburgring).
  - Verify render target readbacks (flares, motion blur passes).

- **Phase 3 — Debanding & Full-Range Presentation Option:**
  - Add optional deband post-processing shader in native Vulkan presentation pipeline.
  - Add Launcher toggle for color depth enhancement.

---

## 6. Acceptance Criteria

1. **Audio Acceptance:**
   - Setting `GT4_BONNET_WIND_VOLUME=0` silences the high-speed aerodynamic rush in bonnet view.
   - Engine notes, exhaust reverberation, curb strikes, and tire screeches remain at 100% fidelity.
   - Chase camera and replay camera sound profiles remain completely unaffected.

2. **Visual Acceptance:**
   - Color banding on sky domes and vehicle paint in 480p mode is measurably reduced or eliminated.
   - Headless image comparisons verify zero unintended corruptions in HUD, text rendering, or post-process passes.
