#!/usr/bin/env python3
"""Controllable uinput gamepads for unattended PartyBoard device tests.

Run this as root on NextOS, then write commands such as ``tap a`` or
``axis lx -32768`` to /tmp/partyboard-pad1 (and pad2..pad4).  This is test
infrastructure only; it is not installed by the release package.
"""

import argparse
import fcntl
import os
import selectors
import signal
import struct
import time


EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502

BUTTONS = {
    "a": 0x130,
    "b": 0x131,
    "x": 0x133,
    "y": 0x134,
    "lb": 0x136,
    "rb": 0x137,
    "select": 0x13A,
    "back": 0x13A,
    "start": 0x13B,
    "guide": 0x13C,
    "ls": 0x13D,
    "rs": 0x13E,
}

AXES = {
    "lx": 0,
    "ly": 1,
    "lt": 2,
    "rx": 3,
    "ry": 4,
    "rt": 5,
    "hatx": 16,
    "haty": 17,
}


class VirtualGamepad:
    def __init__(self, number: int, fifo_prefix: str):
        self.number = number
        self.device_fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.device_fd, UI_SET_EVBIT, EV_KEY)
        for code in BUTTONS.values():
            fcntl.ioctl(self.device_fd, UI_SET_KEYBIT, code)
        fcntl.ioctl(self.device_fd, UI_SET_EVBIT, EV_ABS)
        for code in AXES.values():
            fcntl.ioctl(self.device_fd, UI_SET_ABSBIT, code)

        absmax = [0] * 64
        absmin = [0] * 64
        absfuzz = [0] * 64
        absflat = [0] * 64
        for code in (0, 1, 3, 4):
            absmin[code] = -32768
            absmax[code] = 32767
            absflat[code] = 4096
        for code in (2, 5):
            absmin[code] = 0
            absmax[code] = 255
        for code in (16, 17):
            absmin[code] = -1
            absmax[code] = 1

        name = f"PartyBoard Virtual Pad {number}".encode()
        name = name[:79] + b"\0" * (80 - len(name[:79]))
        descriptor = struct.pack("80sHHHHi", name, 0x03, 0x045E, 0x028E, 0x0110, 0)
        descriptor += struct.pack("64i", *absmax)
        descriptor += struct.pack("64i", *absmin)
        descriptor += struct.pack("64i", *absfuzz)
        descriptor += struct.pack("64i", *absflat)
        os.write(self.device_fd, descriptor)
        fcntl.ioctl(self.device_fd, UI_DEV_CREATE)

        self.fifo_path = f"{fifo_prefix}{number}"
        try:
            os.unlink(self.fifo_path)
        except FileNotFoundError:
            pass
        os.mkfifo(self.fifo_path, 0o666)
        self.control_fd = os.open(self.fifo_path, os.O_RDWR | os.O_NONBLOCK)
        self.pending = b""
        self.center()

    def emit(self, event_type: int, code: int, value: int):
        os.write(self.device_fd, struct.pack("llHHi", 0, 0, event_type, code, value))

    def sync(self):
        self.emit(EV_SYN, SYN_REPORT, 0)

    def button(self, name: str, value: int):
        self.emit(EV_KEY, BUTTONS[name], value)
        self.sync()

    def tap(self, name: str, duration: float = 0.12):
        self.button(name, 1)
        time.sleep(duration)
        self.button(name, 0)

    def axis(self, name: str, value: int):
        code = AXES[name]
        if name in ("lt", "rt"):
            value = max(0, min(255, value))
        elif name in ("hatx", "haty"):
            value = max(-1, min(1, value))
        else:
            value = max(-32768, min(32767, value))
        self.emit(EV_ABS, code, value)
        self.sync()

    def center(self):
        for name in ("lx", "ly", "rx", "ry", "lt", "rt", "hatx", "haty"):
            self.axis(name, 0)

    def process(self, line: str) -> bool:
        fields = line.strip().lower().split()
        if not fields:
            return True
        command = fields[0]
        if command == "tap" and len(fields) >= 2:
            self.tap(fields[1], float(fields[2]) if len(fields) >= 3 else 0.12)
        elif command in ("down", "up") and len(fields) == 2:
            self.button(fields[1], 1 if command == "down" else 0)
        elif command == "axis" and len(fields) == 3:
            self.axis(fields[1], int(fields[2], 0))
        elif command == "center":
            self.center()
        elif command == "quit":
            return False
        else:
            print(f"pad{self.number}: ignored command {line!r}", flush=True)
        return True

    def close(self):
        try:
            fcntl.ioctl(self.device_fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(self.control_fd)
        os.close(self.device_fd)
        try:
            os.unlink(self.fifo_path)
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pads", type=int, default=1, choices=range(1, 5))
    parser.add_argument("--fifo-prefix", default="/tmp/partyboard-pad")
    args = parser.parse_args()

    running = True

    def stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    pads = [VirtualGamepad(number, args.fifo_prefix) for number in range(1, args.pads + 1)]
    selector = selectors.DefaultSelector()
    for pad in pads:
        selector.register(pad.control_fd, selectors.EVENT_READ, pad)
    print(
        f"ready: {len(pads)} virtual pad(s), controls {args.fifo_prefix}1..{args.fifo_prefix}{len(pads)}",
        flush=True,
    )
    time.sleep(1.0)

    try:
        while running:
            for key, _mask in selector.select(timeout=0.5):
                pad = key.data
                try:
                    chunk = os.read(pad.control_fd, 4096)
                except BlockingIOError:
                    continue
                pad.pending += chunk
                while b"\n" in pad.pending:
                    raw, pad.pending = pad.pending.split(b"\n", 1)
                    if not pad.process(raw.decode(errors="replace")):
                        running = False
                        break
    finally:
        selector.close()
        for pad in pads:
            pad.close()


if __name__ == "__main__":
    main()
