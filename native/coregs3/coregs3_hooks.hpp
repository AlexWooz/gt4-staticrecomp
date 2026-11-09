// TASK-090 CoreGS 3.0, phase 1: the observe-only EE hooks (docs/COREGS3-PLAN.md §5.2 K1, K3, K4, K5).
//
// The hooks are host calls inserted into the translated units by scripts/coregs3_units.py, plus one VBlank call from the
// host loop. They read guest memory and the VU0 macro registers; they never write guest memory, never call guest code and
// never touch the guest cycle budget (the units' `c.budget` accounting is untouched), so the simulation digest, the race
// clock and every read-back stay identical. `GT4_COREGS3=off` (the default) makes each entry point one predictable
// branch.
#pragma once

#include <cstdint>
#include <cstdio>

#include "coregs3_scene.hpp"

namespace ps2 {
struct Cpu;
}

namespace gt4::coregs3 {

// Reads GT4_COREGS3 (off|shadow|overlay|present), GT4_COREGS3_CAPTURE (a .gt4scene path; `shadow` only) and
// GT4_COREGS3_CAPTURE_VBLANKS (a-b, inclusive). Called once by the host before the guest starts. An unknown mode refuses
// the start, like GT4_FORCE_480P does; `overlay` refuses until the in-process seam exists. `present` is the debug-only
// live layer of docs/COREGS3-HOOKS.md §10.3: it publishes the complete frame ring and the HWR backend draws it over the
// composed image.
void configure_from_environment();
void shutdown();

Mode mode() noexcept;
bool active() noexcept;  // mode != off
void at_vblank(std::uint64_t vblank) noexcept;

const CaptureCounters &counters() noexcept;
void print_report(std::FILE *out) noexcept;

// Entry points called by the translated units and by the host loop. `before`/`after` bracket the `jal 0x0049D308` of
// ModelSet2___render at 0x00455A5C/0x00455A64; `car_pass_begin`/`end` are the block starts of L_0033EE28 and
// L_0033EE54 in func_0033EAB8; `projection_set`/`modelview_set` are L_0033EBB4 and L_0033EBF4.
void before_shape_call(ps2::Cpu &cpu) noexcept;
void after_shape_call(ps2::Cpu &cpu) noexcept;
void car_pass_begin(ps2::Cpu &cpu) noexcept;
void car_pass_end(ps2::Cpu &cpu) noexcept;
void projection_set(ps2::Cpu &cpu) noexcept;
void modelview_set(ps2::Cpu &cpu) noexcept;

// Test seam: the camera record captured at K3/K4 for the frame being built (raw guest words; see CameraSnapshot).
const CameraSnapshot &current_camera() noexcept;

}  // namespace gt4::coregs3
