#!/usr/bin/env python3
"""TASK-090 texture-mip: the per-page palette/texel census.

Reads the world-stage sample dump (gt4_coregs3_render --sample-dump, 24 B/px) and the reference frame, inverts the
captured post chain (FIX 127, FIX 255, CT16 present with the captured DIMX dither) on the reference, undoes the fog and
MODULATE with the per-pixel values the renderer recorded, and compares the reference-implied texel with the decoded
texel. Per page it reports:

  * texel ratio  = mean implied texel luma / mean decoded texel luma (the study's fix-3 gate: 1.0 +/- 0.1);
  * world ratio  = mean implied world luma / mean rendered world luma (chain-consistent level);
  * corr         = per-pixel correlation of the implied and decoded texel luma (content registration);
  * tone slope   = slope of the binned mean(implied | decoded texel) curve (1.0 = the palette transfer is right);
  * scatter      = residual of that binned curve (content/palette-vs-content split).

The chain is deterministic per dither phase, so its inverse is built exactly from the captured DIMX and the formula the
tool applies (`apply_full_screen_multiply` and `apply_present_ct16`).
"""
import argparse
import json
import struct

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

HUD_RECTS = ((0, 0, 168, 82), (168, 0, 500, 88), (500, 0, 640, 58), (0, 330, 78, 418),
             (556, 344, 640, 418), (0, 416, 640, 448))


def read_dump(path):
    with open(path, 'rb') as handle:
        header = struct.unpack('<5I', handle.read(20))
        magic, version, width, height, stride = header
        assert magic == 0x53345447 and version == 1 and stride == 24, header
        data = np.fromfile(handle, dtype=np.uint8)
    assert data.size == width * height * stride, (data.size, width, height)
    data = data.reshape(height, width, stride)
    return dict(width=width, height=height,
                flags=data[..., 0], level=data[..., 1],
                texel=data[..., 3:7].astype(np.float64),
                vertex=data[..., 7:11].astype(np.float64),
                fragment=data[..., 11:15].astype(np.float64),
                fog=data[..., 15].astype(np.float64),
                fog_colour=data[..., 16:19].astype(np.float64),
                page=(data[..., 19].astype(np.int32) | (data[..., 20].astype(np.int32) << 8)))


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def parse_dither(report):
    """The present's DIMX and DTHE from the tool's chain report line."""
    with open(report, 'r', errors='replace') as handle:
        for line in handle:
            if 'present-ct16' in line and 'DIMX' in line:
                words = line.split()
                dthe = int(words[words.index('DTHE') + 1])
                dimx = int(words[words.index('DIMX') + 1], 16)
                return dthe, dimx
    raise SystemExit('no present-ct16 DIMX line in %s' % report)


def chain_forward(values, fix, clamp=True):
    out = (values.astype(np.int64) * fix) >> 7
    if clamp:
        out = np.clip(out, 0, 255)
    return out


def build_chain_lut(dimx):
    """Per phase (4x4), the inverse of world value -> scanout value: F -> the middle world value that maps to it."""
    inv = np.zeros((16, 256), dtype=np.float64)
    valid = np.zeros((16, 256), dtype=bool)
    world = np.arange(256, dtype=np.int64)
    for phase in range(16):
        field = (dimx >> (4 * phase)) & 7
        dither = field - 8 if field >= 4 else field
        v = chain_forward(world, 127)
        v = chain_forward(v, 255)
        final = np.clip(v + dither, 0, 255)
        final = ((final >> 3) & 0x1F) << 3
        for value in range(256):
            preimage = world[final == value]
            if preimage.size:
                inv[phase, value] = preimage.mean()
                valid[phase, value] = True
    return inv, valid


def inverse_reference(reference, inv, valid):
    """The reference's scanout, inverted to the world stage per pixel phase (x/3, y/3)."""
    height, width = reference.shape[:2]
    sx = (np.arange(width) // 3) & 3
    sy = (np.arange(height) // 3) & 3
    phase = (sy[:, None] * 4 + sx[None, :]).astype(np.int32)
    out = np.zeros_like(reference)
    valid_mask = np.ones(reference.shape[:2], dtype=bool)
    for channel in range(3):
        values = reference[..., channel].astype(np.int64)
        lut = inv[phase, values]  # gather per pixel
        mask = valid[phase, values]
        out[..., channel] = lut
        valid_mask &= mask
    return out, valid_mask


def fog_undo(world, fog, fog_colour):
    """Undoes the GS fog mix ((F*M + (255-F)*FC) >> 8) with the recorded F and FOGCOL."""
    f = fog[..., None]
    fc = fog_colour
    numerator = world * 256.0 - (255.0 - f) * fc
    out = np.zeros_like(world)
    usable = f[..., 0] >= 16.0
    np.divide(numerator, np.maximum(f, 1.0), out=out)
    return out, usable


def stats(name, sel, implied, decoded, implied_world, world, ref_world):
    n = int(sel.sum())
    if n == 0:
        return None
    di = implied[sel]
    dd = decoded[sel]
    li = luma(di)
    ld = luma(dd)
    world_ratio = float(luma(implied_world[sel]).mean() / max(1e-9, luma(world[sel]).mean()))
    corr = float(np.corrcoef(ld, li)[0, 1]) if ld.std() > 0 and li.std() > 0 else float('nan')
    # The tone curve: bins over the decoded texel luma, the mean implied luma per bin.
    bins = np.linspace(0.0, 255.0, 17)
    centres, means, weights = [], [], []
    for index in range(16):
        inside = (ld >= bins[index]) & (ld < bins[index + 1])
        if inside.sum() < 50:
            continue
        centres.append(float(ld[inside].mean()))
        means.append(float(li[inside].mean()))
        weights.append(int(inside.sum()))
    if len(centres) >= 2:
        a, b = np.polyfit(np.array(centres), np.array(means), 1, w=np.sqrt(np.array(weights)))
        residual = float(np.sqrt(np.average((np.array(means) - (a * np.array(centres) + b)) ** 2,
                                            weights=np.array(weights))))
    else:
        a = b = float('nan')
        residual = float('nan')
    result = dict(name=name, pixels=n,
                  texel_decoded=float(ld.mean()), texel_implied=float(li.mean()),
                  texel_ratio=float(li.mean() / max(1e-9, ld.mean())),
                  world_ratio=world_ratio, corr=corr,
                  tone_slope=float(a), tone_offset=float(b), tone_residual=residual,
                  implied_world_luma=float(luma(implied_world[sel]).mean()),
                  world_luma=float(luma(world[sel]).mean()),
                  ref_world_luma=float(luma(ref_world[sel]).mean()))
    print('%-10s px %8d texel implied %6.2f decoded %6.2f ratio %.3f | world implied %6.2f rendered %6.2f ratio '
          '%.3f | corr %+.3f tone %.3f*w%+.2f res %.2f' %
          (name, n, float(li.mean()), float(ld.mean()), li.mean() / max(1e-9, ld.mean()),
           result['implied_world_luma'], result['world_luma'], world_ratio, corr, a, b, residual))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dump')
    parser.add_argument('reference')
    parser.add_argument('world')
    parser.add_argument('report', help='gt4_coregs3_render report with the present DIMX line')
    parser.add_argument('--pages', default=None, help='page-state-map.npz')
    parser.add_argument('--mask', default=None, help='coverage mask PGM')
    parser.add_argument('--car-mask', default=None)
    parser.add_argument('--clean-hud', action='store_true')
    parser.add_argument('--json', default=None)
    args = parser.parse_args()

    dump = read_dump(args.dump)
    ref = np.asarray(Image.open(args.reference).convert('RGB'), dtype=np.float64)
    world = np.asarray(Image.open(args.world).convert('RGB'), dtype=np.float64)
    if ref.shape != world.shape:
        raise SystemExit('reference %s and world %s differ in size' % (ref.shape, world.shape))
    dthe, dimx = parse_dither(args.report)
    print('chain: DTHE %d DIMX %08X' % (dthe, dimx))
    inv, valid = build_chain_lut(dimx)
    ref_world, ref_valid = inverse_reference(ref, inv, valid)
    ref_world = np.clip(ref_world, 0.0, 255.0)

    textured = (dump['flags'] & 1) != 0
    fogged = (dump['flags'] & 2) != 0
    sel = textured & ref_valid
    if args.mask:
        coverage = np.asarray(Image.open(args.mask).convert('L')) > 0
        sel &= coverage
    else:
        coverage = np.ones(sel.shape, dtype=bool)
    if args.clean_hud:
        hud = np.zeros((448, 640), dtype=bool)
        for x0, y0, x1, y1 in HUD_RECTS:
            hud[y0:y1, x0:x1] = True
        sel &= ~np.repeat(np.repeat(hud, 3, 0), 3, 1)
    if args.car_mask:
        from scipy.ndimage import binary_dilation
        car = np.asarray(Image.open(args.car_mask).convert('L')) > 0
        sel &= ~binary_dilation(car, iterations=2)

    implied_modulate, fog_ok = fog_undo(ref_world, dump['fog'], dump['fog_colour'])
    implied_modulate = np.where(fogged[..., None], implied_modulate, ref_world)
    vertex = np.maximum(dump['vertex'][..., :3], 1.0)
    implied_texel = implied_modulate * 128.0 / vertex
    usable = sel & (vertex[..., 0] >= 8.0) & (vertex[..., 1] >= 8.0) & (vertex[..., 2] >= 8.0)
    usable &= fog_ok | ~fogged

    summary = dict(chain=dict(dthe=dthe, dimx='%08X' % dimx), pages={})
    all_row = stats('all', usable, implied_texel, dump['texel'], implied_modulate, world, ref_world)
    summary['pages']['all'] = all_row
    if args.pages:
        page = np.repeat(np.repeat(np.load(args.pages)['page'], 3, 0), 3, 1)
        for value in sorted(set(np.unique(page[usable]).tolist())):
            if value == 0:
                continue
            row = stats('0x%04X' % value, usable & (page == value), implied_texel, dump['texel'], implied_modulate,
                        world, ref_world)
            summary['pages']['0x%04X' % value] = row
    if args.json:
        with open(args.json, 'w') as handle:
            json.dump(summary, handle, indent=2)
        print('wrote', args.json)


if __name__ == '__main__':
    main()
