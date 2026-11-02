"""Replay a GT4_PGS_CAPTURE stream into any GS backend (TASK-027 M1).

Written from the TASK-026 specification (docs/hw-renderer-spec), chapters 14.3
and 15 (M1), clean room per decision 0069.

The driver knows nothing about the renderer. It walks the records of a capture
and calls a GsBackend: GIF packets, privileged writes, FIFO reads (performed so
readback paths run; the capture stores only the qwords count) and frame ends.
After each frame end it asks the backend for the VRAM-decoded display image and
writes it as a binary PPM named frame_<n>.ppm, the naming of the headless dumps.

Backends:
- RecordingBackend: pure Python, deterministic, used by the tests and as the
  calibration reference for the driver itself.
- Donor and HWR backends are not in this module. The donor GS is a C++ object
  inside the native build; its adapter (a native replay executable speaking
  this interface) is a later M1 step, not part of this commit. HWR adapts
  through the same GsBackend once M0 provides its backend object.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
import hashlib
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import hw_capture  # noqa: E402


@dataclass(frozen=True)
class DisplayImage:
    """A VRAM-decoded display buffer, RGB8, row-major, no padding."""

    width: int
    height: int
    rgb: bytes

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0:
            raise ValueError("display image needs a positive size")
        if len(self.rgb) != self.width * self.height * 3:
            raise ValueError("display image byte count does not match its size")


class GsBackend(ABC):
    """What a replay needs from a GS backend. Implementations must be deterministic."""

    @abstractmethod
    def gif_transfer(self, path: int, data: bytes) -> None:
        """Feed one GIF packet from GIF path 0..3."""

    @abstractmethod
    def write_privileged(self, address: int, value: int) -> None:
        """Write one privileged GS register (guest address, 64-bit value)."""

    @abstractmethod
    def read_transfer_fifo(self, qwords: int) -> bytes:
        """Perform a LOCAL->HOST FIFO read of qwords 64-bit words and return them."""

    @abstractmethod
    def end_frame(self, frame: int) -> None:
        """Called at each 'V' record with the frame number the capture recorded."""

    @abstractmethod
    def display_image(self, frame: int) -> DisplayImage | None:
        """The display buffer of the frame just ended, or None when nothing is shown."""


class DonorBackendUnavailable(GsBackend):
    """Placeholder that fails loudly until the native donor replay adapter exists."""

    def __init__(self) -> None:
        raise NotImplementedError(
            "the donor GS replay adapter is not built yet (TASK-027 M1 follow-up); "
            "use RecordingBackend for driver tests")

    def gif_transfer(self, path, data):  # pragma: no cover - never constructed
        raise NotImplementedError

    def write_privileged(self, address, value):  # pragma: no cover
        raise NotImplementedError

    def read_transfer_fifo(self, qwords):  # pragma: no cover
        raise NotImplementedError

    def end_frame(self, frame):  # pragma: no cover
        raise NotImplementedError

    def display_image(self, frame):  # pragma: no cover
        raise NotImplementedError


class RecordingBackend(GsBackend):
    """Logs every call and derives a display image from the log, so identical input gives identical bytes.

    The image is a width x height grid whose channels come from a SHA-256 of
    the running log at each frame end. It is not a GS model; it only lets the
    tests check that the driver feeds the same calls in the same order.
    """

    def __init__(self, width: int = 16, height: int = 8) -> None:
        self.width = width
        self.height = height
        self.log: list[tuple] = []
        self._digest = hashlib.sha256()

    def _record(self, *entry) -> None:
        self.log.append(entry)
        self._digest.update(repr(entry).encode("ascii"))

    def gif_transfer(self, path, data):
        self._record("G", path, bytes(data))

    def write_privileged(self, address, value):
        self._record("P", address, value)

    def read_transfer_fifo(self, qwords):
        data = bytes((index * 37 + qwords) & 0xFF for index in range(qwords * 8))
        self._record("R", qwords)
        return data

    def end_frame(self, frame):
        self._record("V", frame)

    def display_image(self, frame):
        seed = hashlib.sha256(self._digest.digest() + frame.to_bytes(8, "little")).digest()
        pixels = bytearray()
        for index in range(self.width * self.height * 3):
            pixels.append(seed[index % len(seed)] ^ (index & 0xFF))
        return DisplayImage(self.width, self.height, bytes(pixels))


def write_ppm(path: Path, image: DisplayImage) -> None:
    header = f"P6\n{image.width} {image.height}\n255\n".encode("ascii")
    path.write_bytes(header + image.rgb)


@dataclass
class ReplayResult:
    frames: list[int] = field(default_factory=list)
    frame_sha256: dict[int, str] = field(default_factory=dict)
    gif_packets: int = 0
    privileged_writes: int = 0
    fifo_reads: int = 0


def replay(capture_data: bytes, backend: GsBackend, output_dir: Path | None = None,
           frame_filter: range | None = None) -> ReplayResult:
    """Feed a capture into backend in stream order.

    frame_filter limits which frame ends write a PPM and hash; all records are
    still fed, so state matches a full replay. output_dir, when given, receives
    frame_<n>.ppm for each written frame.
    """
    records = hw_capture.parse_records(capture_data)
    result = ReplayResult()
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
    for record in records:
        if record.tag == "G":
            backend.gif_transfer(record.path, record.data)
            result.gif_packets += 1
        elif record.tag == "P":
            backend.write_privileged(record.address, record.value)
            result.privileged_writes += 1
        elif record.tag == "R":
            backend.read_transfer_fifo(record.qwords)
            result.fifo_reads += 1
        elif record.tag == "V":
            backend.end_frame(record.frame)
            if frame_filter is not None and record.frame not in frame_filter:
                continue
            image = backend.display_image(record.frame)
            if image is None:
                continue
            result.frames.append(record.frame)
            result.frame_sha256[record.frame] = hashlib.sha256(image.rgb).hexdigest()
            if output_dir is not None:
                write_ppm(output_dir / f"frame_{record.frame:05d}.ppm", image)
    return result


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Replay a GS capture into a backend.")
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path, help="directory for frame_<n>.ppm")
    parser.add_argument("--backend", choices=["recording"], default="recording",
                        help="donor and hw adapters are not available yet")
    args = parser.parse_args(argv)
    backend = RecordingBackend()
    try:
        result = replay(args.capture.read_bytes(), backend, args.output)
    except hw_capture.CaptureError as error:
        print(f"invalid capture: {error}", file=sys.stderr)
        return 2
    print(f"frames {len(result.frames)}, gif packets {result.gif_packets}, "
          f"privileged writes {result.privileged_writes}, fifo reads {result.fifo_reads}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
