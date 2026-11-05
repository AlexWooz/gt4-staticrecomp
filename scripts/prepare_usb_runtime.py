"""USB capsule (TASK-018, native DFP wheel): derives the donor's iop/hw.cpp so the OHCI registers at
0x1F801600 reach the controller model in native/usb_ohci.cpp, which carries the virtual Driving Force Pro of
native/usb_dfp.cpp. Without a wheel in DFP mode (wheel.toml mode = "dfp" or GT4_DFP=1) the donor's register stub
answers exactly as before.

usage: prepare_usb_runtime.py <fresh output folder> [--assets-root <checkout>]
Writes hw.cpp and runtime.json (with the derived file's SHA-256, which CMake checks).
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FILE = 'private/references/ps2-web-recomp/runtime/src/iop/hw.cpp'
DONOR_SHA256 = None  # recorded in runtime.json; the donor checkout itself is pinned by external-recomp-references.json

INCLUDES_OLD = '#include "ps2/state.hpp"\n'
INCLUDES_NEW = '#include "ps2/state.hpp"\n\n#include "usb_dfp.hpp"\n'

READ_OLD = 'std::uint32_t ohci_read(std::uint32_t a) {\n'
READ_NEW = '''// USB capsule: the controller model when a virtual wheel is in use, else the donor stub below.
bool usb_model_active() { return gt4::usb::DfpBridge::shared().enabled(); }
std::uint64_t g_usb_generation = 0;
bool g_usb_timer = false;

gt4::usb::Bus usb_bus() {
    gt4::usb::Bus bus;
    bus.read32 = [](std::uint32_t a) { return ps2::iop::read32(a); };
    bus.write32 = [](std::uint32_t a, std::uint32_t v) { ps2::iop::write32(a, v); };
    bus.read8 = [](std::uint32_t a) { return ps2::iop::read8(a); };
    bus.write8 = [](std::uint32_t a, std::uint8_t v) { ps2::iop::write8(a, v); };
    bus.raise_interrupt = [] { raise_interrupt(22); };
    return bus;
}

gt4::usb::Ohci &usb_controller() { return gt4::usb::machine_controller(usb_bus()); }

// One USB frame per millisecond of IOP time (36,864 cycles at 36.864 MHz), from the first register access on.
void usb_frame_tick(std::uint64_t generation) {
    if (generation != g_usb_generation) return;
    usb_controller().frame();
    kernel::schedule(kernel::now() + 36864u, [generation] { usb_frame_tick(generation); });
}

void usb_start_timer() {
    if (g_usb_timer) return;
    g_usb_timer = true;
    const std::uint64_t generation = ++g_usb_generation;
    kernel::schedule(kernel::now() + 36864u, [generation] { usb_frame_tick(generation); });
}

std::uint32_t ohci_read(std::uint32_t a) {
    if (usb_model_active()) {
        usb_start_timer();
        return usb_controller().read((a - 0x1F801600u) >> 2);
    }
'''

WRITE_OLD = 'void ohci_write(std::uint32_t a, std::uint32_t v) {\n    const std::uint32_t reg = (a - 0x1F801600u) >> 2;\n'
WRITE_NEW = '''void ohci_write(std::uint32_t a, std::uint32_t v) {
    if (usb_model_active()) {
        usb_start_timer();
        usb_controller().write((a - 0x1F801600u) >> 2, v);
        return;
    }
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
'''

RESET_OLD = '    std::memset(g_ohci, 0, sizeof g_ohci);\n}\n'
RESET_NEW = '''    std::memset(g_ohci, 0, sizeof g_ohci);
    g_usb_timer = false;
    ++g_usb_generation;
    if (usb_model_active()) usb_controller().reset();
}
'''

# reset() is defined before ohci_read in the donor file (outside the anonymous namespace), so the capsule's state is
# declared just before it.
FORWARD_OLD = 'void reset() {\n'
FORWARD_NEW = '''bool usb_model_active();
gt4::usb::Ohci &usb_controller();
extern std::uint64_t g_usb_generation;
extern bool g_usb_timer;

void reset() {
'''


def derive(source: str) -> str:
    for old in (INCLUDES_OLD, READ_OLD, WRITE_OLD, RESET_OLD, FORWARD_OLD):
        if source.count(old) != 1:
            raise SystemExit(f'edit point not found exactly once in the donor iop/hw.cpp: {old[:60]!r}')
    source = source.replace(INCLUDES_OLD, INCLUDES_NEW)
    source = source.replace(FORWARD_OLD, FORWARD_NEW)
    source = source.replace(RESET_OLD, RESET_NEW)
    source = source.replace(READ_OLD, READ_NEW)
    source = source.replace(WRITE_OLD, WRITE_NEW)
    return source


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    donor = (args.assets_root / DONOR_FILE).read_bytes()
    text = donor.decode('utf-8')
    crlf = '\r\n' in text  # a Windows checkout of the donor has CRLF line endings; edits are made on LF
    derived_text = derive(text.replace('\r\n', '\n'))
    derived = (derived_text.replace('\n', '\r\n') if crlf else derived_text).encode('utf-8')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'hw.cpp').write_bytes(derived)
    receipt = {'schema_version': 1, 'donor_file': DONOR_FILE, 'donor_sha256': hashlib.sha256(donor).hexdigest(),
               'hw_sha256': hashlib.sha256(derived).hexdigest(), 'tool': 'scripts/prepare_usb_runtime.py'}
    (args.output / 'runtime.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
