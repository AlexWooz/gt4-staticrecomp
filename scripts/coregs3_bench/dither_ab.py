#!/usr/bin/env python3
"""TASK-090 dither option: the A/B between the captured CT16 present and the modern no-dither present.

Two images of the same frame render (`gt4_coregs3_render` with `GT4_COREGS3_DITHER=1` and `=0`, or any pair the caller
wants to compare) are reduced to the numbers that isolate the dither, and optional crops make the dot pattern visible:

  * PSNR, mean |diff|, max |diff|, differing-pixel count and fraction, and the mean signed difference (first image minus
    second), all over the RGB channels;
  * the fraction of channel values divisible by 8 and the number of distinct levels per channel, which show the CT16
    five-bit scanout of the faithful image against the eight-bit modern one;
  * `--crop x,y,w,h` writes the same crop of both images, and `--diff` writes the per-pixel maximum channel difference
    amplified by 8 (clipped), so the 4x4 dither cells are visible without a viewer zoom.

Usage:
  python scripts/coregs3_bench/dither_ab.py <faithful.png> <off.png> [--crop x,y,w,h] [--json out.json]
      [--crop-faithful out.png] [--crop-off out.png] [--diff out.png]

The script uses Pillow only (no numpy): the Python suite runs it under the tooling venv.
"""
import argparse
import json
import math
import sys

from PIL import Image, ImageChops, ImageStat

CHANNELS = (0, 1, 2)  # R, G, B; the alpha lane is a single CT16 bit and is not part of the dither comparison


def open_rgb(path):
    return Image.open(path).convert('RGB')


def band_statistics(image):
    """Per-channel value statistics: divisibility by 8 (the CT16 five-bit expansion) and distinct levels."""
    divisible = 0
    total = 0
    levels = set()
    for channel in CHANNELS:
        histogram = image.getchannel(channel).histogram()
        total += sum(histogram)
        divisible += sum(histogram[value] for value in range(0, 256, 8))
        levels.update(value for value, count in enumerate(histogram) if count)
    return {
        'fraction_divisible_by_8': divisible / total if total else float('nan'),
        'distinct_levels': len(levels),
    }


def compare(a, b):
    """RGB metrics of image `a` against image `b` (both same size)."""
    if a.size != b.size:
        raise SystemExit(f'the images differ in size: {a.size} vs {b.size}')
    pixels = a.size[0] * a.size[1]
    difference = ImageChops.difference(a, b)
    stat = ImageStat.Stat(difference)
    mse = sum(stat.sum2) / (len(CHANNELS) * pixels)
    # The per-pixel maximum of the three channel differences: a pixel differs when any channel does.
    maximum = ImageChops.lighter(ImageChops.lighter(difference.getchannel(0), difference.getchannel(1)),
                                 difference.getchannel(2))
    histogram = maximum.histogram()
    differing = sum(histogram[1:])
    # a - b + 128, so the signed mean comes back with one offset subtraction; useful to see the CT16 bias.
    signed = ImageChops.subtract(a, b, scale=1.0, offset=128)
    mean_signed = sum(ImageStat.Stat(signed).mean) / len(CHANNELS) - 128
    return {
        'pixels': pixels,
        'mse': mse,
        'psnr': float('inf') if mse <= 0 else 10.0 * math.log10(255.0 * 255.0 / mse),
        'mean_abs': sum(stat.sum) / (len(CHANNELS) * pixels),
        'max_abs': max(channel.getextrema()[1] for channel in difference.split()),
        'differing_pixels': differing,
        'differing_fraction': differing / pixels if pixels else float('nan'),
        'mean_signed': mean_signed,
    }


def parse_crop(text):
    try:
        values = [int(part) for part in text.split(',')]
    except ValueError:
        raise SystemExit(f'--crop {text} is not x,y,w,h')
    if len(values) != 4 or values[2] <= 0 or values[3] <= 0:
        raise SystemExit(f'--crop {text} is not x,y,w,h with positive w/h')
    return tuple(values)


def main():
    parser = argparse.ArgumentParser(description='TASK-090 dither A/B metrics')
    parser.add_argument('faithful', help='the image with GT4_COREGS3_DITHER=1 (the captured CT16 present)')
    parser.add_argument('off', help='the image with GT4_COREGS3_DITHER=0 (the modern no-dither present)')
    parser.add_argument('--crop', default=None, help='x,y,w,h: measure and write this crop only')
    parser.add_argument('--json', default=None, help='write the metrics as JSON')
    parser.add_argument('--crop-faithful', default=None, help='write the faithful crop here')
    parser.add_argument('--crop-off', default=None, help='write the off crop here')
    parser.add_argument('--diff', default=None, help='write the amplified per-pixel difference here')
    args = parser.parse_args()

    faithful = open_rgb(args.faithful)
    off = open_rgb(args.off)
    if faithful.size != off.size:
        raise SystemExit(f'the images differ in size: {faithful.size} vs {off.size}')
    crop = None
    if args.crop is not None:
        crop = parse_crop(args.crop)
        if crop[0] < 0 or crop[1] < 0 or crop[0] + crop[2] > faithful.size[0] or crop[1] + crop[3] > faithful.size[1]:
            raise SystemExit(f'--crop {args.crop} is outside the {faithful.size[0]}x{faithful.size[1]} image')
        faithful_crop = faithful.crop((crop[0], crop[1], crop[0] + crop[2], crop[1] + crop[3]))
        off_crop = off.crop((crop[0], crop[1], crop[0] + crop[2], crop[1] + crop[3]))
    else:
        faithful_crop, off_crop = faithful, off

    metrics = {
        'faithful': args.faithful,
        'off': args.off,
        'crop': list(crop) if crop is not None else None,
        'compare': compare(faithful_crop, off_crop),
        'faithful_quantisation': band_statistics(faithful_crop),
        'off_quantisation': band_statistics(off_crop),
    }
    print(f"pixels {metrics['compare']['pixels']}  PSNR {metrics['compare']['psnr']:.2f}  "
          f"mean|d| {metrics['compare']['mean_abs']:.3f}  max|d| {metrics['compare']['max_abs']}  "
          f"differing {metrics['compare']['differing_pixels']} ({100 * metrics['compare']['differing_fraction']:.1f}%)  "
          f"mean signed {metrics['compare']['mean_signed']:+.3f}")
    print(f"faithful: divisible by 8 {100 * metrics['faithful_quantisation']['fraction_divisible_by_8']:.1f}%, "
          f"{metrics['faithful_quantisation']['distinct_levels']} levels; "
          f"off: {100 * metrics['off_quantisation']['fraction_divisible_by_8']:.1f}%, "
          f"{metrics['off_quantisation']['distinct_levels']} levels")

    if args.crop_faithful:
        faithful_crop.save(args.crop_faithful)
    if args.crop_off:
        off_crop.save(args.crop_off)
    if args.diff:
        difference = ImageChops.difference(faithful_crop, off_crop)
        maximum = ImageChops.lighter(ImageChops.lighter(difference.getchannel(0), difference.getchannel(1)),
                                     difference.getchannel(2))
        Image.eval(maximum, lambda value: min(255, value * 8)).save(args.diff)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as handle:
            json.dump(metrics, handle, indent=2)
            handle.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
