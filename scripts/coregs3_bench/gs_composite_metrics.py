#!/usr/bin/env python3
"""TASK-090 S2: the world-vs-reference metrics before and after the captured wet-road composite.

Inputs are the existing evidence images: the CoreGS 2.0 reference frame, an offline world PNG, the M1 world mask and the
car mask. The compared mask is the one the pass-textures round used (the world mask minus the 2 px dilated car mask and
the six measured HUD rectangles), so the numbers are directly comparable with world-final-metrics.json. The script also
reports the composite mask area and the mean luma it adds on that mask, and the linear fit ref = a * world + b.

Usage:
  python scripts/coregs3_bench/gs_composite_metrics.py <ref.ppm> <world.png> <world-mask.pgm> <car-mask.pgm>
      [--composite-mask <pgm>] [--pages <page-fog-map.npz>] [--json <out.json>]
"""
import argparse
import json
import math

import numpy as np
from PIL import Image
from scipy.ndimage import binary_dilation, gaussian_filter

HUD_RECTS = ((0, 0, 168, 82), (168, 0, 500, 88), (500, 0, 640, 58), (0, 330, 78, 418),
             (556, 344, 640, 418), (0, 416, 640, 448))


def rgb(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)


def mask(path):
    if path is None:
        return None
    return np.asarray(Image.open(path).convert('L')) > 0


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def psnr(mse):
    return float('inf') if mse <= 0 else 10.0 * math.log10(255.0 * 255.0 / mse)


def ssim_map(a, b):
    sigma, c1, c2 = 1.5, (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma = gaussian_filter(a, sigma, mode='reflect')
    mb = gaussian_filter(b, sigma, mode='reflect')
    va = gaussian_filter(a * a, sigma, mode='reflect') - ma * ma
    vb = gaussian_filter(b * b, sigma, mode='reflect') - mb * mb
    cov = gaussian_filter(a * b, sigma, mode='reflect') - ma * mb
    return ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))


def fit(x, y):
    n = x.size
    sx, sy, sxx, sxy = x.sum(), y.sum(), (x * x).sum(), (x * y).sum()
    det = n * sxx - sx * sx
    if abs(det) < 1e-9:
        return float('nan'), float('nan')
    return float((n * sxy - sx * sy) / det), float((sy - (n * sxy - sx * sy) / det * sx) / n)


def stats(name, sel, ref, world, lr, lw):
    n = int(sel.sum())
    if n == 0:
        return None
    delta = np.abs(world - ref)
    per_pixel = delta.max(axis=2)[sel]
    mse = float((delta[sel] ** 2).mean())
    x, y = lw[sel], lr[sel]
    corr = float(np.corrcoef(x, y)[0, 1]) if n > 1 and x.std() > 0 and y.std() > 0 else float('nan')
    a, b = fit(x, y)
    out = dict(name=name, pixels=n, luma_ref=float(y.mean()), luma_world=float(x.mean()),
               ratio=float(y.mean() / x.mean()) if x.mean() > 0 else float('nan'), correlation=corr,
               mean_abs=float(per_pixel.mean()), psnr=psnr(mse), pct_bad=float((per_pixel > 8).mean()),
               ssim=float(ssim_map(lr, lw)[sel].mean()), fit_a=a, fit_b=b)
    print(f'{name:16s} px {n:8d} luma {out["luma_ref"]:6.2f} <- {out["luma_world"]:6.2f} ref/world {out["ratio"]:.3f} '
          f'corr {corr:+.3f} mean|d| {out["mean_abs"]:6.2f} PSNR {out["psnr"]:6.2f} bad {100 * out["pct_bad"]:5.1f}% '
          f'SSIM {out["ssim"]:.5f} fit ref = {a:.4f}*world + {b:.3f}')
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('ref')
    parser.add_argument('world')
    parser.add_argument('world_mask')
    parser.add_argument('car_mask')
    parser.add_argument('--composite-mask', default=None)
    parser.add_argument('--pages', default=None)
    parser.add_argument('--json', default=None)
    args = parser.parse_args()

    ref = rgb(args.ref)
    world = rgb(args.world)
    clean = mask(args.world_mask) & ~binary_dilation(mask(args.car_mask), iterations=2)
    hud = np.zeros((448, 640), dtype=bool)
    for x0, y0, x1, y1 in HUD_RECTS:
        hud[y0:y1, x0:x1] = True
    clean = clean & ~np.repeat(np.repeat(hud, 3, 0), 3, 1)
    lr, lw = luma(ref), luma(world)
    summary = dict(all=stats('all', clean, ref, world, lr, lw))
    if args.pages:
        data = np.load(args.pages)
        page = np.repeat(np.repeat(data['page'], 3, 0), 3, 1)
        summary['page36'] = stats('page 0x36D0', clean & (page == 0x36D0), ref, world, lr, lw)
    if args.composite_mask:
        composite = mask(args.composite_mask)
        inside = composite & clean
        if inside.any():
            before = luma(world)
            # The mask PGM stores the destination alpha the composite read (0..255); the added luma is not recoverable
            # from it alone, so the mask's area and the luma of the world inside it are reported here and the added luma
            # is printed by the renderer's report (wet composite raster line).
            summary['composite_mask'] = dict(name='composite mask', pixels=int(composite.sum()),
                                             pixels_inside_clean=int(inside.sum()),
                                             area_fraction_of_clean=float(inside.sum() / max(1, int(clean.sum()))),
                                             luma_world_inside=float(before[inside].mean()),
                                             luma_ref_inside=float(lr[inside].mean()))
            print(f'composite mask    px {int(composite.sum())} (inside the clean mask {int(inside.sum())}, '
                  f'{100.0 * inside.sum() / max(1, int(clean.sum())):.4f}%): world luma '
                  f'{summary["composite_mask"]["luma_world_inside"]:.2f}, ref {summary["composite_mask"]["luma_ref_inside"]:.2f}')
    if args.json:
        with open(args.json, 'w') as handle:
            json.dump(summary, handle, indent=2)
        print('wrote', args.json)


if __name__ == '__main__':
    main()
