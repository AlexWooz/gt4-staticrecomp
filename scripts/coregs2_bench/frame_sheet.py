#!/usr/bin/env python3
"""TASK-082: frame by frame check of frame skipping, and a sheet to look at.

The same recorded drive runs twice, headless, with a dump of every VBlank: once with no frame left out (the reference) and once with
frames left out. The simulation is the same, so each frame shown in the second run must be a frame of the reference: the one of
that VBlank, or of the last VBlank that drew. The first attempt at frame skipping failed exactly here: it showed older frames
between newer ones (a total time of 0:00.983 between 0:06.433 and 0:06.466).

For each VBlank this finds the reference frame nearest to what was shown, among the `--search` VBlanks before it, and writes:
  frames.csv  vblank, rendered, reference frame shown, how old it is, differing pixels, verdict
  sheet.png   thumbnails in order, with the verdicts; a wrong one has a red border
  summary.txt the counts
Verdicts: ok; old (older than the frames left out allow); back (older than the frame shown before it); other (no reference frame is
near it: not a frame of the game's sequence). A few pixels may differ: the crowd's camera flashes differ between two runs of the
same build, so run it with two reference runs (`--noise`) to see how much.

usage: frame_sheet.py <reference frames> <test frames> <test log csv> <out dir> [--search N] [--max-age N] [--noise <frames>] [--thumb W]
Exit code 1 if any verdict is wrong.
"""
import argparse
import csv
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

NEAR_FRACTION = 0.02  # of the pixels: more than this differing is not the same frame


def read_ppm(path):
    data = open(path, 'rb').read()
    parts, at = [], 0
    while len(parts) < 4:
        end = at
        while data[end] not in b' \n\t\r':
            end += 1
        parts.append(data[at:end])
        at = end + 1
    width, height = int(parts[1]), int(parts[2])
    return np.frombuffer(data, dtype=np.uint8, offset=at, count=width * height * 3).reshape(height, width, 3)


def frames_of(folder):
    found = {}
    for name in os.listdir(folder):
        match = re.search(r'(\d+)\.ppm$', name)
        if match:
            found[int(match.group(1))] = os.path.join(folder, name)
    return found


def differing(a, b):
    if a.shape != b.shape:
        return a.shape[0] * a.shape[1]
    return int(np.count_nonzero(np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2) > 8))


def read_log(path):
    """{vblank: rendered} from the frame skip log: a decision taken after that VBlank."""
    rows = {}
    with open(path, newline='') as file:
        for row in csv.DictReader(file):
            rows[int(row['vblank'])] = int(row['rendered'])
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference')
    parser.add_argument('test')
    parser.add_argument('log')
    parser.add_argument('out')
    parser.add_argument('--search', type=int, default=8)
    parser.add_argument('--max-age', type=int, default=4, help='frames behind the reference that are still fine')
    parser.add_argument('--noise', help='a second reference run: its differing pixels show what is noise')
    parser.add_argument('--thumb', type=int, default=320)
    parser.add_argument('--columns', type=int, default=6)
    parser.add_argument('--rows', type=int, default=20)
    options = parser.parse_args()
    os.makedirs(options.out, exist_ok=True)

    reference = frames_of(options.reference)
    test = frames_of(options.test)
    log = read_log(options.log)
    noise = frames_of(options.noise) if options.noise else {}
    # The first ones have no older reference frames to be matched with.
    vblanks = sorted(set(test) & set(reference))[options.search:]
    if not vblanks:
        print('no frames in common')
        return 2

    cache = {}

    def ref_image(v):
        if v not in cache:
            if len(cache) > 32:
                cache.pop(min(cache))
            cache[v] = read_ppm(reference[v])
        return cache[v]

    results = []
    last_shown = None
    near = None
    for v in vblanks:
        shown = read_ppm(test[v])
        if near is None:
            near = int(shown.shape[0] * shown.shape[1] * NEAR_FRACTION)
        best, best_diff = None, None
        for candidate in range(v, v - options.search - 1, -1):
            if candidate not in reference:
                continue
            diff = differing(shown, ref_image(candidate))
            if best_diff is None or diff < best_diff:
                best, best_diff = candidate, diff
            if diff == 0:
                break
        age = v - best
        if best_diff > near:
            verdict = 'other'
        elif last_shown is not None and best < last_shown:
            verdict = 'back'
        elif age > options.max_age:
            verdict = 'old'
        else:
            verdict = 'ok'
        noise_diff = ''
        if v in noise:
            noise_diff = differing(read_ppm(noise[v]), ref_image(v))
        results.append({'vblank': v, 'rendered': log.get(v, ''), 'shows': best, 'age': age, 'differing_pixels': best_diff,
                        'noise_pixels': noise_diff, 'verdict': verdict})
        if verdict in ('ok', 'old'):
            last_shown = best

    with open(os.path.join(options.out, 'frames.csv'), 'w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)

    counts = {}
    for row in results:
        counts[row['verdict']] = counts.get(row['verdict'], 0) + 1
    ages = {}
    for row in results:
        ages[row['age']] = ages.get(row['age'], 0) + 1
    distinct = len({row['shows'] for row in results})
    exact = sum(1 for row in results if row['differing_pixels'] == 0)
    largest = max(row['differing_pixels'] for row in results)
    lines = [f'frames {len(results)}, VBlanks {vblanks[0]}-{vblanks[-1]}',
             f'verdicts {counts}',
             f'distinct frames shown {distinct}',
             f'how old what is shown is, in VBlanks: {dict(sorted(ages.items()))}',
             f'exact {exact}, largest difference {largest} pixels']
    wrong = [row for row in results if row['verdict'] != 'ok']
    if wrong:
        lines.append('first wrong: ' + ', '.join(f"{r['vblank']}:{r['verdict']}" for r in wrong[:10]))
    with open(os.path.join(options.out, 'summary.txt'), 'w') as file:
        file.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))

    # The wrong frames first, then from the start, up to one sheet.
    picked = (wrong[:options.columns * 2] + results)[:options.columns * options.rows]
    picked.sort(key=lambda row: row['vblank'])
    first = read_ppm(test[picked[0]['vblank']])
    width = options.thumb
    height = width * first.shape[0] // first.shape[1]
    pad = 18
    sheet = Image.new('RGB', (options.columns * (width + 4), options.rows * (height + pad + 4)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, row in enumerate(picked):
        image = Image.fromarray(read_ppm(test[row['vblank']])).resize((width, height), Image.BILINEAR)
        x = (i % options.columns) * (width + 4) + 2
        y = (i // options.columns) * (height + pad + 4) + 2
        sheet.paste(image, (x, y + pad))
        state = 'draw' if row['rendered'] == 1 else 'skip' if row['rendered'] == 0 else '?'
        good = row['verdict'] == 'ok'
        draw.text((x + 2, y + 3), f"V{row['vblank']} {state} shows V{row['shows']} age {row['age']} {row['verdict']}",
                  fill=(210, 230, 210) if good else (255, 90, 90))
        if not good:
            draw.rectangle((x, y + pad, x + width - 1, y + pad + height - 1), outline=(255, 40, 40), width=3)
    sheet.save(os.path.join(options.out, 'sheet.png'))
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
