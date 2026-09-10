#!/usr/bin/env python3
"""A virtual gamepad on /dev/uinput, to test a port's real input path.

A port's controller code is usually only ever proven by a human holding a pad,
which means it is not proven in an unattended run at all.  This creates a real
evdev device with the standard gamepad buttons and axes, so the events travel
the whole way the physical pad's do -- kernel input layer, SDL's evdev
backend, SDL's GameController mapping, and only then the port.

Usage (on the device):
    virtual_pad.py <script>

where <script> is ';'-separated steps, one per second unless a step says
otherwise:

    right / left / up / down    hold that d-pad direction
    lx+ / lx- / ly+ / ly-       hold the left stick that way
    lx=N[@S] / ly=N[@S]         set an exact stick value, optionally for S sec
    rx=N[@S] / ry=N[@S]
    a / b / x / y / start /
    select / l1 / r1            tap that button
    +name / -name               press / release without a tap
    release                     let everything go
    waitN                       wait N seconds
"""
import fcntl, os, struct, sys, time

UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0

BTN = {
    'a': 0x130, 'b': 0x131, 'x': 0x134, 'y': 0x133,
    'l1': 0x136, 'r1': 0x137, 'select': 0x13a, 'start': 0x13b,
    'mode': 0x13c, 'l3': 0x13d, 'r3': 0x13e,
}
ABS_X, ABS_Y, ABS_RX, ABS_RY = 0x00, 0x01, 0x03, 0x04
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
AXES = [ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y]


def create():
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_ABS, EV_SYN):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for code in BTN.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for code in AXES:
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)

    absmax, absmin = [0] * 64, [0] * 64
    for code in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
        absmax[code], absmin[code] = 32767, -32767
    for code in (ABS_HAT0X, ABS_HAT0Y):
        absmax[code], absmin[code] = 1, -1

    dev = (b'NextOS Virtual Pad'.ljust(80, b'\0')
           + struct.pack('<HHHH', 3, 0x1209, 0x7070, 1)   # bustype USB
           + struct.pack('<i', 0)
           + struct.pack('<64i', *absmax) + struct.pack('<64i', *absmin)
           + struct.pack('<64i', *([0] * 64)) + struct.pack('<64i', *([0] * 64)))
    os.write(fd, dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, etype, code, value):
    # struct input_event: struct timeval (two 64-bit longs on this ABI), then
    # u16 type, u16 code, s32 value.  struct's '<' prefix uses standard sizes,
    # where 'l' is four bytes -- the timeval needs 'q'.
    now = time.time()
    os.write(fd, struct.pack('<qqHHi', int(now), int((now % 1) * 1e6),
                             etype, code, value))


def sync(fd):
    emit(fd, EV_SYN, SYN_REPORT, 0)


def main():
    steps = sys.argv[1].split(';') if len(sys.argv) > 1 else ['wait5']
    fd = create()
    print('virtual pad created; giving the game a moment to notice it',
          flush=True)
    time.sleep(4)

    held_axes, held_buttons = {}, set()

    def set_axis(code, value):
        held_axes[code] = value
        emit(fd, EV_ABS, code, value)
        sync(fd)

    def set_button(name, down):
        emit(fd, EV_KEY, BTN[name], 1 if down else 0)
        sync(fd)
        held_buttons.discard(name)
        if down:
            held_buttons.add(name)

    AXIS_STEP = {
        'right': (ABS_HAT0X, 1), 'left': (ABS_HAT0X, -1),
        'down': (ABS_HAT0Y, 1), 'up': (ABS_HAT0Y, -1),
        'lx+': (ABS_X, 32767), 'lx-': (ABS_X, -32767),
        'ly+': (ABS_Y, 32767), 'ly-': (ABS_Y, -32767),
        'rx+': (ABS_RX, 32767), 'rx-': (ABS_RX, -32767),
        'ry+': (ABS_RY, 32767), 'ry-': (ABS_RY, -32767),
    }
    EXACT_AXIS = {
        'lx': ABS_X, 'ly': ABS_Y, 'rx': ABS_RX, 'ry': ABS_RY,
    }

    for step in steps:
        step = step.strip()
        if not step:
            continue
        print('step:', step, flush=True)
        if step.startswith('wait'):
            time.sleep(float(step[4:] or 1))
        elif step == 'release':
            for code in list(held_axes):
                set_axis(code, 0)
            for name in list(held_buttons):
                set_button(name, False)
            time.sleep(0.5)
        elif step in AXIS_STEP:
            code, value = AXIS_STEP[step]
            set_axis(code, value)
            time.sleep(1)
        elif '=' in step and step.split('=', 1)[0] in EXACT_AXIS:
            name, raw_value = step.split('=', 1)
            duration = 1.0
            if '@' in raw_value:
                raw_value, raw_duration = raw_value.split('@', 1)
                try:
                    duration = float(raw_duration)
                except ValueError:
                    print('  (invalid axis duration, ignored)', flush=True)
                    continue
            try:
                value = int(raw_value, 10)
            except ValueError:
                print('  (invalid axis value, ignored)', flush=True)
                continue
            if value < -32767 or value > 32767:
                print('  (axis value out of range, ignored)', flush=True)
                continue
            if duration < 0.0 or duration > 60.0:
                print('  (axis duration out of range, ignored)', flush=True)
                continue
            set_axis(EXACT_AXIS[name], value)
            time.sleep(duration)
        elif step.startswith('+') and step[1:] in BTN:
            set_button(step[1:], True)
            time.sleep(1)
        elif step.startswith('-') and step[1:] in BTN:
            set_button(step[1:], False)
            time.sleep(0.5)
        elif step in BTN:
            set_button(step, True)
            time.sleep(0.15)
            set_button(step, False)
            time.sleep(0.85)
        else:
            print('  (unknown step, ignored)', flush=True)

    for code in list(held_axes):
        set_axis(code, 0)
    for name in list(held_buttons):
        set_button(name, False)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
    print('virtual pad removed', flush=True)


main()
