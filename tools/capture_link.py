"""The laptop's end of the `capture` firmware's serial link (wire format in
firmware/apps/capture/main.c). Board -> laptop: frames and status lines.
Laptop -> board: 'H' (heartbeat, sent from poll()), 'G' (start), 'S' (stop).

    link = Link(find_port())
    while True:
        for event in link.poll():   # Frame, CrcError or a str (a text line)
            ...
    link.close()                     # sends STOP: the board is left paused
"""

import glob
import struct
import subprocess
import time
import zlib
from dataclasses import dataclass

import numpy as np
import serial

BAUD = 2_000_000  # CAPTURE_BAUD in the firmware
MAGIC = b"DGCF"
HEADER = struct.Struct("<4sIHHB3x")  # magic, seq, width, height, format
FORMAT_RGB565 = 1
HEARTBEAT, GO, STOP = b"H", b"G", b"S"
HEARTBEAT_EVERY_S = 0.5


class LinkError(Exception):
    """No board, or the port can't be opened; the message says what to do."""


@dataclass
class Frame:
    seq: int
    width: int
    height: int
    pixels: bytes  # raw RGB565, little-endian, row-major from the top left

    def rgb(self) -> np.ndarray:
        return rgb565_to_rgb888(self.pixels, self.width, self.height)


@dataclass
class CrcError:
    seq: int


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise LinkError("no /dev/cu.usbmodem*: is the board's ST-LINK USB (CN2) plugged in?")
    return ports[0]


def rgb565_to_rgb888(raw: bytes, width: int, height: int) -> np.ndarray:
    """Widen to 8 bits per channel exactly as the firmware does for the LCD (bit
    replication, so 31 -> 255), so saved images hold the pixels the device sees."""
    p = np.frombuffer(raw, dtype="<u2").reshape(height, width).astype(np.uint32)
    r, g, b = (p >> 11) & 0x1F, (p >> 5) & 0x3F, p & 0x1F
    rgb = np.stack([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2], axis=-1)
    return rgb.astype(np.uint8)


class Parser:
    """Splits the byte stream into frames and text lines. Feed it whatever arrived;
    it keeps partial frames and lines until the rest comes."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list:
        self.buf += data
        buf, events = self.buf, []
        while True:
            start = buf.find(MAGIC)
            # Text before a frame (or with no frame in sight): whole lines only.
            text_end = start if start >= 0 else len(buf)
            while (nl := buf.find(b"\n", 0, text_end)) >= 0:
                line = buf[:nl].decode("ascii", "replace").strip()
                if line:
                    events.append(line)
                del buf[: nl + 1]
                text_end -= nl + 1
            if start < 0:
                # Keep a possible partial magic at the end; drop older junk.
                if len(buf) > 4096:
                    del buf[: len(buf) - 3]
                return events
            del buf[:text_end]  # now buf starts with the magic

            if len(buf) < HEADER.size:
                return events
            _, seq, width, height, fmt = HEADER.unpack_from(buf)
            if fmt != FORMAT_RGB565 or not (0 < width <= 2048 and 0 < height <= 2048):
                del buf[:1]  # not a real header: resync on the next magic
                continue
            n_pixels = width * height * 2
            total = HEADER.size + n_pixels + 4
            if len(buf) < total:
                return events
            pixels = bytes(buf[HEADER.size : HEADER.size + n_pixels])
            (crc,) = struct.unpack_from("<I", buf, HEADER.size + n_pixels)
            del buf[:total]
            events.append(Frame(seq, width, height, pixels) if zlib.crc32(pixels) == crc else CrcError(seq))


def port_holders(port: str) -> str:
    """Who else has the port open, e.g. "Python (pid 4242)", from lsof."""
    try:
        out = subprocess.run(["lsof", "-F", "pc", port], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
    holders, pid = [], None
    for line in out.splitlines():
        if line.startswith("p"):
            pid = line[1:]
        elif line.startswith("c") and pid:
            holders.append(f"{line[1:]} (pid {pid})")
    return ", ".join(holders)


class Link:
    def __init__(self, port: str, baud: int = BAUD) -> None:
        self.port = port
        try:
            # exclusive: a second program opening the port fails instead of
            # silently sharing it (two readers each get half the bytes).
            self.serial = serial.Serial(port, baud, timeout=0.1, exclusive=True)
        except serial.SerialException as e:
            holders = port_holders(port)
            if holders:
                raise LinkError(f"{port} is in use by {holders}; only one program can hold the board's port") from e
            raise LinkError(f"can't open {port}: {e}") from e
        self.serial.reset_input_buffer()
        self.parser = Parser()
        self.last_beat = 0.0
        self.stop()  # start paused, whatever state the board was left in

    def poll(self) -> list:
        """Send a heartbeat if one is due, then return what arrived (waits up to 0.1 s)."""
        if time.monotonic() - self.last_beat >= HEARTBEAT_EVERY_S:
            self.serial.write(HEARTBEAT)
            self.last_beat = time.monotonic()
        return self.parser.feed(self.serial.read(self.serial.in_waiting or 1))

    def go(self) -> None:
        self.serial.write(GO)

    def stop(self) -> None:
        self.serial.write(STOP)

    def close(self) -> None:
        """Leave the board paused and let the port go."""
        try:
            self.stop()
            self.serial.flush()
        except (OSError, serial.SerialException):
            pass
        self.serial.close()
