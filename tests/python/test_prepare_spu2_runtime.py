"""TASK-125: the SPU2 capsule script (scripts/prepare_spu2_runtime.py) on a synthetic donor; the pinned donor when present."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import prepare_spu2_runtime as producer

DONOR_TEXT = (
    '#include "spu2.hpp"\n\n#include "hw.hpp"\n\nstruct Envelope {\n'
    '        int delta = 1;\n'
    '        if (exponential && decrease) delta = delta * level / 0x8000;\n'
    '        level = std::clamp(level + delta, 0, 0x7FFF);\n}\n'
)


class Spu2CapsuleTests(unittest.TestCase):
    def test_overlay_reverses_and_pins_the_derived_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            donor = root / 'private/references/ps2-web-recomp' / producer.DONOR_SPU2
            donor.parent.mkdir(parents=True)
            data = DONOR_TEXT.replace('\n', '\r\n').encode()
            donor.write_bytes(data)
            with patch.object(producer, 'DONOR_SPU2_SHA256', hashlib.sha256(data).hexdigest()):
                output = root / 'private/spu2'
                derived = producer.prepare(output, root)
                manifest = json.loads((output / 'runtime.json').read_text())
                self.assertEqual(manifest['spu2_sha256'], hashlib.sha256((output / 'spu2.cpp').read_bytes()).hexdigest())
                self.assertIn('#include "spu2_envelope.hpp"', derived)
                self.assertIn('gt4::spu2_envelope::exponential_decrease(delta, level)', derived)
                self.assertNotIn('delta * level / 0x8000', derived)
                with self.assertRaisesRegex(ValueError, 'private/'):
                    producer.prepare(root / 'elsewhere', root)

    def test_changed_donor_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            donor = root / 'private/references/ps2-web-recomp' / producer.DONOR_SPU2
            donor.parent.mkdir(parents=True)
            donor.write_bytes(DONOR_TEXT.encode())
            with self.assertRaisesRegex(ValueError, 'pinned version'):
                producer.prepare(root / 'private/spu2', root)

    def test_pinned_donor_when_present(self):
        root = Path(__file__).resolve().parents[2]
        assets = Path('C:/Antigravity/gt4-staticrecomp')
        for candidate in (root, assets):
            if (candidate / 'private/references/ps2-web-recomp' / producer.DONOR_SPU2).exists():
                source = producer.read_donor(candidate)
                derived, removed = producer.apply_overlay(source)
                self.assertEqual(producer.remove_overlay(derived, removed), source)
                self.assertEqual(derived.count('gt4::spu2_envelope::exponential_decrease'), 1)
                return
        self.skipTest('the pinned donor runtime is not in this checkout')


if __name__ == '__main__':
    unittest.main()
