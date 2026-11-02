#!/usr/bin/env python3
"""Frame comparison helpers for the HWR replay work (TASK-027, chapters 12.9 and 14.4).

Commands:
  identical <dir-a> <dir-b>            per-file sha256 equality, reporting which frames differ
  pct <dir-a> <dir-b> <frame> [...]    percentage of differing RGB pixels for the given frame numbers
  diff <a.ppm> <b.ppm> <out.png>       amplified absolute-difference PNG (4x, for inspection)
  png <in.ppm> <out.png>               PPM to PNG, for looking at a frame without an image tool
  slm <a.bin> <b.bin>                  byte comparison of two 4 MiB shadow local memories
  boxpsnr <scaled.ppm> <native.ppm> <s>  06.7 box downsample of an upscaled frame (s x s average) and its PSNR
                                       against the 1x frame (chapter 06.9 acceptance metric, IN-372)

Frames are 6-bit PPM (P6, maxval 255); the SLM dumps are the raw 4 MiB shadow local memory written by
hw_writeback_replay / hw_cpuref_replay with --slm-frame N.

Copyright 2026 GT4Recomp contributors. MIT licence.
"""

import hashlib
import pathlib
import struct
import sys
import zlib


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    parts = data.split(b"\n", 3)
    if parts[0].strip() != b"P6":
        raise SystemExit(f"not a P6 PPM: {path}")
    width, height = (int(v) for v in parts[1].split())
    return width, height, parts[3]


def write_png(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type 0
        raw += rgb[y * stride : (y + 1) * stride]

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    pathlib.Path(path).write_bytes(png)


def cmd_identical(args):
    left, right = pathlib.Path(args[0]), pathlib.Path(args[1])
    names = sorted(p.name for p in left.glob("*.ppm"))
    differing = []
    for name in names:
        a = hashlib.sha256((left / name).read_bytes()).hexdigest()
        b = hashlib.sha256((right / name).read_bytes()).hexdigest()
        if a != b:
            differing.append(name)
    print(f"{len(names)} frames, {len(differing)} differ")
    if differing:
        print("differing:", ",".join(n[6:11] for n in differing))


def cmd_pct(args):
    left, right = pathlib.Path(args[0]), pathlib.Path(args[1])
    for text in args[2:]:
        frame = int(text)
        name = f"frame_{frame:05d}.ppm"
        width, height, a = read_ppm(left / name)
        _, _, b = read_ppm(right / name)
        bad = 0
        for i in range(width * height):
            if a[i * 3 : i * 3 + 3] != b[i * 3 : i * 3 + 3]:
                bad += 1
        print(f"{name}: {100.0 * bad / (width * height):6.3f}% bad ({bad} pixels)")


def cmd_diff(args):
    left, right, target = args[0], args[1], pathlib.Path(args[2])
    width, height, a = read_ppm(left)
    width_b, height_b, b = read_ppm(right)
    if (width, height) != (width_b, height_b):
        raise SystemExit("frame sizes differ")
    out = bytearray(width * height * 3)
    for i in range(width * height):
        for c in range(3):
            out[i * 3 + c] = min(255, abs(a[i * 3 + c] - b[i * 3 + c]) * 4)
    target.parent.mkdir(parents=True, exist_ok=True)
    write_png(target, width, height, out)
    print(f"wrote {target}")


def cmd_png(args):
    width, height, rgb = read_ppm(args[0])
    target = pathlib.Path(args[1])
    target.parent.mkdir(parents=True, exist_ok=True)
    write_png(target, width, height, rgb)
    print(f"wrote {target} ({width}x{height})")


def cmd_slm(args):
    a = pathlib.Path(args[0]).read_bytes()
    b = pathlib.Path(args[1]).read_bytes()
    if len(a) != len(b):
        raise SystemExit(f"size mismatch: {len(a)} vs {len(b)}")
    differing = [i for i in range(len(a)) if a[i] != b[i]]
    print(f"{len(a)} bytes, {len(differing)} differ")
    if differing:
        buckets = {}
        for i in differing:
            buckets.setdefault(i // 65536, 0)
            buckets[i // 65536] += 1
        hot = sorted(buckets.items(), key=lambda kv: -kv[1])[:6]
        print("hot 64 KiB pages:", ", ".join(f"{page}={count}" for page, count in hot))


def box_downsample(width, height, rgb, scale):
    """06.7 box filter: each native pixel is the rounded average of its s x s block (integer arithmetic)."""
    out_w, out_h = width // scale, height // scale
    out = bytearray(out_w * out_h * 3)
    area = scale * scale
    stride = width * 3
    for oy in range(out_h):
        for ox in range(out_w):
            sums = [0, 0, 0]
            for dy in range(scale):
                row = (oy * scale + dy) * stride + ox * scale * 3
                for dx in range(scale):
                    at = row + dx * 3
                    sums[0] += rgb[at]
                    sums[1] += rgb[at + 1]
                    sums[2] += rgb[at + 2]
            at = (oy * out_w + ox) * 3
            for c in range(3):
                out[at + c] = (sums[c] + area // 2) // area
    return out_w, out_h, bytes(out)


def psnr(a, b):
    """PSNR in dB of two equal-length RGB byte strings (inf when identical)."""
    if len(a) != len(b):
        raise SystemExit(f"size mismatch: {len(a)} vs {len(b)}")
    total = sum((x - y) * (x - y) for x, y in zip(a, b))
    if total == 0:
        return float("inf")
    mse = total / len(a)
    import math
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def cmd_boxpsnr(args):
    scale = int(args[2])
    width, height, rgb = read_ppm(args[0])
    native_w, native_h, native = read_ppm(args[1])
    out_w, out_h, boxed = box_downsample(width, height, rgb, scale)
    if (out_w, out_h) != (native_w, native_h):
        raise SystemExit(f"downsampled {out_w}x{out_h} does not match native {native_w}x{native_h}")
    print(f"box {width}x{height} / {scale} -> {out_w}x{out_h}: PSNR {psnr(boxed, native[: len(boxed)]):.2f} dB")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command, args = sys.argv[1], sys.argv[2:]
    table = {"identical": cmd_identical, "pct": cmd_pct, "diff": cmd_diff, "png": cmd_png, "slm": cmd_slm,
             "boxpsnr": cmd_boxpsnr}
    if command not in table:
        print(__doc__)
        return 2
    table[command](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
