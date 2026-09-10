#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Virtual gamepad over /dev/uinput, for driving a REAL Godot process.

It is an input DEVICE, not a proof: everything it produces is host evidence.
The capability set is declared explicitly so the engine's own enumeration --
not our model -- decides the ordinals.
"""
import array, ctypes, fcntl, glob, os, struct, time

UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BUS_USB = 0x03


def _UI_GET_SYSNAME(length):
    return (2 << 30) | (ord("U") << 8) | 44 | (length << 16)


def _node_of(fd):
    """The /dev/input/eventN the kernel created for THIS uinput device.

    Asking the kernel which node it made is the only way to be sure: two
    pads in flight, or a node whose removal has not landed yet, would
    otherwise be mistaken for each other.
    """
    buf = array.array("B", [0] * 64)
    try:
        fcntl.ioctl(fd, _UI_GET_SYSNAME(64), buf, True)
    except OSError:
        return None
    sysname = bytes(buf).split(b"\0")[0].decode()
    for _ in range(40):
        for candidate in glob.glob("/dev/input/event*"):
            link = "/sys/class/input/%s/device" % os.path.basename(candidate)
            if os.path.realpath(link).endswith(sysname):
                return candidate
        time.sleep(0.1)
    return None


class UinputUserDev(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 80),
                ("bustype", ctypes.c_uint16), ("vendor", ctypes.c_uint16),
                ("product", ctypes.c_uint16), ("version", ctypes.c_uint16),
                ("ff_effects_max", ctypes.c_uint32),
                ("absmax", ctypes.c_int32 * 64),
                ("absmin", ctypes.c_int32 * 64),
                ("absfuzz", ctypes.c_int32 * 64),
                ("absflat", ctypes.c_int32 * 64)]

class VirtualPad:
    def __init__(self, name, keys, axes, vendor=0x045e, product=0x028e,
                 version=0x0110, bustype=BUS_USB):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        if axes:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
            for code in axes:
                fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
        dev = UinputUserDev()
        dev.name = name.encode()[:79]
        dev.bustype, dev.vendor = bustype, vendor
        dev.product, dev.version = product, version
        for code, (lo, hi, flat) in (axes or {}).items():
            dev.absmin[code], dev.absmax[code] = lo, hi
            dev.absflat[code], dev.absfuzz[code] = flat, 0
        os.write(self.fd, bytes(dev))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(1.2)   # let udev create /dev/input/eventN
        self.node = _node_of(self.fd)

    def _emit(self, etype, code, value):
        os.write(self.fd, struct.pack("@llHHi", 0, 0, etype, code, value))

    def key(self, code, down):
        self._emit(EV_KEY, code, 1 if down else 0)
        self._emit(EV_SYN, SYN_REPORT, 0)

    def abs(self, code, value):
        self._emit(EV_ABS, code, value)
        self._emit(EV_SYN, SYN_REPORT, 0)

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)
        # Wait for the node to actually go: a pad whose removal has not
        # landed is a pad the next engine will still enumerate.
        node = getattr(self, "node", None)
        if node:
            for _ in range(50):
                if not os.path.exists(node):
                    break
                time.sleep(0.1)
