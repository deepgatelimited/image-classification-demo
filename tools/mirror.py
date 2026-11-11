#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "numpy>=1.26", "pillow>=10", "av>=12"]
# ///
"""The board's screen on the laptop, live in a browser and optionally recorded.
The board must run `classify`; its status lines print here too.

Usage:
    tools/mirror.py                                  # opens http://127.0.0.1:8766
    tools/mirror.py --record take1.mp4 --no-browser  # just record, until Ctrl-C

--record writes H.264 at 30 fps and 2x the LCD's size (1600x960), timed by the
board's clock. Wire format: firmware/common/inc/mirror.h.
"""

import argparse
import io
import queue
import signal
import struct
import sys
import threading
import time
import webbrowser
import zlib
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import av
import numpy as np
import serial
from PIL import Image

from capture_link import LinkError, find_port, port_holders

BAUD = 12_500_000  # MIRROR_BAUD in firmware/common/inc/mirror.h
MAGIC = b"DGMF"
# magic, seq, screen_w, screen_h, image_w, format, palette_n, rle_bytes, payload_bytes, flags, tick_ms
HEADER = struct.Struct("<4sIHHHBBIIII")
FORMAT_YCBCR420 = 1
FLAG_NO_PANEL = 1
RLE_MAX = 48 * 1024  # MIRROR_RLE_MAX: the most panel runs the board takes
HEARTBEAT_EVERY_S = 0.5
MAX_PAYLOAD = 1 << 20

PAGE = b"""<!doctype html>
<meta charset="utf-8"><title>Board screen</title>
<style>
  html, body { margin: 0; height: 100%; background: #000; }
  img { width: 100%; height: 100%; object-fit: contain; image-rendering: pixelated; }
</style>
<img src="/stream" alt="">
"""


@dataclass
class MirrorFrame:
    seq: int
    screen_w: int
    screen_h: int
    image_w: int
    palette_n: int
    rle_bytes: int
    flags: int
    tick_ms: int
    payload: bytes


@dataclass
class CrcError:
    seq: int


def _sizes(screen_w: int, screen_h: int, image_w: int) -> tuple[int, int]:
    """Bytes of the Y plane and of each chroma plane."""
    y = (image_w // 2) * (screen_h // 2)
    return y, y // 4


class Parser:
    """Splits the byte stream into frames and text lines, like capture_link.Parser."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list:
        self.buf += data
        buf, events = self.buf, []
        while True:
            start = buf.find(MAGIC)
            text_end = start if start >= 0 else len(buf)
            while (nl := buf.find(b"\n", 0, text_end)) >= 0:
                raw = bytes(buf[:nl]).strip()
                # The board prints plain ASCII; anything else is the tail of a frame
                # that was in flight when the port opened.
                if raw and all(32 <= b < 127 or b == 9 for b in raw):
                    events.append(raw.decode("ascii"))
                del buf[: nl + 1]
                text_end -= nl + 1
            if start < 0:
                if len(buf) > 4096:
                    del buf[: len(buf) - 3]
                return events
            del buf[:text_end]
            if len(buf) < HEADER.size:
                return events
            _, seq, sw, sh, iw, fmt, pal_n, rle, payload_n, flags, tick = HEADER.unpack_from(buf)
            y, c = _sizes(sw, sh, iw)
            sane = (fmt == FORMAT_YCBCR420 and 0 < sw <= 2048 and 0 < sh <= 2048 and sh % 4 == 0
                    and 0 < iw < sw and iw % 4 == 0 and rle % 2 == 0
                    and payload_n == y + 2 * c + rle + 4 * pal_n and payload_n <= MAX_PAYLOAD)
            if not sane:
                del buf[:1]  # not a real header: resync on the next magic
                continue
            total = HEADER.size + payload_n + 4
            if len(buf) < total:
                return events
            payload = bytes(buf[HEADER.size : HEADER.size + payload_n])
            (crc,) = struct.unpack_from("<I", buf, HEADER.size + payload_n)
            del buf[:total]
            if zlib.crc32(payload) == crc:
                events.append(MirrorFrame(seq, sw, sh, iw, pal_n, rle, flags, tick, payload))
            else:
                events.append(CrcError(seq))


def ycbcr420_to_rgb(y: np.ndarray, cb: np.ndarray, cr: np.ndarray) -> np.ndarray:
    """JFIF full-range YCbCr (chroma at half resolution) -> RGB, as uint8."""
    cb = np.repeat(np.repeat(cb, 2, axis=0), 2, axis=1).astype(np.float32) - 128
    cr = np.repeat(np.repeat(cr, 2, axis=0), 2, axis=1).astype(np.float32) - 128
    y = y.astype(np.float32)
    rgb = np.stack([y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb], axis=-1)
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


def decode(frame: MirrorFrame, last_panel: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """The whole screen as an H x W x 3 uint8 array, and its panel (to pass back in
    as last_panel, for frames that leave the panel out)."""
    iw, ih = frame.image_w // 2, frame.screen_h // 2
    ny, nc = _sizes(frame.screen_w, frame.screen_h, frame.image_w)
    data = np.frombuffer(frame.payload, np.uint8)
    y = data[:ny].reshape(ih, iw)
    cb = data[ny : ny + nc].reshape(ih // 2, iw // 2)
    cr = data[ny + nc : ny + 2 * nc].reshape(ih // 2, iw // 2)
    image = ycbcr420_to_rgb(y, cb, cr)
    image = np.repeat(np.repeat(image, 2, axis=0), 2, axis=1)

    panel_w = frame.screen_w - frame.image_w
    panel_shape = (frame.screen_h, panel_w, 3)
    if frame.flags & FLAG_NO_PANEL or frame.palette_n == 0:
        panel = last_panel if last_panel is not None and last_panel.shape == panel_shape else np.zeros(panel_shape, np.uint8)
    else:
        start = ny + 2 * nc
        pairs = data[start : start + frame.rle_bytes].reshape(-1, 2)
        argb = data[start + frame.rle_bytes :].view("<u4")
        colours = np.stack([(argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF], axis=-1).astype(np.uint8)
        index, runs = pairs[:, 0], pairs[:, 1].astype(np.int64)
        if runs.sum() != frame.screen_h * panel_w or index.max(initial=0) >= frame.palette_n:
            raise ValueError(f"frame {frame.seq}: panel runs don't fill the panel")
        panel = colours[np.repeat(index, runs)].reshape(panel_shape)
    return np.concatenate([image, panel], axis=1), panel


class Recorder:
    """Screens -> an H.264 file at a constant FPS, in a thread of its own so the
    encoder never holds up the serial link. A screen shows from its board time
    until the next one's."""

    FPS = 30
    SCALE = 2

    def __init__(self, path: Path) -> None:
        self.path = path
        # The muxer opens the file only when the first frame is written, so a bad
        # path would fail mid-take, in the thread: make the folder and file now.
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch(exist_ok=False)
        self.container = av.open(str(path), "w")
        self.stream = None
        self.queue: queue.Queue = queue.Queue(maxsize=64)
        self.written = 0
        self.dropped = 0
        self.error: Exception | None = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def add(self, screen: np.ndarray, board_ms: int) -> None:
        if self.error or not self.thread.is_alive():
            return
        try:
            self.queue.put_nowait((screen, board_ms))
        except queue.Full:
            if not self.dropped:
                print("mirror: the recording is falling behind; dropping screens (the take will stutter)")
            self.dropped += 1

    def close(self) -> None:
        """Finish the file. Never blocks for long, even if the thread has died."""
        if self.thread.is_alive():
            try:
                self.queue.put(None, timeout=5)
            except queue.Full:
                pass
            self.thread.join(timeout=30)

    def _write(self, screen: np.ndarray, count: int) -> None:
        big = np.repeat(np.repeat(screen, self.SCALE, axis=0), self.SCALE, axis=1)
        for _ in range(count):
            for packet in self.stream.encode(av.VideoFrame.from_ndarray(big, format="rgb24")):
                self.container.mux(packet)
        self.written += count

    def _run(self) -> None:
        try:
            self._encode()
        except Exception as e:  # say so once; the live view carries on
            self.error = e
            print(f"mirror: recording failed: {e}")

    def _encode(self) -> None:
        last = start = prev_ms = None
        while (item := self.queue.get()) is not None:
            screen, board_ms = item
            if self.stream is None:
                self.container.metadata["creation_time"] = datetime.now(timezone.utc).isoformat()
                self.stream = self.container.add_stream("libx264", rate=self.FPS)
                self.stream.width, self.stream.height = screen.shape[1] * self.SCALE, screen.shape[0] * self.SCALE
                self.stream.pix_fmt = "yuv420p"
                self.stream.options = {"crf": "14", "preset": "veryfast"}
                start = board_ms
            elif board_ms < prev_ms:  # the board restarted: carry on from here
                start = board_ms - self.written * 1000 / self.FPS
            # Output frame k shows the screen current at start + k / FPS.
            due = int(-(-(board_ms - start) * self.FPS // 1000))  # ceil
            if last is not None and due > self.written:
                self._write(last, due - self.written)
            last, prev_ms = screen, board_ms
        if last is None:  # no screen ever came: no file
            self.container.close()
            self.path.unlink(missing_ok=True)
            return
        self._write(last, 1)
        for packet in self.stream.encode():
            self.container.mux(packet)
        self.container.close()


class Mirror:
    """Holds the port, sends heartbeats, keeps the latest screen as a JPEG."""

    def __init__(self, port: str, baud: int, record: Path | None) -> None:
        try:
            self.serial = serial.Serial(port, baud, timeout=0.1, exclusive=True)
        except serial.SerialException as e:
            holders = port_holders(port)
            if holders:
                raise LinkError(f"{port} is in use by {holders}; only one program can hold the board's port") from e
            raise LinkError(f"can't open {port}: {e}") from e
        self.serial.reset_input_buffer()
        self.parser = Parser()
        self.recorder = Recorder(record) if record else None
        self.cond = threading.Condition()
        self.jpeg: bytes | None = None
        self.number = 0  # frames shown so far
        self.panel = None
        self.closing = False

    def run(self) -> None:
        last_beat = last_report = time.monotonic()
        shown = crc_errors = 0
        while not self.closing:
            now = time.monotonic()
            if now - last_beat >= HEARTBEAT_EVERY_S:
                self.serial.write(b"H")
                last_beat = now
            for event in self.parser.feed(self.serial.read(self.serial.in_waiting or 1)):
                if isinstance(event, str):
                    print(event)
                elif isinstance(event, CrcError):
                    crc_errors += 1
                else:
                    self.show(event)
                    shown += 1
            if now - last_report >= 5:
                if shown == 0:
                    print("mirror: no frames: is `classify` flashed and running? (./deploy.sh)")
                elif crc_errors:
                    print(f"mirror: {shown / (now - last_report):.1f} fps, {crc_errors} corrupt frames dropped")
                last_report, shown, crc_errors = now, 0, 0

    def show(self, frame: MirrorFrame) -> None:
        try:
            screen, self.panel = decode(frame, self.panel)
        except ValueError as e:
            print(f"mirror: {e}")
            return
        image = Image.fromarray(screen, "RGB")
        out = io.BytesIO()
        image.save(out, "JPEG", quality=92)
        with self.cond:
            self.jpeg = out.getvalue()
            self.number += 1
            self.cond.notify_all()
        if self.recorder:
            self.recorder.add(screen, frame.tick_ms)

    def wait_frame(self, after: int, timeout: float = 2.0) -> tuple[int, bytes | None]:
        with self.cond:
            self.cond.wait_for(lambda: self.number > after or self.closing, timeout)
            return self.number, self.jpeg

    def close(self) -> None:
        self.closing = True
        with self.cond:
            self.cond.notify_all()
        self.serial.close()  # no more heartbeats: the board stops sending within 1.5 s
        if self.recorder:
            self.recorder.close()
            seconds = self.recorder.written / Recorder.FPS
            if self.recorder.dropped:
                print(f"mirror: {self.recorder.dropped} screens were dropped from the recording")
            if self.recorder.error:
                print(f"recording failed ({self.recorder.error}): {self.recorder.path} is incomplete")
            elif not seconds:
                print(f"no screens arrived: {self.recorder.path} not saved")
            else:
                print(f"saved {self.recorder.path} ({int(seconds // 60)}:{seconds % 60:04.1f}, "
                      f"{self.recorder.path.stat().st_size / 1e6:.0f} MB)")


def make_handler(mirror: Mirror):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def do_GET(self) -> None:
            if self.path == "/":
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(PAGE)))
                self.end_headers()
                self.wfile.write(PAGE)
            elif self.path == "/stream":
                # MJPEG: one JPEG part per new screen, for as long as the page is open.
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                seen = 0
                try:
                    while not mirror.closing:
                        seen, jpeg = mirror.wait_frame(seen)
                        if jpeg is None:
                            continue
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                         + str(len(jpeg)).encode() + b"\r\n\r\n" + jpeg + b"\r\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
            else:
                self.send_error(404)

    return Handler


def main() -> None:
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: the first /dev/cu.usbmodem*)")
    ap.add_argument("--baud", type=int, default=BAUD, help="must match MIRROR_BAUD in the firmware")
    ap.add_argument("--http-port", type=int, default=8766, help="the page's port on 127.0.0.1 (default: 8766)")
    ap.add_argument("--record", type=Path, metavar="FILE.mp4", help="also record the screen to this video")
    ap.add_argument("--no-browser", action="store_true", help="don't open the page")
    args = ap.parse_args()

    if args.record and args.record.exists():
        raise SystemExit(f"{args.record} exists: pick another name (a take is never overwritten)")
    if args.record and args.record.suffix.lower() not in (".mp4", ".mov"):
        raise SystemExit("--record takes a .mp4 or .mov file name")
    try:
        mirror = Mirror(args.port or find_port(), args.baud, args.record)
    except LinkError as e:
        raise SystemExit(str(e))
    except OSError as e:
        raise SystemExit(f"can't record to {args.record}: {e.strerror}")
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.http_port), make_handler(mirror))
    except OSError as e:
        mirror.close()  # lets the port go and removes the still-empty video
        raise SystemExit(f"can't serve on port {args.http_port} ({e.strerror}): is mirror.py already running? "
                         "(--http-port picks another)")
    server.daemon_threads = True
    link_thread = threading.Thread(target=mirror.run, daemon=True)
    link_thread.start()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.http_port}/"
    print(f"open {url}   (Ctrl-C to quit)")
    if args.record:
        print(f"recording to {args.record}: Ctrl-C ends the take")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        while link_thread.is_alive():
            link_thread.join(1)
    except KeyboardInterrupt:
        pass
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)  # a second Ctrl-C must not cut the video short
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        mirror.closing = True
        link_thread.join(timeout=2)
        server.shutdown()
        mirror.close()


if __name__ == "__main__":
    main()
