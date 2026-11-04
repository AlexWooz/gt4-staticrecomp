"""TASK-117 (rc6): the flare-limits derived ELF (scripts/flare_limits_elf_patch.py) and its parameter block.

Rules checked:
  * the four redirects and the three helpers are self-consistent, and every helper word is pinned here by value;
  * the helpers read the block of native/flare_limits.hpp (the same address and offsets as the C++ header);
  * the original words match the pinned reconstructed ELF (skipped when the private ELF is absent), the padding is zero and
    nothing jumps into it;
  * a produced derivative is exactly `patch(pinned ELF)` (skipped when no producer directory exists);
  * the aspect rows of TASK-080 stay in the table, so the flare profile keeps the HUD and mirror fixes.
"""
import hashlib
import json
import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import flare_limits_elf_patch as table  # noqa: E402

ASSETS = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))
ELF = ASSETS / 'private/reconstructed/SCUS_973.28.elf'
PRODUCER = ASSETS / f'private/slice299-ee-flare-producer'
MANIFEST = ROOT / 'docs/inputs/slice299-ee-libraries.json'

# The helper words, pinned from the verified assembly (2026-10-25). A change to the assembly must change this table.
PINNED_HELPERS = {
    'halo': (0x00495580, [0x8C087F10, 0x31080001, 0x11000002, 0x00000000, 0x44809800, 0x08126066, 0x00000000]),
    'track': (0x004955A0, [0x8C087F10, 0x31090004, 0x11200008, 0x00000000, 0x8C891848, 0x8C0A7F20, 0x012A482A,
                           0x15200003, 0x00000000, 0x03E00008, 0x00001021, 0x31090002, 0x11200006, 0x00000000,
                           0xC4007F14, 0x460D0034, 0x45000002, 0x00000000, 0x46000346, 0x080EDF26, 0x00000000]),
    'car': (0x00495600, [0x8C087F10, 0x31090004, 0x11200008, 0x00000000, 0x8C890A34, 0x8C0A7F24, 0x012A482A,
                         0x15200003, 0x00000000, 0x03E00008, 0x00001021, 0x31090002, 0x11200006, 0x00000000,
                         0xC4007F18, 0x460D0034, 0x45000002, 0x00000000, 0x46000346, 0x080EDF26, 0x00000000]),
}
PINNED_REDIRECTS = {
    0x003B7BE4: 0x0C125568,
    0x003B85C0: 0x0C125580,
    0x003B8BA0: 0x0C125560,
    0x003B8CE8: 0x0C125560,
}


def source(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def header_constant(name):
    match = re.search(rf'inline constexpr std::uint32_t {name} = (0x[0-9A-Fa-f]+|\d+)u;', source('native/flare_limits.hpp'))
    if not match:
        raise AssertionError(f'{name} not found in native/flare_limits.hpp')
    return int(match.group(1), 0)


class HelperTableTests(unittest.TestCase):
    def test_table_is_self_consistent(self):
        self.assertTrue(table.check_table())

    def test_helpers_are_pinned_word_by_word(self):
        blocks = table.helper_blocks()
        self.assertEqual(set(blocks), set(PINNED_HELPERS))
        for name, (address, words) in blocks.items():
            with self.subTest(helper=name):
                expected_address, expected_words = PINNED_HELPERS[name]
                self.assertEqual(address, expected_address)
                self.assertEqual(words, expected_words)

    def test_redirects_are_pinned(self):
        self.assertEqual({r.address: r.new_word for r in table.REDIRECTS}, PINNED_REDIRECTS)
        for redirect in table.REDIRECTS:
            # The original call: the visibility test for the fills, the flare routine for the drawing sites.
            self.assertEqual(redirect.old_word, table.jal(table.FLARE_ROUTINE if redirect.helper == 'halo'
                                                          else table.VISIBILITY_TEST))

    def test_helpers_read_the_host_block(self):
        block = header_constant('kParameterBlock')
        self.assertEqual(table.PARAMETER_BLOCK, block)
        self.assertEqual(block + 4 * header_constant('kFlagsOffset') // 4, block)
        halo = table.helper_blocks()['halo'][1]
        self.assertEqual(halo[0], 0x8C087F10)  # lw t0, flags(zero)
        track, car = table.helper_blocks()['track'][1], table.helper_blocks()['car'][1]
        self.assertEqual(track[5], 0x8C0A7F20)  # lw t2, track cap(zero)
        self.assertEqual(car[5], 0x8C0A7F24)    # lw t2, car cap(zero)
        self.assertEqual(track[14], 0xC4007F14)  # lwc1 f0, track depth(zero)
        self.assertEqual(car[14], 0xC4007F18)    # lwc1 f0, car depth(zero)
        # The host writes the same offsets: flags 0, track depth 4, car depth 8, caps 16 and 20 bytes into the block.
        self.assertEqual(header_constant('kFlagsOffset'), 0)
        self.assertEqual(header_constant('kTrackDepthOffset'), 4)
        self.assertEqual(header_constant('kCarDepthOffset'), 8)
        self.assertEqual(header_constant('kTrackCapOffset'), 16)
        self.assertEqual(header_constant('kCarCapOffset'), 20)

    def test_flags_match_the_host_bits(self):
        halo, track = table.helper_blocks()['halo'][1], table.helper_blocks()['track'][1]
        self.assertEqual(halo[1], 0x31080001, 'andi t0,t0,1: no halo')
        self.assertEqual(track[1], 0x31090004, 'andi t1,t0,4: light caps')
        self.assertEqual(track[11], 0x31090002, 'andi t1,t0,2: distance cull')
        bits = (header_constant('kNoHalo') | header_constant('kDistanceCull') | header_constant('kLightCaps'))
        self.assertEqual(bits, 7, 'the rc6 flags word')

    def test_helpers_keep_the_original_behaviour_with_a_zero_block(self):
        """With flags 0 each helper reaches its original routine with the argument registers untouched."""
        for name in ('halo', 'track', 'car'):
            words = table.helper_blocks()[name][1]
            self.assertEqual(words[-2] & 0xFC000000, 0x08000000, 'the helper ends in a jump, not a jal')
            target = (words[-2] & 0x03FFFFFF) << 2
            self.assertEqual(target, table.FLARE_ROUTINE if name == 'halo' else table.VISIBILITY_TEST)
        halo = table.helper_blocks()['halo'][1]
        # Only $f19 is written, and only after the flags test.
        self.assertIn(0x44809800, halo)
        self.assertEqual(halo.index(0x44809800), 4)

    def test_aspect_rows_stay_in_the_file(self):
        import aspect_elf_patch
        self.assertEqual(len(aspect_elf_patch.PATCHES), 2)
        pairs = {address: word for address, word in table.patch_pairs()}
        for patch in aspect_elf_patch.PATCHES:
            self.assertEqual(pairs[patch.address], patch.new_word)

    def test_the_padding_is_inside_the_hole(self):
        for name, (address, words) in table.helper_blocks().items():
            self.assertGreaterEqual(address, table.HOLE_START)
            self.assertLessEqual(address + len(words) * 4, table.HOLE_END)
        addresses = [(name, address + index * 4) for name, (address, words) in table.helper_blocks().items()
                     for index in range(len(words))]
        self.assertEqual(len(addresses), len({address for _, address in addresses}), 'no two helpers overlap')


@unittest.skipUnless(ELF.exists(), 'private reconstructed ELF not available')
class PinnedElfTests(unittest.TestCase):
    def test_old_words_match_the_pinned_elf(self):
        elf = ELF.read_bytes()
        self.assertEqual(hashlib.sha256(elf).hexdigest(), table.ELF_SHA256)
        self.assertTrue(table.verify_old_words(elf))

    def test_patch_is_additive_and_reversible(self):
        elf = ELF.read_bytes()
        patched = table.patch_elf(elf)
        covered = set()
        for address, word in table.patch_pairs():
            offset = table.file_offset(elf, address)
            covered.update(range(offset, offset + 4))
            self.assertEqual(table.word_at(patched, address), word, f'0x{address:08X} holds the table word')
        for index in range(len(elf)):
            if index not in covered:
                self.assertEqual(patched[index], elf[index], f'byte 0x{index:X} outside the table changed')
        self.assertLess(len(covered), len(elf))
        self.assertTrue(table.verify_derived(patched, elf))
        for redirect in table.REDIRECTS:
            self.assertEqual(table.word_at(patched, redirect.address), redirect.new_word)
            self.assertEqual(table.word_at(elf, redirect.address), redirect.old_word)


@unittest.skipUnless((PRODUCER / 'flare-generation.json').exists(), 'no slice299 producer in this checkout')
class ProducerTests(unittest.TestCase):
    def test_derived_elf_matches_the_table(self):
        receipt = json.loads((PRODUCER / 'flare-generation.json').read_text(encoding='utf-8'))
        derived = (PRODUCER / receipt['patched_elf']['path']).read_bytes()
        self.assertEqual(hashlib.sha256(derived).hexdigest(), receipt['patched_elf']['sha256'])
        self.assertTrue(table.verify_derived(derived, (PRODUCER / 'SCUS_973.28.elf').read_bytes()))

    def test_generated_calls_reach_the_helpers(self):
        receipt = json.loads((PRODUCER / 'flare-generation.json').read_text(encoding='utf-8'))
        self.assertEqual(len(receipt['generated_calls']), 4)
        for row in receipt['generated_calls']:
            self.assertEqual(int(row['helper'], 16), {'0x004955A0': table.TRACK_HELPER, '0x00495600': table.CAR_HELPER,
                                                      '0x00495580': table.HALO_HELPER}[row['helper']])
        self.assertEqual(receipt['patches'][0]['kind'], 'redirect')

    def test_library_pin_matches_the_receipt(self):
        if not MANIFEST.exists():
            self.skipTest('library not recorded yet')
        manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
        entry = manifest['profiles']['flare']
        receipt = json.loads((PRODUCER / 'flare-generation.json').read_text(encoding='utf-8'))
        self.assertEqual(entry['patched_elf_sha256'], receipt['patched_elf']['sha256'])
        self.assertEqual(entry['patches'], receipt['patches'])
        self.assertEqual(entry['changed_objects_vs_slice208'],
                         sorted(name + '.obj' for name in receipt['changed_units_vs_slice208']))


if __name__ == '__main__':
    unittest.main()
