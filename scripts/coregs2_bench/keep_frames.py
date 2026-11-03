"""Deletes the frames a run dumps outside the wanted windows, while it runs, so dumping every frame of a long replay fits on disk.

usage: keep_frames.py <frames dir> <stop file> A-B [C-D ...]
Runs until the stop file exists, then does one last pass. A frame is any file whose name ends in digits before its extension.
"""
import re
import sys
import time
from pathlib import Path

NUMBER = re.compile(r'(\d+)\.[a-z]+$')


def in_windows(number, windows):
    return any(first <= number <= last for first, last in windows)


def clean(frames, windows):
    for path in frames.iterdir():
        match = NUMBER.search(path.name)
        if match and not in_windows(int(match.group(1)), windows):
            try:
                path.unlink()
            except OSError:
                pass  # still being written: the next pass takes it


def main():
    frames, stop = Path(sys.argv[1]), Path(sys.argv[2])
    windows = [tuple(int(part) for part in window.split('-')) for window in sys.argv[3:]]
    while not stop.exists():
        clean(frames, windows)
        time.sleep(1.0)
    time.sleep(1.0)
    clean(frames, windows)


if __name__ == '__main__':
    main()
