//@section includes
#include "license_guard.hpp"
//@section helpers
// TASK-017 license demo guard (slice206, scripts/prepare_license_guard.py). Runs only for the indirect call from a
// license demo loader into its demo-input virtual: v2.00 0x001100D0 (return label 0x001101E4) or 0x00110230 (return
// label 0x00110344); Spec II 0x0010CA18 (return label 0x0010CB54) or 0x0010CBA8 (return label 0x0010CCE4), whatever the
// target. Rule and rationale: native/license_guard.hpp. One stderr line per checked demo load.
namespace {

bool license_guard_guest_span(std::uint32_t address, std::uint32_t size) {
    return address < kRamSize && size <= kRamSize - address;
}

void license_demo_guard(Cpu &c) {
    namespace guard = gt4::license_guard;
    const std::uint32_t loader = guard::loader_for_return(c.r[31].w[0]);
    // A dispatch of the return label itself (unwinding at kMaxDepth) is not the demo-input call.
    if (!loader || c.pc == c.r[31].w[0]) return;
    const std::uint32_t target = c.pc;
    const std::uint32_t buffer = c.r[17].w[0];        // s1: the demo file buffer the loader decrypted in place
    const std::uint32_t body_pointer = c.r[5].w[0];   // a1: the pointer the loader hands the demo-input virtual
    if (!license_guard_guest_span(buffer, guard::kHeaderBytes)) {
        std::fprintf(stderr, "[license-guard] loader 0x%08X target 0x%08X demo buffer 0x%08X outside RAM -> unchanged\n",
                     loader, target, buffer);
        return;
    }
    const std::uint32_t offset = ld32(buffer), size = ld32(buffer + 4u), key = ld32(buffer + 12u);
    // The header's own offset is the start of the decrypted body, the region the game CRC covers. The virtual may
    // receive that pointer shifted (Spec II: 0x80 into the body), so the pointer check allows the target's shift and
    // the rule below runs on the header's body start.
    const std::uint32_t body = buffer + offset;
    if (offset != guard::kExpectedBodyOffset || !guard::body_pointer_matches(body, body_pointer) ||
        size > guard::kMaxBodySize || !license_guard_guest_span(body, size)) {
        std::fprintf(stderr, "[license-guard] loader 0x%08X target 0x%08X demo 0x%08X body 0x%08X header offset 0x%X "
                     "size 0x%X arg1 0x%08X unexpected -> unchanged\n", loader, target, body_pointer, body, offset, size,
                     body_pointer);
        return;
    }
    const guard::Verdict verdict = guard::evaluate(g_ram + body, size, key);
    if (!verdict.guard) {
        std::fprintf(stderr, "[license-guard] loader 0x%08X target 0x%08X demo 0x%08X size 0x%X key 0x%08X crc=%s "
                     "layout=%s count=0x%08X -> unchanged\n", loader, target, body_pointer, size, key,
                     verdict.crc_ok ? "ok" : "fail", verdict.layout_ok ? "ok" : "bad", verdict.count);
        return;
    }
    // Manager::load reads the count at the virtual's own address (pointer + the target's add); write exactly there.
    const std::uint32_t manager = guard::manager_address(body_pointer);
    if (!license_guard_guest_span(manager, 4u)) {
        std::fprintf(stderr, "[license-guard] loader 0x%08X target 0x%08X demo 0x%08X manager 0x%08X outside RAM "
                     "-> unchanged\n", loader, target, body_pointer, manager);
        return;
    }
    st32(manager, guard::kSkipCount);
    std::fprintf(stderr, "[license-guard] loader 0x%08X target 0x%08X demo 0x%08X size 0x%X key 0x%08X crc=fail "
                 "layout=bad count=0x%08X -> skipped (count set to 0x%08X, guarded load %u) manager 0x%08X\n",
                 loader, target, body_pointer, size, key, verdict.count, guard::kSkipCount, guard::note_guarded(),
                 manager);
}

}  // namespace

//@section hook
    if (gt4::license_guard::loader_for_return(c.r[31].w[0])) [[unlikely]] license_demo_guard(c);
