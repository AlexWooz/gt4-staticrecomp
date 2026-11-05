"""scripts/prepare_usb_runtime.py: the USB capsule's derived iop/hw.cpp (native DFP wheel work)."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_usb_runtime', ROOT / 'scripts/prepare_usb_runtime.py')
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)
DONOR = ROOT / usb.DONOR_FILE


@unittest.skipUnless(DONOR.exists(), 'needs the pinned donor checkout under private/references')
class UsbRuntimeTests(unittest.TestCase):
    def donor_text(self):
        return DONOR.read_bytes().decode('utf-8').replace('\r\n', '\n')

    def test_every_edit_point_is_found_once(self):
        derived = usb.derive(self.donor_text())
        self.assertIn('#include "usb_dfp.hpp"', derived)
        self.assertEqual(derived.count('bool usb_model_active() {'), 1)
        self.assertEqual(derived.count('usb_controller().read('), 1)
        self.assertEqual(derived.count('usb_controller().write('), 1)

    def test_donor_stub_is_kept_for_the_no_wheel_case(self):
        derived = usb.derive(self.donor_text())
        # The donor's register stub still answers when no virtual wheel is plugged in.
        self.assertIn('case 0x00: return 0x110u;', derived)
        self.assertIn('if (reg == 0x03) g_ohci[reg] &= ~v;', derived)

    def test_frames_run_on_the_iop_clock(self):
        derived = usb.derive(self.donor_text())
        self.assertIn('kernel::schedule(kernel::now() + 36864u', derived)  # 1 ms at 36.864 MHz
        self.assertIn('raise_interrupt(22)', derived)  # the IOP's USB interrupt line

    def test_unknown_donor_text_is_refused(self):
        with self.assertRaises(SystemExit):
            usb.derive('// not the donor iop/hw.cpp\n')

    def test_prepare_writes_the_capsule_with_its_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'usb'
            usb.main([str(out)])
            receipt = json.loads((out / 'runtime.json').read_text(encoding='utf-8'))
            import hashlib
            self.assertEqual(receipt['hw_sha256'], hashlib.sha256((out / 'hw.cpp').read_bytes()).hexdigest())
            with self.assertRaises(FileExistsError):
                usb.main([str(out)])


if __name__ == '__main__':
    unittest.main()
