#!/usr/bin/env python3
"""TASK-082: game speed and frames drawn of a window run with frame skipping, from its frametime.csv.

The csv has one row per presentation, with its guest VBlank: VBlanks per second over the stretch is the game's speed (60 is 100%), and
presentations per second is what reached the display.

usage: frame_skip_speed.py <frametime.csv> [first VBlank] [last VBlank]
"""
import csv
import sys


def main():
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 2040
    last = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 62
    rows = [r for r in csv.DictReader(open(sys.argv[1], newline='')) if first <= int(r['vblank']) <= last]
    if len(rows) < 2:
        print('no presentations in that stretch')
        return 2
    vblanks = int(rows[-1]['vblank']) - int(rows[0]['vblank'])
    seconds = (float(rows[-1]['present_ms']) - float(rows[0]['present_ms'])) / 1000.0
    steps = [int(b['vblank']) - int(a['vblank']) for a, b in zip(rows, rows[1:])]
    counts = {}
    for step in steps:
        counts[step] = counts.get(step, 0) + 1
    print(f'game speed {vblanks / seconds / 60.0 * 100.0:.1f}%, {len(rows) / seconds:.1f} frames per second presented, '
          f'VBlanks per presentation {dict(sorted(counts.items()))}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
