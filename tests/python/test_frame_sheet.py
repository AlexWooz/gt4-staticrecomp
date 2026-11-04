"""TASK-082: the frame by frame check of frame skipping (scripts/coregs2_bench/frame_sheet.py) must catch the first attempt's defect.

Each test makes frames with a number drawn in them, one folder as the reference and one as the run under test. A control that always
passes proves nothing: the wrong sequences here must give a wrong verdict and exit code 1.
"""

import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    import numpy as np
    from PIL import Image  # noqa: F401: the tool needs it
    AVAILABLE = True
except ImportError:
    AVAILABLE = False

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'scripts/coregs2_bench/frame_sheet.py'
CHECK = ROOT / 'scripts/coregs2_bench/frame_skip_check.py'
WIDTH, HEIGHT = 64, 48


def frame_bytes(number):
    """A picture no other number shares: a pattern of blocks from the number's bits."""
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    rng = np.random.default_rng(number)
    blocks = rng.integers(0, 255, size=(6, 8, 3), dtype=np.uint8)
    image[:] = np.kron(blocks, np.ones((8, 8, 1), dtype=np.uint8))
    return b'P6\n%d %d\n255\n' % (WIDTH, HEIGHT) + image.tobytes()


def write_frames(folder, sequence):
    folder.mkdir(parents=True)
    for vblank, number in sequence:
        (folder / f'frame_{vblank:05d}.ppm').write_bytes(frame_bytes(number))


def write_log(path, vblanks, rendered):
    with open(path, 'w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(['vblank', 'rendered'])
        for v in vblanks:
            writer.writerow([v, 1 if rendered(v) else 0])


@unittest.skipUnless(AVAILABLE, 'numpy and Pillow are needed')
class FrameSheetTests(unittest.TestCase):
    def run_tool(self, shown):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            vblanks = range(100, 160)
            write_frames(root / 'reference', [(v, v) for v in vblanks])
            write_frames(root / 'test', [(v, shown(v)) for v in vblanks])
            write_log(root / 'log.csv', vblanks, lambda v: v % 2 == 0)
            done = subprocess.run([sys.executable, str(TOOL), str(root / 'reference'), str(root / 'test'), str(root / 'log.csv'),
                                   str(root / 'out')], capture_output=True, text=True)
            with open(root / 'out/frames.csv', newline='') as file:
                rows = list(csv.DictReader(file))
            self.assertTrue((root / 'out/sheet.png').is_file())
            return done, rows

    def test_every_frame_shown(self):
        done, rows = self.run_tool(lambda v: v)
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual({r['verdict'] for r in rows}, {'ok'})
        self.assertEqual({r['age'] for r in rows}, {'0'})

    def test_one_frame_in_two_holds_the_last(self):
        done, rows = self.run_tool(lambda v: v - v % 2)
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual({r['age'] for r in rows}, {'0', '1'})

    def test_the_first_attempts_defect_an_old_frame_between_new_ones(self):
        done, rows = self.run_tool(lambda v: v if v % 2 == 0 else 100)
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertIn('other', {r['verdict'] for r in rows}, 'an old frame out of the search')

    def test_a_recent_old_frame_between_new_ones_goes_back(self):
        done, rows = self.run_tool(lambda v: v if v % 3 else v - 4)
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertIn('back', {r['verdict'] for r in rows})

    def test_a_frame_that_is_not_of_the_sequence(self):
        done, rows = self.run_tool(lambda v: v if v != 130 else 99999)
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual([r['vblank'] for r in rows if r['verdict'] == 'other'], ['130'])


@unittest.skipUnless(AVAILABLE, 'numpy is needed')
class FrameSkipCheckTests(unittest.TestCase):
    def run_check(self, clock_shift=0, changed_words=()):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name, shift, rendered in (('a', 0, lambda v: True), ('b', clock_shift, lambda v: v % 2 == 0)):
                with open(root / f'{name}.csv', 'w', newline='') as file:
                    writer = csv.writer(file)
                    writer.writerow(['vblank', 'rendered', 'clock'])
                    for v in range(100):
                        writer.writerow([v, 1 if rendered(v) else 0, v * 50 + (shift if v > 50 else 0)])
            state = np.arange(1024, dtype=np.uint32)
            state.tofile(root / 'a.bin')
            state[list(changed_words)] += 1
            state.tofile(root / 'b.bin')
            return subprocess.run([sys.executable, str(CHECK), str(root / 'a.csv'), str(root / 'b.csv'), str(root / 'a.bin'),
                                   str(root / 'b.bin'), '--dynamics-bytes', '0x1000', '--allow-words', '4'], capture_output=True, text=True)

    def test_equal(self):
        self.assertEqual(self.run_check().returncode, 0)

    def test_a_clock_that_differs(self):
        self.assertEqual(self.run_check(clock_shift=50).returncode, 1)

    def test_state_over_what_drawing_writes(self):
        self.assertEqual(self.run_check(changed_words=[1, 2, 3]).returncode, 0)
        self.assertEqual(self.run_check(changed_words=range(10)).returncode, 1)


if __name__ == '__main__':
    unittest.main()
