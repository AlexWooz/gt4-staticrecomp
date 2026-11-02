#!/usr/bin/env python3
"""TASK-082: game speed and frames drawn of a window run with frame skipping, from its frame skip log and frametime.csv.

The log has the wall clock at each guest VBlank's frame: VBlanks per second over the stretch is the game's speed (60 is 100%).
Presentations per second is what reached the display.

usage: frame_skip_speed.py <frame skip log> <frametime.csv> [first VBlank] [last VBlank]
"""
import csv
import sys


def main():
    first = int(sys.argv[3]) if len(sys.argv) > 3 else 2400  # the gate needs a second of race, and the grid the time to come back
    last = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 62
    rows = [r for r in csv.DictReader(open(sys.argv[1], newline='')) if first <= int(r['vblank']) <= last]
    if len(rows) < 2:
        print('no frames in that stretch')
        return 2
    vblanks = int(rows[-1]['vblank']) - int(rows[0]['vblank'])
    seconds = (int(rows[-1]['wall_us']) - int(rows[0]['wall_us'])) / 1e6
    drawn = sum(int(r['rendered']) for r in rows)
    start = int(rows[0]['wall_us']) / 1000.0
    runs = {}
    run = 0
    for row in rows:
        if row['rendered'] == '0':
            run += 1
        else:
            if run:
                runs[run] = runs.get(run, 0) + 1
            run = 0
    print(f'game speed {vblanks / seconds / 60.0 * 100.0:.2f}%, {drawn / seconds:.1f} frames drawn per second, '
          f'runs of frames left out {dict(sorted(runs.items()))}, {seconds:.1f} s')
    present = list(csv.DictReader(open(sys.argv[2], newline='')))
    print(f'presentations in the file: {len(present)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
