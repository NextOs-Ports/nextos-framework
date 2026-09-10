# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- real kernel pads, created through uinput.

WHAT THIS IS, AND WHAT IT IS NOT

These are REAL evdev devices. The kernel creates them, assigns them a
/dev/input/eventN node, answers EVIOCGBIT and EVIOCGABS for them and delivers
their events through the same path a physical pad uses. SDL cannot tell them
apart from hardware, which is exactly why they can prove what SDL does.

They are still NOT physical proof. A uinput pad shares the host's kernel and
input stack, not the device's; it says nothing about a CFW, a board or a
driver. C5B drew that line for Godot and C6 keeps it: every uinput result is
host evidence and the physical claim stays PENDING_PHYSICAL.
"""

import ctypes, fcntl, os, struct, time

# linux/input-event-codes.h
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BUS_USB, BUS_HOST = 0x03, 0x19
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11

BTN_A, BTN_B, BTN_X, BTN_Y = 0x130, 0x131, 0x133, 0x134
BTN_TL, BTN_TR, BTN_TL2, BTN_TR2 = 0x136, 0x137, 0x138, 0x139
BTN_SELECT, BTN_START, BTN_MODE = 0x13a, 0x13b, 0x13c
BTN_THUMBL, BTN_THUMBR = 0x13d, 0x13e

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502

# The full gamepad this suite uses. Ordered as the kernel reports it, which
# is what both SDL majors then enumerate over.
KEYS = [BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR]
AXES = [ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y]

# Ranges the kernel will report through EVIOCGABS. The sticks are signed and
# the triggers are 0..255, which is what makes a trigger's "half range"
# question real rather than decorative.
RANGES = {
    ABS_X: (-32768, 32767), ABS_Y: (-32768, 32767),
    ABS_RX: (-32768, 32767), ABS_RY: (-32768, 32767),
    ABS_Z: (0, 255), ABS_RZ: (0, 255),
    ABS_HAT0X: (-1, 1), ABS_HAT0Y: (-1, 1),
}


class Pad:
    """One real uinput gamepad."""

    def __init__(self, name, vendor=0x0912, product=0xc5a1, version=0x0110,
                 bustype=BUS_USB, keys=None, axes=None, ranges=None):
        self.name = name
        self.keys = list(KEYS if keys is None else keys)
        self.axes = list(AXES if axes is None else axes)
        self.ranges = dict(RANGES if ranges is None else ranges)
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_SYN)
        for k in self.keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        for a in self.axes:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, a)

        # struct uinput_user_dev: char name[80]; input_id{bustype,vendor,
        # product,version}; ff_effects_max; absmax/min/fuzz/flat[64]
        absmax = [0] * 64
        absmin = [0] * 64
        for code, (lo, hi) in self.ranges.items():
            absmin[code], absmax[code] = lo, hi
        payload = self.name.encode()[:79].ljust(80, b"\0")
        payload += struct.pack("HHHH", bustype, vendor, product, version)
        payload += struct.pack("i", 0)
        payload += struct.pack("64i", *absmax)
        payload += struct.pack("64i", *absmin)
        payload += struct.pack("64i", *([0] * 64))   # fuzz
        payload += struct.pack("64i", *([0] * 64))   # flat
        os.write(self.fd, payload)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(0.35)

    def _emit(self, etype, code, value):
        # struct input_event on 64-bit: timeval{s,us}, type, code, value
        os.write(self.fd, struct.pack("llHHi", 0, 0, etype, code, value))

    def key(self, code, down):
        self._emit(EV_KEY, code, 1 if down else 0)
        self._emit(EV_SYN, SYN_REPORT, 0)

    def abs(self, code, value):
        self._emit(EV_ABS, code, value)
        self._emit(EV_SYN, SYN_REPORT, 0)

    def tap(self, code, hold=0.12):
        self.key(code, True)
        time.sleep(hold)
        self.key(code, False)
        time.sleep(0.06)

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(self.fd)
        time.sleep(0.3)
