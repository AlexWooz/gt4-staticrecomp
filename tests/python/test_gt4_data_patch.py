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
