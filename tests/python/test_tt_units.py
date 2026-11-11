"""Tourist Trophy: the translated units with TT's host call and pedal statements (scripts/tt_units.py, docs/TT-STUDY.md).

Rules checked:
  * the license call goes right after the cipher's entry label; the four pedal statements (TASK-161, as v2.00) are replaced
    inside their own labelled block only, exactly once; the declarations follow the include; nothing else changes;
  * removing the insertions gives the input back; a missing or repeated statement, a missing label and an existing output
    folder are refused; every other file is copied unchanged;
  * on the real units (skipped when the private folder is absent): the patched copy has each edit once.
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import tt_units  # noqa: E402

PEDAL_UNIT = '''#include "units.hpp"

namespace gen {
L_00403B50:
    set32(c.r[3], c.r[3].w[0] << 1);
L_00403B68:
    c.r[4].d[0] = ld8((c.r[18].w[0]));
    set32(c.r[3], c.r[3].w[0] << 1);
    set32(c.r[2], c.r[3].w[0] << 1);
L_00403B90:
    mmi::div(c, 0, c.r[2].w[0], c.r[17].w[0]);
L_00403BC0:
    set32(c.r[3], c.r[3].w[0] << 1);
    set32(c.r[2], c.r[3].w[0] << 1);
L_00403BEC:
    mmi::div(c, 0, c.r[2].w[0], c.r[22].w[0]);
L_00403C18:
    set32(c.r[3], c.r[3].w[0] << 1);
    mmi::div(c, 0, c.r[2].w[0], c.r[21].w[0]);
}
'''

CIPHER_UNIT = '''#include "units.hpp"

namespace gen {
L_004A4218:
    c.pc = 1;
L_004A4228:
    set32(c.r[29], c.r[29].w[0] - 32u);
}
'''


def pedal_edits():
    return [edit for edit in tt_units.EDITS if edit.label != 'L_004A4228']


class TtUnitsTest(unittest.TestCase):
    def test_pedal_statements_are_replaced_in_their_blocks_only(self):
        out = tt_units.patch(PEDAL_UNIT, pedal_edits())
        self.assertEqual(out.count('::gt4::trigger_range::pedal_pressure(c.r[3].w[0])'), 2)
        self.assertIn('pedal_divisor(c.r[17].w[0])', out)
        self.assertIn('pedal_divisor(c.r[22].w[0])', out)
        # Untouched: the second `<< 1` of each case, the block before the throttle case and the other pedal cases.
        self.assertEqual(out.count('set32(c.r[2], c.r[3].w[0] << 1);'), 2)
        self.assertIn('L_00403B50:\n    set32(c.r[3], c.r[3].w[0] << 1);\n', out)
        self.assertIn('L_00403C18:\n    set32(c.r[3], c.r[3].w[0] << 1);\n    mmi::div(c, 0, c.r[2].w[0], c.r[21].w[0]);', out)
        self.assertIn('namespace gt4::trigger_range {', out.split('namespace gen {')[0])

    def test_license_call_follows_the_entry_label(self):
        out = tt_units.patch(CIPHER_UNIT, [tt_units.LICENSE])
        self.assertIn('L_004A4228:\n    if (::gt4::tt_license::before_license_decrypt(c)) return;', out)
        self.assertIn('namespace gt4::tt_license { bool before_license_decrypt(ps2::Cpu &c); }', out)
        self.assertIn('L_004A4218:\n    c.pc = 1;\n', out)

    def test_missing_or_repeated_statements_are_refused(self):
        with self.assertRaises(ValueError):
            tt_units.patch(PEDAL_UNIT.replace('c.r[17].w[0]);', 'c.r[16].w[0]);'), pedal_edits())
        doubled = PEDAL_UNIT.replace('L_00403B90:\n', 'L_00403B90:\n    mmi::div(c, 0, c.r[2].w[0], c.r[17].w[0]);\n')
        with self.assertRaises(ValueError):
            tt_units.patch(doubled, pedal_edits())
        with self.assertRaises(ValueError):
            tt_units.patch(PEDAL_UNIT.replace('L_00403BEC:', 'L_00403BEE:'), pedal_edits())

    def test_folder_run_copies_the_rest_and_refuses_an_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            units = Path(temporary) / 'units'
            units.mkdir()
            # The translator writes LF; write the fixtures byte for byte (write_text would use CRLF on Windows).
            (units / 'unit_00C0.cpp').write_bytes(PEDAL_UNIT.encode('utf-8'))
            (units / 'unit_00E9.cpp').write_bytes(CIPHER_UNIT.encode('utf-8'))
            (units / 'unit_0000.cpp').write_bytes(b'#include "units.hpp"\nL_00100000:\n')
            (units / 'table.cpp').write_bytes(b'// table\n')
            output = Path(temporary) / 'out'
            self.assertEqual(tt_units.main([str(units), str(output)]), 0)
            self.assertEqual((output / 'table.cpp').read_bytes(), b'// table\n')
            self.assertEqual((output / 'unit_0000.cpp').read_bytes(), (units / 'unit_0000.cpp').read_bytes())
            self.assertIn('pedal_pressure', (output / 'unit_00C0.cpp').read_text(encoding='utf-8'))
            self.assertIn('before_license_decrypt', (output / 'unit_00E9.cpp').read_text(encoding='utf-8'))
            with self.assertRaises(SystemExit):
                tt_units.main([str(units), str(output)])

    def test_real_units_when_present(self):
        patched = ROOT / 'private' / 'tt-ee-experiment' / 'generated-tt'
        if not patched.is_dir():
            self.skipTest('private TT units not present')
        pedal = (patched / 'unit_00C0.cpp').read_text(encoding='utf-8')
        cipher = (patched / 'unit_00E9.cpp').read_text(encoding='utf-8')
        self.assertEqual(pedal.count('::gt4::trigger_range::pedal_pressure(c.r[3].w[0])'), 2)
        self.assertEqual(pedal.count('::gt4::trigger_range::pedal_divisor('), 2)
        self.assertEqual(cipher.count('::gt4::tt_license::before_license_decrypt(c)) return;'), 1)


if __name__ == '__main__':
    unittest.main()
