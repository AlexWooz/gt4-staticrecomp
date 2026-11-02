"""Reader and validator for GT4_PGS_CAPTURE GS stream captures (TASK-027 M1).

Written from the TASK-026 specification (docs/hw-renderer-spec), chapter 14.3,
clean room per decision 0069. The record layout is the one the donor-side
writer emits (GT4_PGS_CAPTURE on task/012-parallel-gs); all integers are
little-endian:

    'G'  u8 path, u32 bytes, bytes[bytes]   GIF packet for a GIF path (0..3)
    'P'  u32 address, u64 value             privileged register write
    'R'  u32 qwords                         LOCAL->HOST FIFO read (data not stored)
    'V'  u64 frame                          end of frame; records before it belong to it

Captures are game data and stay under private/. This module never writes a
capture; it only parses and validates them.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import struct

GIF_PATH_COUNT = 4
MAX_GIF_BYTES = 64 * 1024 * 1024

TAG_GIF = ord("G")
TAG_PRIVILEGED = ord("P")
TAG_FIFO_READ = ord("R")
TAG_FRAME_END = ord("V")

TAG_NAMES = {
    TAG_GIF: "G",
    TAG_PRIVILEGED: "P",
    TAG_FIFO_READ: "R",
    TAG_FRAME_END: "V",
}


class CaptureError(ValueError):
    """Raised when a capture stream is malformed. The message names the byte offset."""


@dataclass(frozen=True)
class Record:
    tag: str
    offset: int
    path: int = 0
    data: bytes = b""
    address: int = 0
    value: int = 0
    qwords: int = 0
    frame: int = 0


@dataclass
class Frame:
    number: int
    records: list[Record] = field(default_factory=list)


@dataclass
class CaptureSummary:
    total_bytes: int
    record_counts: dict[str, int]
    gif_bytes_per_path: list[int]
    frame_numbers: list[int]
    open_records_after_last_frame: int
    issues: list[str]

    @property
    def valid(self) -> bool:
        return not self.issues


def parse_records(data: bytes) -> list[Record]:
    """Parse every record. Raises CaptureError on an unknown tag, truncation or a bad path."""
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("capture data must be bytes-like")
    data = bytes(data)
    records: list[Record] = []
    position = 0
    while position < len(data):
        offset = position
        tag = data[position]
        position += 1
        if tag == TAG_GIF:
            header = _take(data, position, 5, offset, "G header")
            path = header[0]
            (size,) = struct.unpack_from("<I", header, 1)
            position += 5
            if path >= GIF_PATH_COUNT:
                raise CaptureError(f"offset {offset}: GIF path {path} out of range 0..{GIF_PATH_COUNT - 1}")
            if size > MAX_GIF_BYTES:
                raise CaptureError(f"offset {offset}: GIF packet of {size} bytes exceeds {MAX_GIF_BYTES}")
            payload = _take(data, position, size, offset, "G payload")
            position += size
            records.append(Record("G", offset, path=path, data=payload))
        elif tag == TAG_PRIVILEGED:
            body = _take(data, position, 12, offset, "P body")
            address, value = struct.unpack("<IQ", body)
            position += 12
            records.append(Record("P", offset, address=address, value=value))
        elif tag == TAG_FIFO_READ:
            body = _take(data, position, 4, offset, "R body")
            (qwords,) = struct.unpack("<I", body)
            position += 4
            records.append(Record("R", offset, qwords=qwords))
        elif tag == TAG_FRAME_END:
            body = _take(data, position, 8, offset, "V body")
            (frame,) = struct.unpack("<Q", body)
            position += 8
            records.append(Record("V", offset, frame=frame))
        else:
            raise CaptureError(f"offset {offset}: unknown record tag 0x{tag:02X}")
    return records


def _take(data: bytes, position: int, count: int, offset: int, what: str) -> bytes:
    if position + count > len(data):
        raise CaptureError(f"offset {offset}: truncated {what} (need {count} bytes, "
                           f"{len(data) - position} left)")
    return data[position:position + count]


def split_frames(records: list[Record]) -> tuple[list[Frame], list[Record]]:
    """Group records into frames at each 'V' record.

    Returns the completed frames and the trailing records that never reached a
    'V' (an unfinished frame, usually from a capture stopped mid-frame).
    """
    frames: list[Frame] = []
    pending: list[Record] = []
    for record in records:
        if record.tag == "V":
            frames.append(Frame(number=record.frame, records=pending))
            pending = []
        else:
            pending.append(record)
    return frames, pending


def summarize(data: bytes) -> CaptureSummary:
    """Parse and validate a capture. Structural errors raise CaptureError; semantic issues are listed."""
    records = parse_records(data)
    frames, trailing = split_frames(records)
    counts = {name: 0 for name in TAG_NAMES.values()}
    gif_bytes = [0] * GIF_PATH_COUNT
    for record in records:
        counts[record.tag] += 1
        if record.tag == "G":
            gif_bytes[record.path] += len(record.data)
    issues: list[str] = []
    numbers = [frame.number for frame in frames]
    for previous, current in zip(numbers, numbers[1:]):
        if current <= previous:
            issues.append(f"frame numbers not strictly increasing: {previous} then {current}")
            break
    if not frames:
        issues.append("capture contains no complete frame ('V' record)")
    return CaptureSummary(
        total_bytes=len(data),
        record_counts=counts,
        gif_bytes_per_path=gif_bytes,
        frame_numbers=numbers,
        open_records_after_last_frame=len(trailing),
        issues=issues,
    )


def main(argv: list[str] | None = None) -> int:
    import argparse
    import json
    import sys
    from pathlib import Path

    parser = argparse.ArgumentParser(description="Validate a GT4_PGS_CAPTURE GS stream capture.")
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", action="store_true", help="print the summary as JSON")
    args = parser.parse_args(argv)
    try:
        summary = summarize(args.capture.read_bytes())
    except CaptureError as error:
        print(f"invalid capture: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({
            "total_bytes": summary.total_bytes,
            "record_counts": summary.record_counts,
            "gif_bytes_per_path": summary.gif_bytes_per_path,
            "frame_count": len(summary.frame_numbers),
            "first_frame": summary.frame_numbers[0] if summary.frame_numbers else None,
            "last_frame": summary.frame_numbers[-1] if summary.frame_numbers else None,
            "open_records_after_last_frame": summary.open_records_after_last_frame,
            "issues": summary.issues,
            "valid": summary.valid,
        }, indent=2))
    else:
        print(f"bytes {summary.total_bytes}, frames {len(summary.frame_numbers)}, "
              f"records {summary.record_counts}")
        for issue in summary.issues:
            print(f"issue: {issue}")
    return 0 if summary.valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
