#pragma once

// TASK-213 Phase 2a probe: dump the MRenderContext.startPage script arguments (the context and page-root handles)
// when GT4_PAGE_PROBE is set. The native still runs; this only observes.
//
// The script native receives (gt4 unit_0022BC20, disassembly):
//   a0 = the return slot, a1 = the address of the context handle slot, a2 = argc, a3 = the argv array of page-root
//   handle pointers. `func_0022AD20(slot, a1)` casts the context wrapper to mRenderContext; `func_00232C78(slot, argv)`
//   casts each page-root wrapper to mRootWindow. The probe logs the wrapper pointers, their class descriptor and the
//   payload words, so the hValue/RefCounter relation can be derived without guessing.

#include <cstddef>
#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace gt4::page_probe {

// 0x0022BC20 on v2.00, 0 when the target has no probe.
std::uint32_t start_page_entry();

void on_start_page(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size);

// At the render functor entry: when GT4_PAGE_LAUNCH_FRAME is set, launch the hidden MonitorRoot page once at that frame
// (the probe path; a normal build has no launch env).
void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth);

} // namespace gt4::page_probe
