"""Compares a window of consecutive frames of a CoreGS 2.0 replay with the same window on the reference renderer.

usage: frame_compare.py <CoreGS frames dir> <reference frames dir> <first> <last> <out dir> [--shift N] [--sheet A-B ...]
Pairs our frame_N.ppm with the reference's live_frame(N-shift).png. The reference's numbering is one frame ahead on the Ford GT
lap, so the default shift is 1. Writes <out dir>/compare.csv: for each N, the mean difference between the two renderers and of
each renderer against its previous frame, on 480 x 270 thumbnails. A frame that jumps on one renderer only is a glitch candidate;
those are printed. --sheet A-B writes sheet-A-B.png, with ours on the left and the reference on the right.
"""
import argparse
import csv
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

THUMBNAIL = (480, 270)
PANEL = (640, 360)
JUMP_RATIO = 3.0
JUMP_MINIMUM = 6.0


def load(path, size):
    return Image.open(path).convert('RGB').resize(size, Image.BILINEAR)


def as_array(image):
    return np.asarray(image, dtype=np.float32)


def write_sheet(ours_dir, reference_dir, first, last, shift, out_path):
    numbers = range(first, last + 1)
    sheet = Image.new('RGB', (PANEL[0] * 2, PANEL[1] * len(numbers)))
    for row, number in enumerate(numbers):
        for column, path, label in ((0, ours_dir / f'frame_{number:05d}.ppm', f'CoreGS2 {number}'),
                                    (1, reference_dir / f'live_frame{number - shift:05d}.png', f'GSdx {number - shift}')):
            if not path.exists():
                continue
            panel = load(path, PANEL)
            ImageDraw.Draw(panel).text((6, 6), label, fill=(255, 255, 0))
            sheet.paste(panel, (column * PANEL[0], row * PANEL[1]))
    sheet.save(out_path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ours', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('first', type=int)
    parser.add_argument('last', type=int)
    parser.add_argument('out', type=Path)
    parser.add_argument('--shift', type=int, default=1)
    parser.add_argument('--sheet', action='append', default=[])
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=True)

    rows = []
    previous_ours = previous_reference = None
    for number in range(options.first, options.last + 1):
        ours_path = options.ours / f'frame_{number:05d}.ppm'
        reference_path = options.reference / f'live_frame{number - options.shift:05d}.png'
        if not ours_path.exists() or not reference_path.exists():
            previous_ours = previous_reference = None
            continue
        ours = as_array(load(ours_path, THUMBNAIL))
        reference = as_array(load(reference_path, THUMBNAIL))
        rows.append({
            'frame': number,
            'between': float(np.abs(ours - reference).mean()),
            'ours_jump': float(np.abs(ours - previous_ours).mean()) if previous_ours is not None else 0.0,
            'reference_jump': float(np.abs(reference - previous_reference).mean()) if previous_reference is not None else 0.0,
        })
        previous_ours, previous_reference = ours, reference

    with open(options.out / 'compare.csv', 'w', newline='', encoding='ascii') as file:
        writer = csv.DictWriter(file, fieldnames=['frame', 'between', 'ours_jump', 'reference_jump'])
        writer.writeheader()
        for row in rows:
            writer.writerow({key: (value if key == 'frame' else f'{value:.3f}') for key, value in row.items()})

    between = np.array([row['between'] for row in rows]) if rows else np.zeros(1)
    print(f'{len(rows)} pairs; difference between renderers: median {np.median(between):.2f}, max {between.max():.2f}')
    for row in rows:
        ours_only = row['ours_jump'] > JUMP_MINIMUM and row['ours_jump'] > JUMP_RATIO * max(row['reference_jump'], 0.5)
        reference_only = row['reference_jump'] > JUMP_MINIMUM and row['reference_jump'] > JUMP_RATIO * max(row['ours_jump'], 0.5)
        if ours_only or reference_only:
            side = 'ours' if ours_only else 'reference'
            print(f"  frame {row['frame']}: jump on {side} only (ours {row['ours_jump']:.1f}, reference {row['reference_jump']:.1f}, between {row['between']:.1f})")
    for window in options.sheet:
        first, last = (int(part) for part in window.split('-'))
        write_sheet(options.ours, options.reference, first, last, options.shift, options.out / f'sheet-{first}-{last}.png')


if __name__ == '__main__':
    main()
