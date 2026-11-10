// TASK-213 Phase 2a probe. See native/page_probe.hpp.
#include "page_probe.hpp"

#include "game_target.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gt4::page_probe {
namespace {

std::uint32_t u32(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  if (at > size || size - at < 4) return 0;
  std::uint32_t value = 0;
  std::memcpy(&value, ram + at, 4);
  return value;
}

void w32(std::uint8_t *ram, std::size_t size, std::uint32_t at, std::uint32_t value) {
  if (at > size || size - at < 4) return;
  std::memcpy(ram + at, &value, 4);
}

std::uint32_t first_child(const std::uint8_t *ram, std::size_t size, std::uint32_t base) {
  return u32(ram, size, base + 0xA4);
}

std::uint32_t next_sibling(const std::uint8_t *ram, std::size_t size, std::uint32_t child) {
  return u32(ram, size, child + 0x5C);
}

std::string string_at(const std::uint8_t *ram, std::size_t size, std::uint32_t dat) {
  if (!dat || dat < 16 || dat >= size) return {};
  const std::uint32_t length = u32(ram, size, dat - 16);
  if (length > 200 || dat + length > size) return {};
  return std::string(reinterpret_cast<const char *>(ram + dat), length);
}

void dump_object(const char *name, std::uint32_t base, const std::uint8_t *ram, std::size_t size) {
  if (base < 0x100000 || base + 0x90 > size) {
    std::fprintf(stderr, "[page-probe] %s 0x%08X is outside RAM\n", name, base);
    return;
  }
  std::fprintf(stderr, "[page-probe] %s 0x%08X vtable 0x%08X:\n", name, base, u32(ram, size, base + 4));
  for (std::uint32_t off = 0; off < 0x90; off += 4)
    std::fprintf(stderr, "[page-probe]   +0x%02X 0x%08X\n", off, u32(ram, size, base + off));
}

std::uint32_t call_guest(ps2::Cpu &cpu, std::uint32_t fn, std::uint32_t depth, std::uint32_t argc,
                         const std::uint32_t *args) {
  if (!fn || !ps2::has_code(fn)) return 0;
  const ps2::Cpu saved = cpu;
  for (std::uint32_t i = 0; i < argc && i < 4; ++i) cpu.r[4 + i].w[0] = args[i];
  cpu.r[31].w[0] = 0;
  ps2::call(cpu, fn, depth + 1u);
  const std::uint32_t result = cpu.r[2].w[0];
  cpu = saved;
  return result;
}

bool has_title_monitor(const std::uint8_t *ram, std::size_t size, std::uint32_t widget, int depth,
                       std::uint32_t &textface) {
  if (!widget || depth > 8 || widget + 0x120 > size) return false;
  const std::uint32_t vtable = u32(ram, size, widget + 4);
  const auto &hooks = gt4::target::active().hooks;
  if (vtable == hooks.screen_textface_vtable) {
    const std::uint32_t localized = u32(ram, size, widget + hooks.screen_text_object_offset);
    if (localized) {
      const std::uint32_t key = u32(ram, size, localized + 8);
      if (string_at(ram, size, key) == "TitleMonitor") {
        textface = widget;
        return true;
      }
    }
    return false;
  }
  // Only real containers carry a child list at +0xA4; a leaf's +0xA4 is another field (the old sensor bug).
  const bool container = vtable == hooks.screen_composite_vtable || vtable == hooks.screen_pane_vtable ||
                         vtable == 0x00665418u;  // mRootWindow
  if (!container) return false;
  for (std::uint32_t child = first_child(ram, size, widget); child; child = next_sibling(ram, size, child)) {
    if (has_title_monitor(ram, size, child, depth + 1, textface)) return true;
  }
  return false;
}

std::uint32_t find_context(const std::uint8_t *ram, std::size_t size) {
  const auto &hooks = gt4::target::active().hooks;
  for (std::uint32_t at = 0x100000; at + 4 < size; at += 4)
    if (u32(ram, size, at) == 0x00663BE0u) return at - 4;  // mRenderContextPS2 vtable at +4
  (void)hooks;
  return 0;
}

std::uint32_t find_monitor_root(const std::uint8_t *ram, std::size_t size, std::uint32_t &textface) {
  for (std::uint32_t at = 0x100000; at + 4 < size; at += 4) {
    if (u32(ram, size, at) != 0x00665418u) continue;  // mRootWindow vtable at +4
    const std::uint32_t root = at - 4;
    if (has_title_monitor(ram, size, root, 0, textface)) return root;
  }
  return 0;
}

bool g_launching = false;
std::uint32_t g_game_page = 0;  // the last page root the game itself started (a registered page)

bool launch(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, std::uint32_t page,
            const char *label) {
  const std::uint32_t context = find_context(ram, size);
  std::fprintf(stderr, "[page-launch] frame %llu %s: context 0x%08X, page 0x%08X\n",
               static_cast<unsigned long long>(ps2::frame_count()), label, context, page);
  if (!context || !page) return true;
  const auto &hooks = gt4::target::active().hooks;
  if (!hooks.page_start_callback || !hooks.screen_alloc || !hooks.screen_heap_name || !hooks.screen_free) return true;
  const std::uint32_t info = call_guest(cpu, hooks.screen_heap_name, depth, 0, nullptr);
  const std::uint32_t name = info ? u32(ram, size, info) : 0;
  const std::uint32_t block_bytes = 32;
  const std::uint32_t args0[3] = {block_bytes, 4u, name};
  const std::uint32_t block = call_guest(cpu, hooks.screen_alloc, depth, 3, args0);
  std::fprintf(stderr, "[page-launch] scratch block 0x%08X (name 0x%08X)\n", block, name);
  if (!block) return true;
  // a1 = address of the context slot, a2 = 1, a3 = the argv array of page-root slots, a0 = the return slot.
  w32(ram, size, block + 0, context);
  w32(ram, size, block + 4, page);
  w32(ram, size, block + 8, 0);
  w32(ram, size, block + 12, 0);
  const std::uint32_t args[4] = {block + 8, block, 1u, block + 4};
  g_launching = true;
  call_guest(cpu, hooks.page_start_callback, depth, 4, args);
  g_launching = false;
  std::fprintf(stderr, "[page-launch] startPage(%s context 0x%08X, page 0x%08X) returned\n", label, context, page);
  const std::uint32_t free_args[4] = {block, block_bytes, 4u, name};
  call_guest(cpu, hooks.screen_free, depth, 4, free_args);
  return true;
}

} // namespace

std::uint32_t start_page_entry() { return gt4::target::active().hooks.page_start_callback; }

void on_start_page(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size) {
  const char *enabled = std::getenv("GT4_PAGE_PROBE");
  const std::uint32_t a0 = cpu.r[4].w[0];
  const std::uint32_t a1 = cpu.r[5].w[0];
  const std::uint32_t a2 = cpu.r[6].w[0];
  const std::uint32_t a3 = cpu.r[7].w[0];
  if (!g_launching && a2 > 0) g_game_page = u32(ram, size, a3);
  if (!enabled || *enabled == '\0') return;
  std::fprintf(stderr, "[page-probe] startPage a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n", a0, a1, a2, a3);
  const std::uint32_t context_wrapper = u32(ram, size, a1);
  std::fprintf(stderr, "[page-probe] context slot 0x%08X wrapper 0x%08X\n", a1, context_wrapper);
  dump_object("context wrapper", context_wrapper, ram, size);
  for (std::uint32_t i = 0; i < a2 && i < 8; ++i) {
    const std::uint32_t entry = a3 + i * 4;
    const std::uint32_t wrapper = u32(ram, size, entry);
    std::fprintf(stderr, "[page-probe] page[%u] argv 0x%08X wrapper 0x%08X\n", i, entry, wrapper);
    dump_object("page wrapper", wrapper, ram, size);
  }
}

void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth) {
  static bool hidden_done = false;
  static bool registered_done = false;
  if (!hidden_done) {
    const char *value = std::getenv("GT4_PAGE_LAUNCH_FRAME");
    if (value && *value != '\0' && ps2::frame_count() == std::strtoull(value, nullptr, 10)) {
      std::uint32_t title = 0;
      const std::uint32_t page = find_monitor_root(ram, size, title);
      std::fprintf(stderr, "[page-launch] MonitorRoot 0x%08X (TitleMonitor 0x%08X)\n", page, title);
      hidden_done = launch(cpu, ram, size, depth, page, "MonitorRoot");
    }
  }
  if (!registered_done) {
    const char *value = std::getenv("GT4_PAGE_LAUNCH_FRAME2");
    if (value && *value != '\0' && ps2::frame_count() == std::strtoull(value, nullptr, 10)) {
      std::uint32_t page = g_game_page;
      if (const char *hex = std::getenv("GT4_PAGE_LAUNCH_PAGE")) page = static_cast<std::uint32_t>(std::strtoul(hex, nullptr, 16));
      std::fprintf(stderr, "[page-launch] registered frame %llu: g_game_page 0x%08X, page 0x%08X\n",
                   static_cast<unsigned long long>(ps2::frame_count()), g_game_page, page);
      registered_done = launch(cpu, ram, size, depth, page, "registered page");
    }
  }
}

} // namespace gt4::page_probe
