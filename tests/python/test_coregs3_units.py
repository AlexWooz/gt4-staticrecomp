"""TASK-090: the CoreGS 3.0 unit insertion script (scripts/coregs3_units.py), on hand-authored unit text.

The real units are private translated code; these controls use a synthetic unit that only contains the anchors the script
looks for, so the insertion rules, the refusals and the byte-exact removal proof are testable without any game data.
"""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('coregs3_units', ROOT / 'scripts' / 'coregs3_units.py')
coregs3_units = importlib.util.module_from_spec(SPEC)
sys.modules['coregs3_units'] = coregs3_units
SPEC.loader.exec_module(coregs3_units)

CALL_0055A64 = ('    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00455A64u);  if (depth < kMaxDepth) unit_00E7(c, 53u, '
                'depth + 1u); else { c.pc = 0x0049D308u; return; } if (c.pc != 0x00455A64u) [[unlikely]] return; }\n')

SYNTHETIC_00D5 = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    '\n'
    'L_00455A5C:\n'
    + CALL_0055A64 +
    'L_00455A64:\n'
    '    return;\n'
    '}\n'
)

SYNTHETIC_008F = (
    '#include "units.hpp"\n'
    '\n'
    'namespace gen {\n'
    'L_0033EBB4:\n'
    '    c.budget -= 2;\n'
    'L_0033EBF4:\n'
    '    c.budget -= 2;\n'
    'L_0033EE28:\n'
    '    call_next();\n'
    'L_0033EE54:\n'
    '    return;\n'
    '}\n'
)


class InsertionTest(unittest.TestCase):
    def test_apply_and_remove_are_exact(self):
        for name, text in (('unit_00D5.cpp', SYNTHETIC_00D5), ('unit_008F.cpp', SYNTHETIC_008F)):
            instrumented = coregs3_units.apply_insertions(name, text)
            self.assertNotEqual(instrumented, text)
            self.assertIn('namespace gt4::coregs3 {', instrumented)
            self.assertEqual(coregs3_units.remove_insertions(name, instrumented), text,
                             f'{name}: removal must restore the input byte for byte')

    def test_k1_call_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_00D5.cpp', SYNTHETIC_00D5)
        self.assertIn('::gt4::coregs3::before_shape_call(c);\n' + CALL_0055A64, instrumented)
        self.assertIn('L_00455A64:\n    ::gt4::coregs3::after_shape_call(c);\n', instrumented)

    def test_pass_and_camera_sites(self):
        instrumented = coregs3_units.apply_insertions('unit_008F.cpp', SYNTHETIC_008F)
        for label, call in (('L_0033EBB4', 'projection_set'), ('L_0033EBF4', 'modelview_set'),
                            ('L_0033EE28', 'car_pass_begin'), ('L_0033EE54', 'car_pass_end')):
            self.assertIn(f'{label}:\n    ::gt4::coregs3::{call}(c);\n', instrumented)

    def test_a_missing_anchor_is_refused(self):
        broken = SYNTHETIC_00D5.replace('0x00455A64u);  if', '0x00455A64u); if')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', broken)

    def test_an_ambiguous_anchor_is_refused(self):
        twice = SYNTHETIC_008F.replace('L_0033EE54:\n', 'L_0033EE54:\nL_0033EE54:\n')
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_008F.cpp', twice)

    def test_a_file_without_the_include_is_refused(self):
        with self.assertRaises(coregs3_units.UnitError):
            coregs3_units.apply_insertions('unit_00D5.cpp', 'int main() {}\n')

    def test_instrument_writes_a_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'units'
            source.mkdir()
            (source / 'unit_00D5.cpp').write_text(SYNTHETIC_00D5, encoding='utf-8', newline='\n')
            (source / 'unit_008F.cpp').write_text(SYNTHETIC_008F, encoding='utf-8', newline='\n')
            (source / 'unit_0000.cpp').write_text('unchanged\n', encoding='utf-8', newline='\n')
            output = Path(temporary) / 'instrumented'
            receipt = coregs3_units.instrument(source, output)
            self.assertEqual(receipt['files']['instrumented'], 2)
            self.assertEqual(receipt['files']['copied'], 1)
            self.assertEqual((output / 'unit_0000.cpp').read_text(encoding='utf-8'), 'unchanged\n')
            self.assertTrue((Path(temporary) / 'instrumented.coregs3-units.json').is_file())
            with self.assertRaises(coregs3_units.UnitError):
                coregs3_units.instrument(source, output)  # a fresh folder only


if __name__ == '__main__':
    unittest.main()
