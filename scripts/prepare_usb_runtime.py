"""USB capsule (TASK-018, native DFP wheel; TASK-181): derives the donor's iop/hw.cpp so the OHCI registers at
0x1F801600 reach the controller model in native/usb_ohci.cpp, which carries the virtual Driving Force Pro of
native/usb_dfp.cpp. Without a wheel (wheel.toml enabled = true, or GT4_DFP=1) the donor's register stub answers exactly as
before.

The donor file is pinned by its SHA-256 (like scripts/prepare_spu2_runtime.py); edit points are anchored on unique donor
text, and removing the edits must restore the normalized donor input. The output folder must be new and inside the assets
checkout's private/ (derived donor code is never committed).

usage: prepare_usb_runtime.py <fresh output folder under private/> [--assets-root <checkout>]
Writes hw.cpp and runtime.json (with the derived file's SHA-256, which CMake checks: GT4_USB_RUNTIME_SOURCE).
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FILE = 'private/references/ps2-web-recomp/runtime/src/iop/hw.cpp'
# ps2-web-recomp 1b4808793a12c45125262fd2d5f50a9ec9f7c020, runtime/src/iop/hw.cpp as checked out on Windows (CRLF).
DONOR_SHA256 = 'b8b56b21a8bf00652004847b55baaca9caa74d6757139ef8c6f27597de38c836'

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

# Applied in this order; removed in the reverse order.
EDITS = [
    ('includes', INCLUDES_OLD, INCLUDES_NEW),
    ('forward declarations', FORWARD_OLD, FORWARD_NEW),
    ('reset', RESET_OLD, RESET_NEW),
    ('register read', READ_OLD, READ_NEW),
    ('register write', WRITE_OLD, WRITE_NEW),
]


def derive(source: str) -> str:
    """The derived text of a normalized (LF) donor text. Refuses a text where an edit point is missing or repeated."""
    for name, old, _ in EDITS:
        if source.count(old) != 1:
            raise ValueError(f'{name}: edit point not found exactly once in the donor iop/hw.cpp: {old[:60]!r}')
    for _, old, new in EDITS:
        source = source.replace(old, new)
    return source


def remove_overlay(derived: str) -> str:
    for name, old, new in reversed(EDITS):
        if derived.count(new) != 1:
            raise ValueError(f'{name}: the derived source lacks this edit')
        derived = derived.replace(new, old)
    return derived


def read_donor(assets_root: Path) -> bytes:
    raw = (assets_root / DONOR_FILE).read_bytes()
    actual = hashlib.sha256(raw).hexdigest()
    if actual != DONOR_SHA256:
        raise ValueError(f'Donor source differs from the pinned version: {DONOR_FILE} ({actual})')
    return raw


def prepare(output: Path, assets_root: Path = None, donor: bytes = None) -> dict:
    """Writes hw.cpp and runtime.json into `output` (new, under <assets>/private). `donor` replaces the pinned donor
    read (the fixtures' synthetic donor); it is still derived and reversed the same way."""
    assets_root = Path(assets_root or ROOT).resolve()
    output = Path(output).resolve()
    private = assets_root / 'private'
    if not output.is_relative_to(private) or output == private:
        raise ValueError('Derived donor code must stay inside the assets checkout private/')
    raw = read_donor(assets_root) if donor is None else donor
    text = raw.decode('utf-8')
    crlf = '\r\n' in text  # a Windows checkout of the donor has CRLF line endings; edits are made on LF
    normalized = text.replace('\r\n', '\n')
    derived_text = derive(normalized)
    if remove_overlay(derived_text) != normalized:
        raise ValueError('USB overlay does not reverse to the donor input')
    derived = (derived_text.replace('\n', '\r\n') if crlf else derived_text).encode('utf-8')
    output.mkdir(parents=True, exist_ok=False)
    (output / 'hw.cpp').write_bytes(derived)
    receipt = {
        'schema_version': 1,
        'donor_file': DONOR_FILE,
        'donor_sha256': hashlib.sha256(raw).hexdigest(),
        'hw_sha256': hashlib.sha256(derived).hexdigest(),
        'overlay': 'TASK-018/TASK-181: OHCI registers reach native/usb_ohci.cpp while the virtual DFP is plugged in',
        'tool': 'scripts/prepare_usb_runtime.py',
    }
    (output / 'runtime.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8', newline='\n')
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    print(json.dumps(prepare(args.output, args.assets_root), indent=2))


if __name__ == '__main__':
    main()
