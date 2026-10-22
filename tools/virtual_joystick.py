#!/usr/bin/env python3
"""A virtual joystick (uinput) for testing axis and button bindings without hardware.

    tools/virtual_joystick.py demo        # create "lr-test-pad", wiggle axes forever (Ctrl+C to stop)

or from Python:

    with VirtualJoystick() as js:
        js.axis(2, 0.5)        # ABS_Z to 50 % of its travel
        js.button(0, True)

The device appears as 1234:5678 "lr-test-pad" with 4 axes (X, Y, Z, Rx; 16 bit) and 2 buttons. Needs write access
to /dev/uinput (desktop sessions normally grant it). Run with python3 -I from a clean directory.
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TRIGGER = 0x120
ABS_MAX_VALUE = 65535


class VirtualJoystick:
    NAME = b"lr-test-pad"
    KEY = "1234:5678:lr-test-pad"  # the device key the app uses in its config

    def __init__(self, axes=(0, 1, 2, 3), buttons=2):
        self.axes, self.buttons = tuple(axes), buttons
        self.fd = None

    def __enter__(self):
        fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
        for b in range(self.buttons):
            fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER + b)
        absmax, absmin, absfuzz, absflat = [0] * 64, [0] * 64, [0] * 64, [0] * 64
        for a in self.axes:
            fcntl.ioctl(fd, UI_SET_ABSBIT, a)
            absmax[a] = ABS_MAX_VALUE
        dev = struct.pack("80sHHHHi", self.NAME, 3, 0x1234, 0x5678, 1, 0)
        dev += struct.pack("64i", *absmax) + struct.pack("64i", *absmin) + struct.pack("64i", *absfuzz) + struct.pack("64i", *absflat)
        os.write(fd, dev)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        self.fd = fd
        time.sleep(1.0)  # let udev / the consumer notice the new device
        return self

    def __exit__(self, *exc):
        if self.fd is not None:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
            os.close(self.fd)

    def _emit(self, etype, code, value):
        os.write(self.fd, struct.pack("llHHi", 0, 0, etype, code, value))
        os.write(self.fd, struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))

    def axis(self, code, fraction):
        self._emit(EV_ABS, code, int(round(max(0.0, min(1.0, fraction)) * ABS_MAX_VALUE)))

    def button(self, index, pressed):
        self._emit(EV_KEY, BTN_TRIGGER + index, 1 if pressed else 0)


def demo():
    with VirtualJoystick() as js:
        print("virtual joystick 'lr-test-pad' created; wiggling axes, Ctrl+C to stop")
        t = 0.0
        try:
            while True:
                for a in js.axes:
                    js.axis(a, 0.5 + 0.5 * __import__("math").sin(t + a))
                t += 0.05
                time.sleep(0.03)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "demo":
        demo()
    else:
        print(__doc__)
