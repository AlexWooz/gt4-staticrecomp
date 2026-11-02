#!/usr/bin/env python3
"""TASK-082: the simulation with frames left out against the same drive without. Compares two runs of a frame skip log.

The log has the race clock of each frame: for the same drive it must be the same at each VBlank, whatever was drawn. And each run
dumps the simulation state at one VBlank (GT4_FRAME_SKIP_DUMP_AT, native/frame_skip.cpp). The simulation is chaotic: a wrong word
in it spreads through the cars within seconds, so a state equal after thousands of frames left out, except for the few words that
drawing itself writes, says the physics did not notice.

The words drawing writes (found by this comparison on George V Paris, Spec II, and stable over a lap): per car, a few floats per
wheel, at 0x4, 0x8, 0xF8, 0xFC, 0x1EC, 0x1F0, 0x2E0, 0x2E4 and 0x7CC..0x7D8 from the car's 0x6B40 with a stride of 0x4540. `--allow` is
a number of words.

usage: frame_skip_check.py <reference log> <test log> <reference dump> <test dump> --dynamics-bytes N [--allow-words N]
Exit code 1 if the clock differs anywhere, or more state differs than allowed.
"""
import argparse
import csv
import sys

import numpy as np


def read(path):
    rows = {}
    with open(path, newline='') as file:
        for row in csv.DictReader(file):
            rows[int(row['vblank'])] = row
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference')
    parser.add_argument('test')
    parser.add_argument('reference_dump')
    parser.add_argument('test_dump')
    parser.add_argument('--dynamics-bytes', type=lambda text: int(text, 0), required=True)
    parser.add_argument('--allow-words', type=int, default=24)
    options = parser.parse_args()
    reference, test = read(options.reference), read(options.test)
    both = sorted(set(reference) & set(test))
    racing = [v for v in both if reference[v]['clock'] != '0' or test[v]['clock'] != '0']
    clocks = [v for v in racing if reference[v]['clock'] != test[v]['clock']]
    skipped = sum(1 for v in test if test[v]['rendered'] == '0')
    print(f'VBlanks in both {len(both)}, racing {len(racing)}, left out in the test {skipped}')
    print(f'clock differs at {len(clocks)}' + (f', first {clocks[:5]}' if clocks else ''))
    words = options.dynamics_bytes // 4
    a = np.fromfile(options.reference_dump, dtype=np.uint32, count=words)
    b = np.fromfile(options.test_dump, dtype=np.uint32, count=words)
    differing = np.nonzero(a != b)[0] * 4
    print(f'simulation state: {len(differing)} of {words} words differ: ' + ' '.join(hex(int(x)) for x in differing[:48]))
    wrong = bool(clocks) or not racing or skipped == 0 or len(differing) > options.allow_words
    print('FAIL' if wrong else 'OK')
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
