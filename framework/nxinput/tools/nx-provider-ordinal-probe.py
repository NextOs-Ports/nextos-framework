#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nx-provider-ordinal-probe -- measure the ordinal table (SDL button index
-> EV_KEY, axis index -> EV_ABS) of ONE real libSDL2 against a uinput pad
whose capabilities are given on the command line. Runs on any Linux with
python3/ctypes/uinput (root or uinput access). Prints a JSON receipt with
the library sha256 (of the file the loader mapped, via /proc/self/maps),
the exported ById capability and the measured table.

  nx-provider-ordinal-probe.py LIB [--keys 0x72,0x73,0x130-0x13c] [--abs 0,1,2,3,0x10,0x11] [--name NAME]

Evidence class: PROVIDER_TABLE_MEASURED (host-or-device evidence of the
provider bytes; NOT a physical-legend proof)."""
import ctypes, fcntl, hashlib, json, os, struct, sys, time

def parse_list(spec):
    out = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part[1:]:
            a, b = part.split("-", 1)
            out += list(range(int(a, 0), int(b, 0) + 1))
        elif part:
            out.append(int(part, 0))
    return out

def main():
    args = sys.argv[1:]
    lib = args[0]
    keys = parse_list("0x72,0x73,0x130-0x13c"); absl = parse_list("0,1,2,3,0x10,0x11"); name = "NX V5 ordinal probe pad"
    i = 1
    while i < len(args):
        if args[i] == "--keys": keys = parse_list(args[i + 1]); i += 2
        elif args[i] == "--abs": absl = parse_list(args[i + 1]); i += 2
        elif args[i] == "--name": name = args[i + 1]; i += 2
        else: i += 1
    UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
    UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    for t in (0x00, 0x01, 0x03): fcntl.ioctl(fd, UI_SET_EVBIT, t)
    for k in keys: fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    for a in absl: fcntl.ioctl(fd, UI_SET_ABSBIT, a)
    amax = [0] * 64; amin = [0] * 64; fuzz = [0] * 64; flat = [0] * 64
    for a in absl:
        if 0x10 <= a <= 0x17: amin[a], amax[a] = -1, 1
        else: amin[a], amax[a], fuzz[a], flat[a] = -32768, 32767, 16, 128
    payload = name.encode()[:79].ljust(80, b"\0") + struct.pack("HHHH", 0x03, 0x0912, 0xc5a1, 0x0110) + struct.pack("i", 0)
    payload += struct.pack("64i", *amax) + struct.pack("64i", *amin) + struct.pack("64i", *fuzz) + struct.pack("64i", *flat)
    os.write(fd, payload); fcntl.ioctl(fd, UI_DEV_CREATE); time.sleep(0.8)
    def emit(t, c, v): os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))
    sdl = ctypes.CDLL(lib)
    # The library the loader REALLY mapped for this handle.
    sdl.SDL_Init.restype = ctypes.c_int
    addr = ctypes.cast(sdl.SDL_Init, ctypes.c_void_p).value
    mapped = None
    for line in open("/proc/self/maps"):
        f = line.split()
        s, e = (int(x, 16) for x in f[0].split("-"))
        if s <= addr < e: mapped = f[5] if len(f) > 5 else None; break
    lib_sha = hashlib.sha256(open(mapped, "rb").read()).hexdigest() if mapped else None
    bytable = all(hasattr(sdl, n) for n in ("SDL_JoystickButtonEventCodeById", "SDL_JoystickAxisEventCodeById", "SDL_JoystickHatEventCodeById", "SDL_JoystickDevicePathById"))
    sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
    assert sdl.SDL_Init(0x200) == 0, sdl.SDL_GetError()
    sdl.SDL_JoystickOpen.restype = ctypes.c_void_p; sdl.SDL_JoystickName.restype = ctypes.c_char_p
    sdl.SDL_JoystickName.argtypes = [ctypes.c_void_p]; sdl.SDL_JoystickNumButtons.argtypes = [ctypes.c_void_p]
    sdl.SDL_JoystickNumAxes.argtypes = [ctypes.c_void_p]; sdl.SDL_JoystickNumHats.argtypes = [ctypes.c_void_p]
    sdl.SDL_JoystickInstanceID.argtypes = [ctypes.c_void_p]; sdl.SDL_JoystickClose.argtypes = [ctypes.c_void_p]
    joy = None
    for _ in range(20):
        sdl.SDL_PumpEvents()
        for i in range(sdl.SDL_NumJoysticks()):
            j = sdl.SDL_JoystickOpen(i); nm = sdl.SDL_JoystickName(j)
            if nm and name.encode() in nm: joy = j; break
            sdl.SDL_JoystickClose(j)
        if joy: break
        time.sleep(0.1)
    assert joy, "clone not seen"
    inst = sdl.SDL_JoystickInstanceID(joy)
    nb, na, nh = sdl.SDL_JoystickNumButtons(joy), sdl.SDL_JoystickNumAxes(joy), sdl.SDL_JoystickNumHats(joy)
    class Ev(ctypes.Structure): _fields_ = [("type", ctypes.c_uint32), ("timestamp", ctypes.c_uint32), ("which", ctypes.c_int32), ("a", ctypes.c_uint8), ("b", ctypes.c_uint8), ("pad", ctypes.c_uint8 * 54)]
    class AxEv(ctypes.Structure): _fields_ = [("type", ctypes.c_uint32), ("timestamp", ctypes.c_uint32), ("which", ctypes.c_int32), ("axis", ctypes.c_uint8), ("p", ctypes.c_uint8 * 3), ("value", ctypes.c_int16)]
    ev = Ev()
    def pump(ms=120):
        t0 = time.time(); seen = []
        while time.time() - t0 < ms / 1000:
            while sdl.SDL_PollEvent(ctypes.byref(ev)):
                if ev.type == 0x603: seen.append(("b", ev.a))
                elif ev.type == 0x600:
                    ax = ctypes.cast(ctypes.byref(ev), ctypes.POINTER(AxEv)).contents
                    seen.append(("a", ax.axis))
            time.sleep(0.004)
        return seen
    pump(300)
    buttons = {}
    for k in keys:
        emit(1, k, 1); emit(0, 0, 0); d = pump(); emit(1, k, 0); emit(0, 0, 0); pump()
        buttons["0x%x" % k] = sorted({o for t, o in d if t == "b"})
    axes = {}
    for a in absl:
        if 0x10 <= a <= 0x17: continue
        emit(3, a, 30000); emit(0, 0, 0); d = pump(); emit(3, a, 0); emit(0, 0, 0); pump()
        axes["0x%x" % a] = sorted({o for t, o in d if t == "a"})
    byid = {}
    if bytable:
        for o in range(nb): byid["b%d" % o] = "0x%x" % sdl.SDL_JoystickButtonEventCodeById(inst, o)
        for o in range(na): byid["a%d" % o] = "0x%x" % sdl.SDL_JoystickAxisEventCodeById(inst, o)
    ver = (ctypes.c_uint8 * 3)(); sdl.SDL_GetVersion(ctypes.byref(ver))
    sdl.SDL_GetRevision.restype = ctypes.c_char_p
    # classify against the two known domains
    ks = sorted(keys)
    asc = {"0x%x" % c: [i] for i, c in enumerate(ks)}
    hf = {"0x%x" % c: [i] for i, c in enumerate([c for c in ks if c >= 0x120] + [c for c in ks if c < 0x120])}
    domain = "sdl2-ascending-patched" if buttons == asc else "sdl2-evdev" if buttons == hf else "unclassified"
    rec = {"receipt": "nx-provider-ordinal-probe/1", "class": "PROVIDER_TABLE_MEASURED", "lib_requested": os.path.basename(lib),
           "lib_mapped_class": "system-lib" if mapped and mapped.startswith("/usr/lib") else "other", "lib_sha256": lib_sha,
           "sdl_version": "%d.%d.%d" % (ver[0], ver[1], ver[2]), "sdl_revision": sdl.SDL_GetRevision().decode(errors="replace"),
           "exports_bytable": bytable, "nbuttons": nb, "naxes": na, "nhats": nh, "buttons": buttons, "axes": axes, "byid": byid,
           "domain_measured": domain, "uname": os.uname().machine, "unix": int(time.time())}
    print(json.dumps(rec, indent=1))
    sdl.SDL_JoystickClose(joy); sdl.SDL_Quit(); fcntl.ioctl(fd, UI_DEV_DESTROY)

if __name__ == "__main__":
    main()
