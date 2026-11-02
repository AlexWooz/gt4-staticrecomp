"""Per-frame comparison of two PPM frame directories for HWR validation (TASK-027 M1).

Written from the TASK-026 specification (docs/hw-renderer-spec), chapter 14.4,
clean room per decision 0069.

Metrics per frame, on RGB (alpha ignored):
- pct_bad: percent of pixels whose max channel |delta| is greater than 8
- max_delta: largest single channel |delta| over the frame
- psnr: 10 log10(255^2 / MSE) over all channel values; "inf" when identical

Gates (chapter 14.4). A gate passes a frame when every bound it states holds.
The scene verdict fails when any frame fails a gating gate. Report-only gates
print the same table and exit 0.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys

PIXEL_DELTA_LIMIT = 8
PEAK = 255


class PpmError(ValueError):
    pass


@dataclass(frozen=True)
class PpmImage:
    width: int
    height: int
    rgb: bytes


def read_ppm(path: Path) -> PpmImage:
    """Read a binary P6 PPM with maxval 255. Comment lines are allowed in the header."""
    data = path.read_bytes()
    tokens: list[bytes] = []
    position = 0
    while len(tokens) < 4:
        while position < len(data) and data[position:position + 1].isspace():
            position += 1
        if position < len(data) and data[position:position + 1] == b"#":
            while position < len(data) and data[position:position + 1] not in (b"\n", b"\r"):
                position += 1
            continue
        start = position
        while position < len(data) and not data[position:position + 1].isspace():
            position += 1
        if start == position:
            raise PpmError(f"{path}: truncated PPM header")
        tokens.append(data[start:position])
    position += 1  # exactly one whitespace byte separates the header from the raster
    if tokens[0] != b"P6":
        raise PpmError(f"{path}: not a binary P6 PPM")
    try:
        width, height, maxval = (int(token) for token in tokens[1:])
    except ValueError as error:
        raise PpmError(f"{path}: non-numeric PPM header") from error
    if maxval != PEAK:
        raise PpmError(f"{path}: maxval {maxval} is not 255")
    raster = data[position:]
    if len(raster) != width * height * 3:
        raise PpmError(f"{path}: raster has {len(raster)} bytes, expected {width * height * 3}")
    return PpmImage(width, height, raster)


@dataclass(frozen=True)
class FrameMetrics:
    pixels: int
    pct_bad: float
    max_delta: int
    psnr: float  # math.inf when identical

    def as_json(self) -> dict:
        return {
            "pixels": self.pixels,
            "pct_bad": self.pct_bad,
            "max_delta": self.max_delta,
            "psnr": "inf" if math.isinf(self.psnr) else round(self.psnr, 4),
        }


def frame_metrics(reference: PpmImage, candidate: PpmImage) -> FrameMetrics:
    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise PpmError(f"size mismatch {reference.width}x{reference.height} vs "
                       f"{candidate.width}x{candidate.height}")
    if reference.rgb == candidate.rgb:
        pixels = reference.width * reference.height
        return FrameMetrics(pixels, 0.0, 0, math.inf)
    deltas = [abs(a - b) for a, b in zip(reference.rgb, candidate.rgb)]
    pixels = reference.width * reference.height
    bad = sum(1 for r, g, b in zip(deltas[0::3], deltas[1::3], deltas[2::3])
              if max(r, g, b) > PIXEL_DELTA_LIMIT)
    squared = sum(value * value for value in deltas)
    mse = squared / len(deltas)
    psnr = math.inf if mse == 0 else 10.0 * math.log10(PEAK * PEAK / mse)
    return FrameMetrics(pixels, 100.0 * bad / pixels, max(deltas), psnr)


@dataclass(frozen=True)
class Gate:
    name: str
    description: str
    max_pct_bad: float | None
    min_psnr: float | None
    identical: bool = False
    gating_from: str | None = None  # milestone after which the gate gates; None = gating now

    def passes(self, metrics: FrameMetrics) -> bool:
        if self.identical:
            return metrics.pct_bad == 0.0 and metrics.max_delta == 0
        if self.max_pct_bad is not None and metrics.pct_bad > self.max_pct_bad:
            return False
        if self.min_psnr is not None and metrics.psnr < self.min_psnr:
            return False
        return True

    def bounds_text(self) -> str:
        if self.identical:
            return "identical"
        parts = []
        if self.max_pct_bad is not None:
            parts.append(f"pct_bad <= {self.max_pct_bad}%")
        if self.min_psnr is not None:
            parts.append(f"PSNR >= {self.min_psnr} dB")
        return " and ".join(parts)


# Chapter 14.4 rows. A gate with gating_from is report-only until that milestone.
GATES: dict[str, Gate] = {
    "calibration": Gate("calibration", "donor vs donor, HWR vs HWR on the same input (nondeterminism)",
                        None, None, identical=True),
    "cache_vs_writeback_1x": Gate("cache_vs_writeback_1x", "HWR cache vs HWR writeback mode, 1x (Decision)",
                                  0.01, 60.0),
    "donor_2d_1x": Gate("donor_2d_1x", "HWR vs donor, 2D/menu scenes, 1x",
                        0.5, 40.0, gating_from="M8"),
    "donor_3d_1x": Gate("donor_3d_1x", "HWR vs donor, 3D scenes, 1x",
                        3.0, 32.0, gating_from="M10"),
    "paralle_mip_dither_1x": Gate("paralle_mip_dither_1x", "HWR vs paraLLEl-GS, mip/dither scenes, 1x",
                                  3.0, 32.0, gating_from="M13"),
    "downsample_3d": Gate("downsample_3d", "HWR Nx box-downsampled vs HWR 1x, 3D (Hypothesis, tuned in M12)",
                          None, 30.0, gating_from="M12"),
    "downsample_menu": Gate("downsample_menu", "HWR Nx box-downsampled vs HWR 1x, menus (Hypothesis, tuned in M12)",
                            None, 35.0, gating_from="M12"),
}


@dataclass
class FrameRow:
    name: str
    metrics: FrameMetrics
    passed: bool


@dataclass
class SceneReport:
    gate: Gate
    rows: list[FrameRow]
    missing: list[str]
    report_only: bool

    @property
    def frames_over_gate(self) -> int:
        return sum(1 for row in self.rows if not row.passed)

    @property
    def verdict(self) -> str:
        if self.report_only:
            return "report-only"
        if not self.rows:
            return "fail"
        return "pass" if self.frames_over_gate == 0 and not self.missing else "fail"


def compare_directories(reference_dir: Path, candidate_dir: Path, gate: Gate,
                        report_only: bool = False) -> SceneReport:
    references = {path.name: path for path in sorted(reference_dir.glob("*.ppm"))}
    candidates = {path.name: path for path in sorted(candidate_dir.glob("*.ppm"))}
    missing = sorted(set(references) ^ set(candidates))
    rows: list[FrameRow] = []
    for name in sorted(set(references) & set(candidates)):
        metrics = frame_metrics(read_ppm(references[name]), read_ppm(candidates[name]))
        rows.append(FrameRow(name, metrics, gate.passes(metrics)))
    if not report_only and gate.gating_from is not None:
        report_only = True  # chapter 14.4: report-only until the named milestone
    return SceneReport(gate, rows, missing, report_only)


def format_table(report: SceneReport) -> str:
    lines = [
        f"gate {report.gate.name}: {report.gate.description} [{report.gate.bounds_text()}]",
        f"{'frame':<22}{'pct_bad %':>11}{'max_delta':>11}{'psnr dB':>12}  result",
    ]
    for row in report.rows:
        psnr = "inf" if math.isinf(row.metrics.psnr) else f"{row.metrics.psnr:.3f}"
        result = "ok" if row.passed else "OVER"
        lines.append(f"{row.name:<22}{row.metrics.pct_bad:>11.4f}{row.metrics.max_delta:>11d}"
                     f"{psnr:>12}  {result}")
    for name in report.missing:
        lines.append(f"{name:<22} present in only one directory")
    lines.append(f"frames {len(report.rows)}, over gate {report.frames_over_gate}, "
                 f"missing {len(report.missing)}, verdict {report.verdict}")
    return "\n".join(lines)


def report_json(report: SceneReport, reference_dir: Path, candidate_dir: Path) -> dict:
    return {
        "gate": report.gate.name,
        "gate_description": report.gate.description,
        "bounds": report.gate.bounds_text(),
        "gating_from": report.gate.gating_from,
        "report_only": report.report_only,
        "reference": str(reference_dir),
        "candidate": str(candidate_dir),
        "frames": [{"name": row.name, "passed": row.passed, **row.metrics.as_json()} for row in report.rows],
        "missing": report.missing,
        "frames_over_gate": report.frames_over_gate,
        "verdict": report.verdict,
    }


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Compare HWR frame dumps against chapter 14.4 gates.")
    parser.add_argument("reference", type=Path, help="directory of reference PPMs (donor)")
    parser.add_argument("candidate", type=Path, help="directory of candidate PPMs (HWR)")
    parser.add_argument("--gate", choices=sorted(GATES), required=True)
    parser.add_argument("--report-only", action="store_true", help="print the verdict but exit 0")
    parser.add_argument("--json", type=Path, help="write the JSON report here")
    args = parser.parse_args(argv)
    report = compare_directories(args.reference, args.candidate, GATES[args.gate], args.report_only)
    print(format_table(report))
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report_json(report, args.reference, args.candidate), indent=2) + "\n",
                             encoding="utf-8")
    return 0 if report.verdict in ("pass", "report-only") else 1


if __name__ == "__main__":
    raise SystemExit(main())
