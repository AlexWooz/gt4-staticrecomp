"""TASK-095: is the candidate's 2D overlay the reference's narrowed by s = R / (16/9) around the frame's centre?

Two runs of one recording at the same ratio R: the reference build (overlays on the game's own canvas, stretched with the frame
when the presentation fills the screen) and the candidate (overlay canvas widened by s, so the overlay is drawn s times narrower
in the frame and the presentation's stretch gives it back its 16:9 shape). The race is the same in both, so a frame differs only
where an overlay is drawn. For each frame that differs this compares the candidate with two predictions inside the differing
pixels of the centred 16:9 box (the 640 / s middle columns, where the candidate's overlay now lies): the reference as it is, and
the reference narrowed by s around the centre column (each row resampled at x_ref = c + (x - c) * s). Where the overlay is
opaque, the narrowed reference explains the candidate and the plain one does not; under translucent parts the scene behind is
not narrowed, so neither is exact and the check is relative: the narrowed error must be clearly below the plain one (`--limit`,
default 0.6 of it). Outside the box the candidate shows the scene where the reference's wider overlay was; a full-screen layer
of the page (a dim or a fade) is narrowed with it, so the sides are listed (pixels that differ outside the box) but not judged.
Frames that differ in fewer than `--min-pixels` pixels inside the box are listed but not judged.

usage: overlay_geometry.py <reference frames> <candidate frames> <R as W:H or a decimal> [--limit F] [--min-pixels N]
Exit code 1 when a judged frame fails, or no frame differs (nothing was checked).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def ratio_of(text):
    if ':' in text:
        w, h = text.split(':')
        return float(w) / float(h)
    return float(text)


def narrowed(frame, s):
    """Each row resampled at x_ref = c + (x - c) * s (linear), c the frame's centre; outside the frame: the edge pixel."""
    height, width, _ = frame.shape
    c = (width - 1) / 2.0
    x = c + (np.arange(width) - c) * s
    x = np.clip(x, 0, width - 1)
    left = np.floor(x).astype(int)
    right = np.minimum(left + 1, width - 1)
    t = (x - left)[None, :, None]
    return frame[:, left, :] * (1 - t) + frame[:, right, :] * t


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('ratio')
    parser.add_argument('--limit', type=float, default=0.6)
    parser.add_argument('--min-pixels', type=int, default=2000)
    options = parser.parse_args()
    s = ratio_of(options.ratio) / (16.0 / 9.0)
    judged = failed = 0
    for path in sorted(options.reference.glob('*.ppm')):
        other = options.candidate / path.name
        if not other.exists():
            continue
        ref = np.asarray(Image.open(path).convert('RGB')).astype(np.float64)
        cand = np.asarray(Image.open(other).convert('RGB')).astype(np.float64)
        if ref.shape != cand.shape:
            print(f'{path.name}: size differs')
            failed += 1
            continue
        differ = np.abs(ref - cand).max(axis=2) > 8
        if not differ.any():
            continue
        width = ref.shape[1]
        centre = (width - 1) / 2.0
        x = np.arange(width)
        inside = np.abs(x - centre) <= width / (2.0 * s)
        mask = differ & inside[None, :]
        outside = int((differ & ~inside[None, :]).sum())
        count = int(mask.sum())
        if count == 0:
            print(f'{path.name}: differs only outside the centred box ({outside} px)')
            continue
        columns = np.nonzero(mask.any(axis=0))[0]
        rows = np.nonzero(mask.any(axis=1))[0]
        plain = np.abs(cand - ref).max(axis=2)[mask].mean()
        narrow = np.abs(cand - narrowed(ref, s)).max(axis=2)[mask].mean()
        verdict = 'small'
        if count >= options.min_pixels:
            judged += 1
            verdict = 'ok' if narrow <= options.limit * plain else 'WRONG'
            failed += verdict == 'WRONG'
        print(f'{path.name}: {count} px differ in the box, {outside} outside; columns {columns[0]}..{columns[-1]} (centre '
              f'{(columns[0] + columns[-1]) / 2:.1f} of {centre:.1f}), rows {rows[0]}..{rows[-1]}; mean error against the reference '
              f'{plain:.1f}, against it narrowed by 1/{s:.4f} {narrow:.1f}: {verdict}')
    print(f'overlay geometry at s = {s:.4f}: {judged} frames judged, {failed} wrong')
    sys.exit(0 if judged and not failed else 1)


if __name__ == '__main__':
    main()
