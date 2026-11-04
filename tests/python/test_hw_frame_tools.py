"""scripts/hw_frame_tools.py box downsample and PSNR (TASK-027 M12, chapter 06.7/06.9, IN-372)."""

import importlib.util
import math
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('hw_frame_tools', ROOT / 'scripts' / 'hw_frame_tools.py')
tools = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tools)


def write_ppm(path, width, height, rgb):
    path.write_bytes(b'P6\n%d %d\n255\n' % (width, height) + bytes(rgb))


class BoxDownsampleTest(unittest.TestCase):
    def test_uniform_blocks_reduce_to_their_colour(self):
        # 2 x 1 native pixels at scale 2: red and green blocks.
        rgb = bytearray()
        for _y in range(2):
            rgb += bytes([255, 0, 0]) * 2 + bytes([0, 255, 0]) * 2
        width, height, out = tools.box_downsample(4, 2, bytes(rgb), 2)
        self.assertEqual((width, height), (2, 1))
        self.assertEqual(out, bytes([255, 0, 0, 0, 255, 0]))

    def test_rounded_average(self):
        # One 2 x 2 block with values 0, 1, 1, 1 -> (3 + 2) // 4 = 1 (rounded half up).
        rgb = bytes([0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1])
        self.assertEqual(tools.box_downsample(2, 2, rgb, 2)[2], bytes([1, 1, 1]))

    def test_psnr(self):
        self.assertEqual(tools.psnr(b'\x10\x20', b'\x10\x20'), float('inf'))
        self.assertAlmostEqual(tools.psnr(b'\x00', b'\x01'), 10 * math.log10(255 * 255), places=6)

    def test_boxpsnr_command(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            write_ppm(folder / 'scaled.ppm', 4, 4, [100] * 48)
            write_ppm(folder / 'native.ppm', 2, 2, [100] * 12)
            tools.cmd_boxpsnr([str(folder / 'scaled.ppm'), str(folder / 'native.ppm'), '2'])
            write_ppm(folder / 'wrong.ppm', 3, 2, [100] * 18)
            with self.assertRaises(SystemExit):
                tools.cmd_boxpsnr([str(folder / 'scaled.ppm'), str(folder / 'wrong.ppm'), '2'])


if __name__ == '__main__':
    unittest.main()
