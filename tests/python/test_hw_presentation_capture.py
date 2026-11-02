"""Menu presentation control on a recorded GT4 capture (TASK-027 M6, IN-339).

SPDX-License-Identifier: MIT
Copyright 2026 GT4Recomp contributors.
Written from the TASK-026 specification (docs/hw-renderer-spec), chapters 13.3, 13.4 and 14.3.

GT4 renders its 480i menus one field at a time (224-line buffers, FFMD = 1), each field half a line apart.
The presentation image (13.3 field offset + 13.4 deinterlace) must put the two fields' lines next to each
other; with the wrong field phase every line lands two rows from its partner and text looks combed (the
owner's test 1 on 2026-10-21). This test replays the capture twice through the GPU write-back renderer and
compares a vertical "combing" measure (mean absolute difference between a row and the average of its two
neighbours) on static garage-menu frames:

  - presentation (default `auto` deinterlace) must comb LESS than the line-doubled reference image, and
  - consecutive presentation frames of a static screen must be identical outside the two edge rows.

Game data stays private: set GT4_HW_WRITEBACK_REPLAY_EXE to the hw_writeback_replay executable under test and
GT4_HW_MENU_CAPTURE to a menu capture covering frames 950..1100 (for example the slice217 donor v2 menu
capture). Without both the test is skipped. Exit code 77 of the tool (no Vulkan device) is a skip.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

FRAMES = (1000, 1001, 1050, 1051)


def read_ppm(path):
    data = Path(path).read_bytes()
    parts = data.split(b"\n", 3)
    if parts[0].strip() != b"P6":
        raise AssertionError(f"not a P6 PPM: {path}")
    width, height = (int(v) for v in parts[1].split())
    return width, height, parts[3][: width * height * 3]


def luma_rows(path):
    width, height, rgb = read_ppm(path)
    rows = []
    for y in range(height):
        base = y * width * 3
        rows.append([rgb[base + 3 * x] + rgb[base + 3 * x + 1] + rgb[base + 3 * x + 2] for x in range(width)])
    return rows


def combing(path):
    rows = luma_rows(path)
    total = 0
    count = 0
    for y in range(1, len(rows) - 1):
        above, row, below = rows[y - 1], rows[y], rows[y + 1]
        for x in range(len(row)):
            total += abs(2 * row[x] - above[x] - below[x])
            count += 1
    return total / (6.0 * count)  # luma is a sum of three channels; difference to the neighbour average


@unittest.skipUnless(os.environ.get("GT4_HW_WRITEBACK_REPLAY_EXE") and os.environ.get("GT4_HW_MENU_CAPTURE"),
                     "set GT4_HW_WRITEBACK_REPLAY_EXE and GT4_HW_MENU_CAPTURE (private capture) to run")
class MenuPresentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = Path(os.environ["GT4_HW_WRITEBACK_REPLAY_EXE"]).resolve()
        cls.capture = Path(os.environ["GT4_HW_MENU_CAPTURE"]).resolve()
        for path in (cls.exe, cls.capture):
            if not path.is_file():
                raise AssertionError(f"missing input: {path}")
        cls.temporary = tempfile.TemporaryDirectory(prefix="hw-presentation-")
        root = Path(cls.temporary.name)
        cls.dirs = {}
        for kind in ("reference", "presentation"):
            out = root / kind
            out.mkdir()
            result = subprocess.run([str(cls.exe), str(cls.capture), str(out), "--compose", kind],
                                    capture_output=True, text=True, timeout=1800)
            if result.returncode == 77:
                raise unittest.SkipTest("no Vulkan device: " + result.stderr.strip())
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            cls.dirs[kind] = out

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def frame(self, kind, number):
        path = self.dirs[kind] / f"frame_{number:05d}.ppm"
        self.assertTrue(path.is_file(), f"capture has no frame {number}")
        return path

    def test_presentation_combs_less_than_the_line_doubled_reference(self):
        for number in FRAMES:
            with self.subTest(frame=number):
                presented = combing(self.frame("presentation", number))
                reference = combing(self.frame("reference", number))
                self.assertLess(presented, reference,
                                f"frame {number}: presentation combing {presented:.3f} >= reference {reference:.3f}")

    def test_static_menu_is_stable_between_fields(self):
        for first in (1000, 1050):
            with self.subTest(frame=first):
                a = luma_rows(self.frame("presentation", first))
                b = luma_rows(self.frame("presentation", first + 1))
                interior_differs = sum(1 for y in range(1, len(a) - 1) if a[y] != b[y])
                self.assertEqual(interior_differs, 0, f"{interior_differs} interior rows change between fields")


if __name__ == "__main__":
    unittest.main()
