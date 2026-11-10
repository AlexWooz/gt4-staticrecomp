"""TASK-090 S1 G-GS-CAP: the `.gt4gs` PATH2/PATH3 bytes against the F8 `[frame] gif bytes/frame` counters.

The capture and the performance probe count the same packets from two sides: `.gt4gs` at the EE acceptance point
(`gt4::mtvu::gif_transfer` / `shadow_direct`) and the F8 line at the donor `ps2::gif::render` call. With
`GT4_MTVU_REPORT_EVERY=1` every report line covers one VBlank interval, so the window is a straight sum.

One measured caveat: with IN-461 direct GS intake (the hw renderer default, `GT4_MTVU_DIRECT` unset/1) the VU1 worker's
PATH1/PATH2 output goes to the GS worker without passing `ps2::gif::render`, so the F8 line reports `path2=0` while the
capture still records every PATH2 byte at acceptance. The PATH2 control therefore needs a `GT4_MTVU_DIRECT=0` run; this
script compares PATH2 only when the F8 lines carry it and says so otherwise.

Usage:
    python scripts/coregs3_bench/gs_capture_f8_check.py <probe.log> <gt4_coregs3_gs --stats output> [--window A-B]

It prints the per-frame table and the window totals. Exit code 1 when a compared total differs; per-frame differences
are printed as evidence because the report drain can move a packet across a boundary while the window sum stays exact.
"""

import argparse
import re
import sys

FRAME_LINE = re.compile(
    r'^\[frame\] gif bytes/frame: path1=(\S+) path2=(\S+) path3=(\S+); '
    r'records/frame: path1=(\S+) path2=(\S+) path3=(\S+)$')
STATS_FRAME = re.compile(
    r'^  frame (\d+) vblank (\d+) path2 (\d+) in (\d+) path3 (\d+) in (\d+) total (\d+) refused (\d+)/(\d+)')


def read_f8(path):
    reports = []
    with open(path, encoding='utf-8', errors='replace') as file:
        for line in file:
            match = FRAME_LINE.match(line.rstrip('\r\n'))
            if match:
                reports.append(tuple(float(value) for value in match.groups()))
    return reports


def read_stats(path):
    frames = []
    with open(path, encoding='utf-8', errors='replace') as file:
        for line in file:
            match = STATS_FRAME.match(line.rstrip('\r\n'))
            if match:
                frames.append({
                    'index': int(match.group(1)),
                    'vblank': int(match.group(2)),
                    'path2_bytes': int(match.group(3)),
                    'path2_records': int(match.group(4)),
                    'path3_bytes': int(match.group(5)),
                    'path3_records': int(match.group(6)),
                })
    return frames


def frame_matches(frame, report, compare_path2):
    """The values the two sides share; PATH2 only when the F8 run counts it (direct GS intake off)."""
    same = frame['path3_bytes'] == report[2] and frame['path3_records'] == report[5]
    if compare_path2:
        same = same and frame['path2_bytes'] == report[1] and frame['path2_records'] == report[4]
    return same


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    parser.add_argument('stats')
    parser.add_argument('--window', help='VBlank range A-B; default every frame in the stats file')
    arguments = parser.parse_args()
    reports = read_f8(arguments.log)
    frames = read_stats(arguments.stats)
    if not reports or not frames:
        print(f'no F8 lines ({len(reports)}) or no captured frames ({len(frames)})')
        return 1
    # Direct GS intake sends the worker's PATH1/PATH2 output straight to the GS worker, so the F8 render hook never sees
    # it and its path2 counters stay zero. The capture is unaffected; only the control is.
    compare_path2 = any(report[1] != 0 or report[4] != 0 for report in reports)
    if arguments.window:
        first, last = (int(value) for value in arguments.window.split('-'))
        frames = [frame for frame in frames if first <= frame['vblank'] <= last]
        if not frames:
            print(f'no captured frame in the window {arguments.window}')
            return 1
    # The report at guest VBlank B covers the interval that ends there; the drain phase is measured rather than assumed:
    # the offset with the most exact per-frame matches wins.
    best = None
    for offset in range(-4, 5):
        matches = 0
        usable = True
        for frame in frames:
            index = frame['vblank'] + offset
            if index < 0 or index >= len(reports):
                usable = False
                break
            matches += 1 if frame_matches(frame, reports[index], compare_path2) else 0
        if usable and (best is None or matches > best[1]):
            best = (offset, matches)
    if best is None:
        print('the F8 log is shorter than the capture window')
        return 1
    offset, matches = best
    print(f'frames {len(frames)}, F8 lines {len(reports)}, best offset {offset:+d} ({matches}/{len(frames)} exact), '
          f'PATH2 {"compared" if compare_path2 else "not in the F8 run (direct GS intake)"}')
    print(f"{'vblank':>8} {'p2 B':>9} {'F8':>9} {'p3 B':>10} {'F8':>10} {'p2 rec':>7} {'F8':>5} {'p3 rec':>7} "
          f"{'F8':>5}  exact")
    totals = {'path2_bytes': 0, 'path3_bytes': 0, 'f8_path2_bytes': 0.0, 'f8_path3_bytes': 0.0,
              'path2_records': 0, 'path3_records': 0, 'f8_path2_records': 0.0, 'f8_path3_records': 0.0}
    for frame in frames:
        report = reports[frame['vblank'] + offset]
        exact = frame_matches(frame, report, compare_path2)
        totals['path2_bytes'] += frame['path2_bytes']
        totals['path3_bytes'] += frame['path3_bytes']
        totals['f8_path2_bytes'] += report[1]
        totals['f8_path3_bytes'] += report[2]
        totals['path2_records'] += frame['path2_records']
        totals['path3_records'] += frame['path3_records']
        totals['f8_path2_records'] += report[4]
        totals['f8_path3_records'] += report[5]
        print(f"{frame['vblank']:8d} {frame['path2_bytes']:9d} {report[1]:9.0f} {frame['path3_bytes']:10d} "
              f"{report[2]:10.0f} {frame['path2_records']:7d} {report[4]:5.0f} {frame['path3_records']:7d} "
              f"{report[5]:5.0f}  {'yes' if exact else 'NO'}")
    print('window totals:')
    print(f"  PATH2 bytes   capture {totals['path2_bytes']}  F8 {totals['f8_path2_bytes']:.0f}"
          f"{'' if compare_path2 else ' (not counted by the F8 run)'}")
    print(f"  PATH3 bytes   capture {totals['path3_bytes']}  F8 {totals['f8_path3_bytes']:.0f}")
    print(f"  PATH2 records capture {totals['path2_records']}  F8 {totals['f8_path2_records']:.0f}")
    print(f"  PATH3 records capture {totals['path3_records']}  F8 {totals['f8_path3_records']:.0f}")
    same = (totals['path3_bytes'] == totals['f8_path3_bytes'] and
            totals['path3_records'] == totals['f8_path3_records'] and
            (not compare_path2 or (totals['path2_bytes'] == totals['f8_path2_bytes'] and
                                   totals['path2_records'] == totals['f8_path2_records'])))
    print('G-GS-CAP ' + ('PASS' if same else 'FAIL') +
          ('' if compare_path2 else ' (PATH3 only; PATH2 needs GT4_MTVU_DIRECT=0)'))
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
