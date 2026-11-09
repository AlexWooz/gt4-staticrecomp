"""TASK-212: the GT4.VOL data patch builder (scripts/gt4_data_patch.py).

These tests use synthetic text and payloads only; the real patch is derived from the user's own disc by the CLI and is
never committed. The runtime side is exercised by native/data_patch_fixture and by the slice421 headless run.
"""
import pathlib
import sys
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))

import gt4_data_patch as patch  # noqa: E402
import gt4_volume  # noqa: E402


class MinifyTextTest(unittest.TestCase):
    def test_collapses_whitespace_outside_strings(self):
        text = 'Project {\n\tname string {"two  spaces"}\n\tchildren [1] {\n\t}\n}\n'
        self.assertEqual(patch.minify_text(text), 'Project { name string {"two  spaces"} children [1] { } }')

    def test_keeps_escaped_quotes_and_backslashes(self):
        text = 'a string {"a \\"quoted\\" \\\\ path"}\n'
        self.assertEqual(patch.minify_text(text), 'a string {"a \\"quoted\\" \\\\ path"}')


class TextReplacementTest(unittest.TestCase):
    def test_requires_exactly_one_anchor(self):
        with self.assertRaises(ValueError):
            patch.apply_text_replacements('a b', [{'old': 'c', 'new': 'x'}])
        with self.assertRaises(ValueError):
            patch.apply_text_replacements('a a', [{'old': 'a', 'new': 'x'}])
        self.assertEqual(patch.apply_text_replacements('a b', [{'old': 'b', 'new': 'c'}]), 'a c')


class RawFileTest(unittest.TestCase):
    """Spec II's arcade.mproject is a raw (kind 1) record; the text edit pads to the exact stored extent."""

    def test_raw_text_edit_pads_to_the_original_extent(self):
        payload = b'Project { name string {"test"} }\n'
        record = type('Record', (), {'kind': 1, 'stored_size': len(payload) + 16, 'size': len(payload) + 16})
        replacement = patch.build_replacement(record, payload, {'text_replacements': [], 'minify': False})
        self.assertEqual(len(replacement), record.size)
        self.assertTrue(replacement.startswith(payload))
        self.assertEqual(replacement[len(payload):], b'\n' * 16)
        container = patch.encode_same_extent(record, replacement)
        self.assertEqual(container, replacement)
        patch.verify_round_trip(record, container, replacement)

    def test_raw_text_edit_growth_is_refused(self):
        payload = b'a' * 32
        record = type('Record', (), {'kind': 1, 'stored_size': 32, 'size': 32})
        with self.assertRaises(ValueError):
            patch.build_replacement(record, payload,
                                    {'text_replacements': [{'old': 'a' * 32, 'new': 'b' * 33}], 'minify': False})


class ByteReplacementTest(unittest.TestCase):
    """TASK-213: the same-length byte edit of the `"NTSC"` literal in option.adc."""

    @staticmethod
    def record(size):
        return type('Record', (), {'kind': 2, 'stored_size': size + 64, 'size': size})

    def test_replaces_exactly_the_named_range(self):
        payload = b'....NTSC....'
        edit = {'byte_replacements': [{'offset': 4, 'length': 4, 'expect_hex': '4e545343', 'hex': '50414c20'}]}
        replacement = patch.build_replacement(self.record(len(payload)), payload, edit)
        self.assertEqual(replacement, b'....PAL ....')
        self.assertEqual(len(replacement), len(payload))

    def test_a_mismatched_anchor_is_refused(self):
        payload = b'....NTSC....'
        edit = {'byte_replacements': [{'offset': 4, 'length': 4, 'expect_hex': '4e54534d', 'hex': '50414c20'}]}
        with self.assertRaises(ValueError):
            patch.build_replacement(self.record(len(payload)), payload, edit)

    def test_a_wrong_length_is_refused(self):
        payload = b'....NTSC....'
        edit = {'byte_replacements': [{'offset': 4, 'length': 4, 'hex': '50414c'}]}
        with self.assertRaises(ValueError):
            patch.build_replacement(self.record(len(payload)), payload, edit)

    def test_a_range_outside_the_file_is_refused(self):
        payload = b'....NTSC....'
        edit = {'byte_replacements': [{'offset': 10, 'length': 4, 'hex': '50414c20'}]}
        with self.assertRaises(ValueError):
            patch.build_replacement(self.record(len(payload)), payload, edit)

    def test_byte_and_text_edits_do_not_mix(self):
        payload = b'....NTSC....'
        edit = {'byte_replacements': [{'offset': 4, 'length': 4, 'hex': '50414c20'}],
                'text_replacements': [{'old': 'x', 'new': 'y'}]}
        with self.assertRaises(ValueError):
            patch.build_replacement(self.record(len(payload)), payload, edit)

    def test_a_grown_replacement_is_refused_at_the_stored_extent(self):
        payload = b'....NTSC....'
        replacement = patch.build_replacement(
            self.record(len(payload)), payload,
            {'byte_replacements': [{'offset': 4, 'length': 4, 'hex': '50414c20'}]})
        encoded = patch.ps2zip(replacement)
        record = type('Record', (), {'kind': 2, 'stored_size': len(encoded) - 1, 'size': len(replacement)})
        with self.assertRaises(ValueError):
            patch.encode_same_extent(record, replacement)


class V200AnchorTest(unittest.TestCase):
    """The real v2.00 decoded files, read-only, when the private study folder is present.

    The private folder is not part of the repository; the test skips without it. It pins the anchor the private recipe
    uses: the one `"NTSC"` literal at 0x247FB in option.adc, and the one label/choice key of each relabelled row in
    option.mproject.
    """

    decoded = pathlib.Path(r'C:\Antigravity\gt4-staticrecomp\private\slice155-spec2-480p\retail_adc')

    def test_the_ntsc_literal_is_unique_at_the_v200_offset(self):
        path = self.decoded / 'projects_option_option.adc.bin'
        if not path.exists():
            self.skipTest('the private v2.00 decoded files are not present')
        data = path.read_bytes()
        self.assertEqual(len(data), 251715)
        self.assertEqual(data.count(b'NTSC'), 1)
        self.assertEqual(data.find(b'NTSC'), 0x247FB)
        self.assertEqual(data[0x247FB:0x247FF], bytes.fromhex('4e545343'))

    def test_the_screen_row_labels_are_unique_and_the_edit_does_not_grow_the_file(self):
        path = self.decoded / 'projects_option_option.mproject.bin'
        if not path.exists():
            self.skipTest('the private v2.00 decoded files are not present')
        text = path.read_text(encoding='utf-8')
        self.assertEqual(len(text.encode('utf-8')), 681977)
        anchors = ['key string {"LabelWide"}', 'key string {"Item4x3"}', 'key string {"Item16x9"}']
        for anchor in anchors:
            self.assertEqual(text.count(anchor), 1, anchor)
        edited = text
        edited = edited.replace('key string {"LabelWide"}', 'text string {"Fullscreen"}')
        edited = edited.replace('key string {"Item4x3"}', 'text string {"Off"}')
        edited = edited.replace('key string {"Item16x9"}', 'text string {"On"}')
        self.assertLessEqual(len(edited.encode('utf-8')), len(text.encode('utf-8')))
        # No minification: the replacement keeps the file's whitespace and pads only the shorter tail.
        self.assertEqual(edited.count('\t'), text.count('\t'))


class Ps2ZipTest(unittest.TestCase):
    def test_round_trip_through_the_catalogue_decoder(self):
        payload = b'Project { name string {"test"} }\n' * 100
        encoded = patch.ps2zip(payload)
        self.assertEqual(gt4_volume.decode_payload(encoded, 2, len(payload)), payload)

    def test_encode_pads_to_the_original_stored_extent_and_verifies(self):
        payload = b'x' * 4096
        encoded = patch.ps2zip(payload)
        record = type('Record', (), {'kind': 2, 'stored_size': len(encoded) + 64, 'size': len(payload)})
        container = patch.encode_same_extent(record, payload)
        self.assertEqual(len(container), record.stored_size)
        patch.verify_round_trip(record, container, payload)
        inflater = zlib.decompressobj(-zlib.MAX_WBITS)
        self.assertEqual(inflater.decompress(container[8:], len(payload) + 1), payload)
        self.assertTrue(inflater.eof)

    def test_a_payload_that_does_not_fit_is_refused(self):
        payload = b'y' * 4096
        encoded = patch.ps2zip(payload)
        record = type('Record', (), {'kind': 2, 'stored_size': len(encoded) - 1, 'size': len(payload)})
        with self.assertRaises(ValueError):
            patch.encode_same_extent(record, payload)


if __name__ == '__main__':
    unittest.main()
