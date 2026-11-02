"""A path in the environment is in the ANSI code page when read in its narrow form, so `u8path` throws on `C:\\Users\\Jo\u00e3o` and the
game closes (owner, 2026-11-02). The renderer reads such paths with `_wgetenv`."""
import re
import unittest
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[2] / 'native'


class PathsFromEnvironment(unittest.TestCase):
    def test_no_path_from_the_environment_is_read_as_utf8(self):
        found = []
        for source in NATIVE.rglob('*.cpp'):
            if 'third_party' in source.parts:
                continue
            for number, line in enumerate(source.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
                if re.search(r'\bu8path\s*\(', line):
                    found.append(f'{source.relative_to(NATIVE)}:{number}')
        self.assertEqual(found, [], 'u8path on a path that may come from the environment')


if __name__ == '__main__':
    unittest.main()
