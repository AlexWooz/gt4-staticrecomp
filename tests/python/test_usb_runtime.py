"""scripts/prepare_usb_runtime.py: the USB capsule's derived iop/hw.cpp (native DFP wheel, TASK-018 and TASK-181).

The tests run on a synthetic donor with every edit point, so they need no private/ checkout; the last test checks the real
pinned donor when it is present.
"""
import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_usb_runtime', ROOT / 'scripts/prepare_usb_runtime.py')
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)
REAL_DONOR = ROOT / usb.DONOR_FILE

# The donor's structure around the edit points (not the donor's text): includes, reset(), the register stub.
SYNTHETIC_DONOR = '''#include "hw.hpp"

#include "ps2/state.hpp"

namespace ps2::iop::hw {

void reset() {
    std::memset(g_ohci, 0, sizeof g_ohci);
}

namespace {

std::uint32_t ohci_read(std::uint32_t a) {
    switch ((a - 0x1F801600u) >> 2) {
    case 0x00: return 0x110u;
    default: return g_ohci[(a - 0x1F801600u) >> 2];
    }
}

void ohci_write(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    if (reg == 0x03) g_ohci[reg] &= ~v;
}

} // namespace
} // namespace ps2::iop::hw
'''


class UsbRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.assets = Path(self.folder.name)
        (self.assets / 'private').mkdir()

    def tearDown(self):
        self.folder.cleanup()

    def test_every_edit_point_is_found_once(self):
        derived = usb.derive(SYNTHETIC_DONOR)
        self.assertIn('#include "usb_dfp.hpp"', derived)
        self.assertEqual(derived.count('bool usb_model_active() {'), 1)
        self.assertEqual(derived.count('usb_controller().read('), 1)
        self.assertEqual(derived.count('usb_controller().write('), 1)
        self.assertEqual(derived.count('if (usb_model_active()) usb_controller().reset();'), 1)

    def test_donor_stub_is_kept_for_the_no_wheel_case(self):
        derived = usb.derive(SYNTHETIC_DONOR)
        self.assertIn('case 0x00: return 0x110u;', derived)
        self.assertIn('if (reg == 0x03) g_ohci[reg] &= ~v;', derived)

    def test_frames_run_on_the_iop_clock(self):
        derived = usb.derive(SYNTHETIC_DONOR)
        self.assertIn('kernel::schedule(kernel::now() + 36864u', derived)  # 1 ms at 36.864 MHz
        self.assertIn('raise_interrupt(22)', derived)  # the IOP's USB interrupt line

    def test_overlay_reverses_to_the_donor(self):
        self.assertEqual(usb.remove_overlay(usb.derive(SYNTHETIC_DONOR)), SYNTHETIC_DONOR)

    def test_unknown_or_repeated_donor_text_is_refused(self):
        with self.assertRaises(ValueError):
            usb.derive('// not the donor iop/hw.cpp\n')
        with self.assertRaises(ValueError):
            usb.derive(SYNTHETIC_DONOR + 'void reset() {\n')

    def test_pinned_donor_hash_is_checked(self):
        donor = self.assets / usb.DONOR_FILE
        donor.parent.mkdir(parents=True)
        donor.write_bytes(SYNTHETIC_DONOR.encode('utf-8'))
        with self.assertRaisesRegex(ValueError, 'differs from the pinned version'):
            usb.prepare(self.assets / 'private' / 'usb', self.assets)
        self.assertEqual(len(usb.DONOR_SHA256), 64)

    def test_prepare_writes_the_capsule_with_its_hash(self):
        out = self.assets / 'private' / 'usb'
        donor = SYNTHETIC_DONOR.replace('\n', '\r\n').encode('utf-8')
        receipt = usb.prepare(out, self.assets, donor=donor)
        written = (out / 'hw.cpp').read_bytes()
        self.assertEqual(receipt['hw_sha256'], hashlib.sha256(written).hexdigest())
        self.assertEqual(receipt['donor_sha256'], hashlib.sha256(donor).hexdigest())
        self.assertIn(b'\r\n', written)  # CRLF donor, CRLF capsule
        on_disk = json.loads((out / 'runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(on_disk, receipt)
        with self.assertRaises(FileExistsError):
            usb.prepare(out, self.assets, donor=donor)

    def test_output_must_stay_in_private(self):
        with self.assertRaisesRegex(ValueError, 'private'):
            usb.prepare(self.assets / 'outside', self.assets, donor=SYNTHETIC_DONOR.encode('utf-8'))

    @unittest.skipUnless(REAL_DONOR.exists(), 'needs the pinned donor checkout under private/references')
    def test_real_donor_matches_the_pin_and_derives(self):
        raw = REAL_DONOR.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), usb.DONOR_SHA256)
        normalized = raw.decode('utf-8').replace('\r\n', '\n')
        self.assertEqual(usb.remove_overlay(usb.derive(normalized)), normalized)


class UsbCapsulePinTests(unittest.TestCase):
    """docs/inputs/slice409-usb-runtime.json: the capsule the rc builds use (scripts/coregs2_bench/game/configure.cmd)."""

    PIN = json.loads((ROOT / 'docs/inputs/slice409-usb-runtime.json').read_text(encoding='utf-8'))
    ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))

    def test_pin_names_the_pinned_donor_and_a_new_slice_folder(self):
        self.assertEqual(self.PIN['donor']['sha256'], usb.DONOR_SHA256)
        self.assertEqual(self.PIN['donor']['path'], usb.DONOR_FILE)
        self.assertEqual(self.PIN['capsule'], 'private/slice409-usb-runtime')
        self.assertEqual({Path(item['path']).name for item in self.PIN['files']}, {'hw.cpp', 'runtime.json'})
        configure = (ROOT / 'scripts/coregs2_bench/game/configure.cmd').read_text(encoding='utf-8')
        self.assertIn('-DGT4_USB_RUNTIME_SOURCE=%USBRT%', configure)
        self.assertIn('private/slice409-usb-runtime', configure)

    def test_capsule_files_match_the_pin(self):
        capsule = self.ASSETS / self.PIN['capsule']
        if not capsule.exists():
            self.skipTest('needs the assets checkout private/ folder')
        for item in self.PIN['files']:
            data = (self.ASSETS / item['path']).read_bytes()
            self.assertEqual((len(data), hashlib.sha256(data).hexdigest()), (item['size_bytes'], item['sha256']),
                             item['path'])
        receipt = json.loads((capsule / 'runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['hw_sha256'], hashlib.sha256((capsule / 'hw.cpp').read_bytes()).hexdigest())
        self.assertEqual(receipt['donor_sha256'], usb.DONOR_SHA256)

    def test_capsule_is_reproducible_from_the_pinned_donor(self):
        if not (self.ASSETS / usb.DONOR_FILE).exists():
            self.skipTest('needs the pinned donor checkout under private/references')
        raw = usb.read_donor(self.ASSETS)
        derived = usb.derive(raw.decode('utf-8').replace('\r\n', '\n'))
        if b'\r\n' in raw:
            derived = derived.replace('\n', '\r\n')
        expected = next(item['sha256'] for item in self.PIN['files'] if item['path'].endswith('hw.cpp'))
        self.assertEqual(hashlib.sha256(derived.encode('utf-8')).hexdigest(), expected)


if __name__ == '__main__':
    unittest.main()
