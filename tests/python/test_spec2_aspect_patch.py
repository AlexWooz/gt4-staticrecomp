"""TASK-166: the Spec II rows of the TASK-080 P2 ELF patch table (scripts/spec2_aspect_patch.py).

Rules checked: each Spec II row is the same instruction, the same new word and the same block word as its v2.00 row, so one
host-filled HUD parameter block serves both targets; the 16:9 block values are the original constants; the old words match
the pinned derived Spec II ELF when it is present; decision 0071 lists every Spec II word.
"""
import hashlib
import json
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import aspect_elf_patch as v200  # noqa: E402
import spec2_aspect_patch as table  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))
ELF = ASSETS / table.DERIVED_ELF


class Spec2PatchTableTests(unittest.TestCase):
    def test_table_is_self_consistent(self):
        self.assertTrue(table.check_table())
        self.assertEqual([hex(p.address) for p in table.PATCHES], ['0x1c519c', '0x1eb65c'])

    def test_rows_match_the_v200_rows(self):
        by_offset = {patch.block_offset: patch for patch in v200.PATCHES}
        self.assertEqual(sorted(by_offset), sorted(patch.block_offset for patch in table.PATCHES))
        for patch in table.PATCHES:
            with self.subTest(address=hex(patch.address)):
                twin = by_offset[patch.block_offset]
                self.assertEqual((patch.old_word, patch.new_word, patch.value_16_9),
                                 (twin.old_word, twin.new_word, twin.value_16_9))

    def test_a_row_with_another_block_word_is_refused(self):
        saved = table.PATCHES
        try:
            font, mirror = saved
            table.PATCHES = (font, v200.Patch(mirror.address, mirror.old_word, v200.encode_lw(8, 0x7F08), 0x08, 640,
                                              mirror.function, mirror.purpose))
            with self.assertRaises(ValueError):
                table.check_table()
        finally:
            table.PATCHES = saved

    def test_decision_0071_lists_every_spec2_word(self):
        decision = (ROOT / 'docs/decisions/0071-native-aspect-ratios-runtime-hooks.md').read_text(encoding='utf-8')
        for patch in table.PATCHES:
            with self.subTest(address=hex(patch.address)):
                self.assertIn(f'0x{patch.address:08X}', decision)
        self.assertIn('TASK-166', decision)

    def test_old_words_match_the_pinned_derived_elf(self):
        if not ELF.exists():
            self.skipTest('derived Spec II ELF not present')
        elf = ELF.read_bytes()
        pin = json.loads((ROOT / 'docs/inputs/spec2-elf.json').read_text(encoding='utf-8'))['derived']
        self.assertEqual(hashlib.sha256(elf).hexdigest(), pin['sha256'])
        self.assertTrue(table.verify_old_words(elf))
        patched = table.patch_elf(elf)
        self.assertEqual(len(patched), len(elf))
        differing = [offset for offset in range(0, len(elf), 4) if elf[offset:offset + 4] != patched[offset:offset + 4]]
        self.assertEqual(differing, sorted(v200.file_offset(elf, patch.address) for patch in table.PATCHES))
        with self.assertRaises(ValueError):
            table.patch_elf(patched)


if __name__ == '__main__':
    unittest.main()
