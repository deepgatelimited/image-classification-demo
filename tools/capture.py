#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "numpy>=1.26", "pillow>=10"]
# ///
"""Receive frames from the `capture` firmware and save them as PNGs.

Usage:
    tools/capture.py --out tasks/helmet/data/raw/session-1 [--max 200]

Start this, then press the board's blue button to start / stop a burst; Ctrl-C
ends the session. Frames are saved losslessly, with the same RGB565 -> RGB888
widening the firmware uses, so the PNGs hold the pixels the model will see.
File names are unique across sessions: <prefix>-<session start>-<seq>.png.
"""

import argparse
import datetime
import signal
import sys
from pathlib import Path

from PIL import Image

from capture_link import BAUD, CrcError, Frame, Link, LinkError, find_port


def main() -> None:
    # A plain kill (SIGTERM) ends the session like Ctrl-C, so the board still gets STOP.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path, help="folder to save PNGs into (created if missing)")
    ap.add_argument("--prefix", default="img", help="file name prefix (default: img)")
    ap.add_argument("--max", type=int, default=0, help="stop after this many images (default: no limit)")
    ap.add_argument("--port", help="serial port (default: the first /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=BAUD, help="must match CAPTURE_BAUD in the firmware")
    args = ap.parse_args()

    out = args.out.expanduser()
    out.mkdir(parents=True, exist_ok=True)
    session = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    try:
        port = args.port or find_port()
        link = Link(port, args.baud)
    except LinkError as e:
        if not any(out.iterdir()):
            out.rmdir()
        sys.exit(str(e))
    print(f"listening on {port} at {args.baud} baud; saving to {out}/")
    print("press the board's blue button to start / stop; Ctrl-C to finish")

    saved = bad = 0
    try:
        while not args.max or saved < args.max:
            for event in link.poll():
                if isinstance(event, str):
                    print(f"  [board] {event}")
                elif isinstance(event, CrcError):
                    bad += 1
                    print(f"  frame {event.seq}: crc mismatch, dropped ({bad} so far)")
                elif isinstance(event, Frame) and (not args.max or saved < args.max):
                    name = f"{args.prefix}-{session}-{saved:04d}.png"
                    Image.fromarray(event.rgb(), "RGB").save(out / name)
                    saved += 1
                    print(f"  saved {name}  ({saved} this session)")
    except KeyboardInterrupt:
        pass
    finally:
        # A second Ctrl-C (or a kill that reaches both uv and Python) must not cut
        # the cleanup short: the board needs its STOP.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        link.close()

    if not any(out.iterdir()):
        out.rmdir()  # nothing captured: don't leave an empty session folder behind
        print(f"\nno images saved; removed the empty {out}/")
        return
    total_in_folder = len(list(out.glob("*.png")))
    print(f"\n{saved} images saved this session ({bad} dropped); {total_in_folder} PNGs in {out}/")


if __name__ == "__main__":
    main()
