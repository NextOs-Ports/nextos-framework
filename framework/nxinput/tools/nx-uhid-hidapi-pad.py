#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxinput 0.11.1 (H2a kit): a HIDAPI-class gamepad on the bench through /dev/uhid.

The uinput clone proves ONLY the evdev path. To claim the HIDAPI path (SDL's
own HID drivers over hidraw: PS4/PS5/Switch/...), a device must exist at the
HID level, with a pinned report descriptor and inputs as HID reports. This
tool creates one through the kernel's uhid interface (root, or a writable
/dev/uhid): a DualShock 4 (054c:05c4) by default, because SDL's HIDAPI PS4
driver is the most widely enabled one and the kernel's hid-playstation/
hid-sony driver also binds it (so BOTH routes exist: hidraw for SDL, evdev
for the kernel -- exactly the arbitration the pre-router's physical graph
covers). Feature reports the drivers request (0x02 calibration, 0x12 MAC,
0x81, 0xa3 firmware) are answered with fixed, plausible data.

usage: nx-uhid-hidapi-pad.py [--seconds N] [--press south|east|west|north|
       start|select,...] [--interval S]  (presses cycle until --seconds ends)
Prints UHID lines (created, get-report answered, output received) so an
oracle can read what the kernel/driver asked. Ctrl-C or --seconds ends it.
Claim stays [!] until a real SDL probe on the device reports the pad through
its HIDAPI path (SDL_JoystickPath = /dev/hidraw*, NXC6-DEVICE driver=hidapi).
"""
import argparse, fcntl, os, select, struct, sys, time

UHID_CREATE2, UHID_DESTROY, UHID_START, UHID_STOP, UHID_OPEN, UHID_CLOSE, UHID_OUTPUT, UHID_GET_REPORT, UHID_GET_REPORT_REPLY, UHID_INPUT2, UHID_SET_REPORT, UHID_SET_REPORT_REPLY = 11, 1, 2, 3, 4, 5, 6, 9, 10, 12, 13, 14
BUS_USB = 3
# DualShock 4 (CUH-ZCT2) report descriptor, USB, as the kernel and SDL know it
DS4_RDESC = bytes.fromhex(
    "05010905a1018501092a1500250a75084095050901"  # partial header is replaced below
)
# A faithful DS4 descriptor (from the sony USB device; 507 bytes). Kept literal so the fixture is pinned.
DS4_RDESC = bytes.fromhex(
    "05010905a10185010930093109320935150026ff0075089504810509390507150025073500463b0165148142650009330934150026ff007508950281050901190129"
    "0e150025017501950e81020601ff09200601ff81020601ff752095368102850509220601ff0922952f9102850409230601ff09239524b102850209240601ff0924952"
    "4b102850809250601ff0925950391028510092609260601ff950491028511092709270601ff9502910285120928092806ff0095039102851309290601ff0929950391"
    "028514092a0601ff092a950391028515092b0601ff092b950391028516092c0601ff092c950391028517092d0601ff092d950391028518092e0601ff092e9503910285"
    "19092f0601ff092f950391028520093006ff00093095039102852109310601ff09319503910285220932063200093295039102852309330601ff0933950391028524"
    "09340601ff0934950391028525093506ff000935950391028526093606ff00093695039102852709370601ff0937950391028528093806ff0009389503910285290939"
    "0601ff0939950391028530093a0601ff093a950391028531093b0601ff093b950391028532093c0601ff093c950391028533093d0601ff093d950391028534093e0601"
    "ff093e950391028535093f0601ff093f95039102c0"
)

def ev(kind, payload=b""):
    return struct.pack("<I", kind) + payload

# A minimal, textbook HID gamepad (8 buttons, X/Y) -- the control case that
# proves the uhid path itself on a kernel before the DS4 descriptor is tried.
GENERIC_RDESC = bytes.fromhex(
    "05010905a1010901a100" "0509190129081500250175019508810205010930093115002500ff7508950281020901c0c0".replace("2500ff", "26ff00"))

def create(fd, name, vid, pid, rdesc):
    # struct uhid_create2_req { u8 name[128]; u8 phys[64]; u8 uniq[64]; u16 rd_size; u16 bus; u32 vendor; u32 product; u32 version; u32 country; u8 rd_data[4096]; }
    req = name.encode()[:127].ljust(128, b"\0") + b"nx-uhid-bench".ljust(64, b"\0") + b"".ljust(64, b"\0")
    req += struct.pack("<HHIIII", len(rdesc), BUS_USB, vid, pid, 0x8111, 0)
    req += rdesc.ljust(4096, b"\0")
    try:
        n = os.write(fd, ev(UHID_CREATE2, req))
    except OSError as e:
        print("UHID create2 failed: %s (kernel refused the descriptor or the request)" % e, flush=True); raise
    print("UHID create2 wrote %d bytes" % n, flush=True)

def input_report(fd, data):
    # struct uhid_input2_req { u16 size; u8 data[4096]; }
    os.write(fd, ev(UHID_INPUT2, struct.pack("<H", len(data)) + data.ljust(4096, b"\0")))

def ds4_input(buttons=0, lx=128, ly=128, rx=128, ry=128, hat=8, l2=0, r2=0, counter=0):
    # report 0x01, 64 bytes (USB): lx ly rx ry | buttons0 (hat low nibble + face) | buttons1 | buttons2 (counter<<2 | ps | tpad) | l2 r2 | timestamp ...
    b0 = (hat & 0x0f) | ((buttons & 0x0f) << 4)          # square=0x10 cross=0x20 circle=0x40 triangle=0x80
    b1 = (buttons >> 4) & 0xff                            # l1 r1 l2 r2 share options l3 r3
    b2 = ((counter & 0x3f) << 2) | ((buttons >> 12) & 0x03)  # ps, touchpad
    rep = bytes([0x01, lx, ly, rx, ry, b0, b1, b2, l2, r2]) + bytes(54)
    return rep

# positional Xbox surface -> DS4 button bits (south=cross, east=circle, west=square, north=triangle)
POS = {"west": 0x001, "south": 0x002, "east": 0x004, "north": 0x008, "l1": 0x010, "r1": 0x020, "l2": 0x040, "r2": 0x080, "select": 0x100, "start": 0x200, "l3": 0x400, "r3": 0x800, "guide": 0x1000}

def get_report_reply(fd, rid, rnum, rtype):
    # struct uhid_get_report_reply_req { u32 id; u16 err; u16 size; u8 data[4096]; }
    data = b""
    if rnum == 0x02:   # calibration (37 bytes): zeros mean "no bias" for the driver
        data = bytes([0x02]) + bytes(36)
    elif rnum == 0x12: # MAC address: report id, 6 bytes MAC, 3 bytes pad?, 6 bytes host MAC
        data = bytes([0x12, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x08, 0x25, 0x00, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11])
    elif rnum == 0x81: # MAC (alt)
        data = bytes([0x81, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
    elif rnum == 0xa3: # firmware/hardware version block (49 bytes)
        data = bytes([0xa3]) + b"Jun  9 2017".ljust(16, b"\0") + b"12:32:11".ljust(16, b"\0") + bytes(16)
    elif rnum == 0x05: # motion calibration (41 bytes)
        data = bytes([0x05]) + bytes(40)
    else:
        os.write(fd, ev(UHID_GET_REPORT_REPLY, struct.pack("<IHH", rid, 0x16, 0) + bytes(4096)))  # EINVAL: not supported
        return rnum, False
    os.write(fd, ev(UHID_GET_REPORT_REPLY, struct.pack("<IHH", rid, 0, len(data)) + data.ljust(4096, b"\0")))
    return rnum, True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--press", default="south,east,west,north,select,start")
    ap.add_argument("--interval", type=float, default=0.6)
    ap.add_argument("--name", default="Wireless Controller")
    ap.add_argument("--vid", type=lambda s: int(s, 16), default=0x054c)
    ap.add_argument("--pid", type=lambda s: int(s, 16), default=0x05c4)
    ap.add_argument("--generic", action="store_true", help="textbook HID gamepad (vid/pid as given) instead of the DS4 descriptor: the uhid control case")
    ap.add_argument("--ds4-input", action="store_true", help="with --generic: keep the generic descriptor (hid-generic binds, hidraw appears) but send DS4-format input reports and answer DS4 feature reports -- SDL's HIDAPI PS4 driver matches by VID/PID over hidraw, not by descriptor")
    a = ap.parse_args()
    fd = os.open("/dev/uhid", os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
    rdesc = GENERIC_RDESC if a.generic else DS4_RDESC
    create(fd, a.name, a.vid, a.pid, rdesc)
    print("UHID created name=%r vid=%04x pid=%04x rdesc_bytes=%d ds4_input=%d" % (a.name, a.vid, a.pid, len(rdesc), int(a.ds4_input)), flush=True)
    presses = [p for p in a.press.split(",") if p]
    t0 = time.time(); counter = 0; started = False; next_press = t0 + 1.5; pi = 0; opened = 0
    try:
        while time.time() - t0 < a.seconds:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try: buf = os.read(fd, 4096)
                except BlockingIOError: buf = b""
                if len(buf) >= 4:
                    kind = struct.unpack("<I", buf[:4])[0]
                    if kind == UHID_START: started = True; print("UHID start", flush=True)
                    elif kind == UHID_OPEN: opened += 1; print("UHID open (a reader attached: driver or hidraw client)", flush=True)
                    elif kind == UHID_CLOSE: print("UHID close", flush=True)
                    elif kind == UHID_GET_REPORT:
                        rid, rnum, rtype = struct.unpack("<IBB", buf[4:10]); num, ok = get_report_reply(fd, rid, rnum, rtype)
                        print("UHID get_report num=0x%02x type=%d answered=%s" % (num, rtype, ok), flush=True)
                    elif kind == UHID_SET_REPORT:
                        rid = struct.unpack("<I", buf[4:8])[0]; os.write(fd, ev(UHID_SET_REPORT_REPLY, struct.pack("<IH", rid, 0) + bytes(2))); print("UHID set_report acked", flush=True)
                    elif kind == UHID_OUTPUT:
                        print("UHID output report received (rumble/led): %d bytes" % (len(buf) - 4), flush=True)
            if started:
                now = time.time()
                if now >= next_press and presses:
                    bits = POS.get(presses[pi % len(presses)], 0)
                    if a.generic and not a.ds4_input:
                        gb = {"south": 1, "east": 2, "west": 4, "north": 8, "select": 64, "start": 128}.get(presses[pi % len(presses)], 0)
                        input_report(fd, bytes([gb, 127, 127])); time.sleep(0.12); input_report(fd, bytes([0, 127, 127]))
                    else:
                        counter = (counter + 1) & 0x3f; input_report(fd, ds4_input(bits, counter=counter)); time.sleep(0.12)
                        counter = (counter + 1) & 0x3f; input_report(fd, ds4_input(0, counter=counter))
                    print("UHID pressed %s (bits=0x%03x)" % (presses[pi % len(presses)], bits), flush=True)
                    pi += 1; next_press = now + a.interval
                elif not a.generic or a.ds4_input:
                    counter = (counter + 1) & 0x3f; input_report(fd, ds4_input(0, counter=counter))
    finally:
        os.write(fd, ev(UHID_DESTROY)); os.close(fd)
        print("UHID destroyed opens=%d" % opened, flush=True)

if __name__ == "__main__":
    main()
