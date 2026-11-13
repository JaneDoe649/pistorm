#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Capture the Mac keyboard and mouse and send them to a PiStorm ("remoteinput" cfg option).

Setup (once):
    pip3 install pyobjc-framework-Quartz
    System Settings > Privacy & Security > Input Monitoring AND Accessibility: allow your terminal app.

Usage:
    # let the script open the SSH tunnel itself
    ./mac_client.py --ssh pi@raspberrypi.local
    # or open the tunnel yourself:  ssh -N -L 7777:127.0.0.1:7777 pi@raspberrypi.local
    ./mac_client.py

Hotkeys (Control+Option+...):
    G   release / re-grab the keyboard and mouse
    Q   quit
While grabbed, every event is swallowed by this script and forwarded to the Amiga (Mac Command = Amiga key).
"""

import argparse
import atexit
import socket
import subprocess
import sys
import threading
import time

# macOS virtual key codes (HIToolbox kVK_*) -> Linux KEY_* codes, which is what PiStorm's keymap_amiga expects
MAC_TO_LINUX = {
    0: 30, 1: 31, 2: 32, 3: 33, 4: 35, 5: 34, 6: 44, 7: 45, 8: 46, 9: 47, 10: 86, 11: 48,
    12: 16, 13: 17, 14: 18, 15: 19, 16: 21, 17: 20, 18: 2, 19: 3, 20: 4, 21: 5, 22: 7, 23: 6,
    24: 13, 25: 10, 26: 8, 27: 12, 28: 9, 29: 11, 30: 27, 31: 24, 32: 22, 33: 26, 34: 23, 35: 25,
    36: 28, 37: 38, 38: 36, 39: 40, 40: 37, 41: 39, 42: 43, 43: 51, 44: 53, 45: 49, 46: 50, 47: 52,
    48: 15, 49: 57, 50: 41, 51: 14, 53: 1,
    # modifiers: Command -> Amiga keys, Control (left or right) -> Amiga Ctrl, Option -> Alt
    54: 126, 55: 125, 56: 42, 57: 58, 58: 56, 59: 29, 60: 54, 61: 100, 62: 29,
    # keypad
    65: 83, 67: 55, 69: 78, 75: 98, 76: 96, 78: 74,
    82: 82, 83: 79, 84: 80, 85: 81, 86: 75, 87: 76, 88: 77, 89: 71, 91: 72, 92: 73,
    # function keys, navigation
    96: 63, 97: 64, 98: 65, 99: 61, 100: 66, 101: 67, 103: 87, 109: 68, 111: 88,
    114: 138, 115: 102, 116: 104, 117: 111, 118: 62, 119: 107, 120: 60, 121: 109, 122: 59,
    123: 105, 124: 106, 125: 108, 126: 103,
}

# device dependent flag bits that tell whether a given modifier key went down or up
MOD_DEVICE_MASK = {56: 0x2, 60: 0x4, 59: 0x1, 62: 0x2000, 58: 0x20, 61: 0x40, 55: 0x8, 54: 0x10}
KEY_CAPSLOCK_MAC = 57
KEY_G, KEY_Q = 5, 12
MOUSE_RATE_HZ = 60  # the Amiga samples the mouse once per frame (50 Hz), more is useless


class Link:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.dx = self.dy = 0
        self.buttons = 0
        self.held = set()
        self.alive = True

    def send(self, line):
        try:
            with self.lock:
                self.sock.sendall((line + "\n").encode())
        except OSError:
            self.alive = False

    def key(self, linux_code, down):
        if down:
            self.held.add(linux_code)
        else:
            self.held.discard(linux_code)
        self.send("K %d %d" % (linux_code, 1 if down else 0))

    def release_all(self):
        for code in list(self.held):
            self.key(code, False)
        self.buttons = 0
        self.send("B 0")

    def add_motion(self, dx, dy):
        self.dx += dx
        self.dy += dy

    def flush_motion(self):
        dx, dy, self.dx, self.dy = self.dx, self.dy, 0, 0
        if dx or dy:
            # stay within what the server accepts per line without surprises
            self.send("M %d %d" % (max(-1000, min(1000, dx)), max(-1000, min(1000, dy))))


def connect(args):
    tunnel = None
    if args.ssh:
        cmd = ["ssh", "-N", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=5",
               "-L", "%d:127.0.0.1:%d" % (args.port, args.port), args.ssh]
        print("Opening SSH tunnel:", " ".join(cmd))
        tunnel = subprocess.Popen(cmd)
        atexit.register(tunnel.terminate)
    deadline = time.time() + 15
    while True:
        try:
            s = socket.create_connection((args.host, args.port), timeout=3)
            s.settimeout(None)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return s
        except OSError as e:
            if tunnel and tunnel.poll() is not None:
                sys.exit("SSH tunnel exited (code %s)" % tunnel.returncode)
            if time.time() > deadline:
                sys.exit("Cannot connect to %s:%d (%s). Is 'remoteinput %d' set in the PiStorm cfg?"
                         % (args.host, args.port, e, args.port))
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssh", metavar="USER@HOST", help="open the SSH tunnel to this Raspberry Pi")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--start-released", action="store_true", help="don't grab the inputs at startup")
    args = ap.parse_args()

    try:
        import Quartz as Q
    except ImportError:
        sys.exit("Missing dependency: pip3 install pyobjc-framework-Quartz")

    link = Link(connect(args))
    state = {"grabbed": False}

    def set_grab(on):
        if on == state["grabbed"]:
            return
        state["grabbed"] = on
        if on:
            Q.CGAssociateMouseAndMouseCursorPosition(False)
            Q.CGDisplayHideCursor(Q.CGMainDisplayID())
            print("Inputs grabbed (Control+Option+G to release)")
        else:
            link.release_all()
            Q.CGAssociateMouseAndMouseCursorPosition(True)
            Q.CGDisplayShowCursor(Q.CGMainDisplayID())
            print("Inputs released")

    def ungrab_at_exit():
        try:
            link.release_all()
        except Exception:
            pass
        Q.CGAssociateMouseAndMouseCursorPosition(True)
        Q.CGDisplayShowCursor(Q.CGMainDisplayID())

    atexit.register(ungrab_at_exit)

    def motion_loop():
        while link.alive:
            time.sleep(1.0 / MOUSE_RATE_HZ)
            if state["grabbed"]:
                link.flush_motion()
        print("Connection lost")
        Q.CFRunLoopStop(Q.CFRunLoopGetMain())

    threading.Thread(target=motion_loop, daemon=True).start()

    button_bit = {Q.kCGEventLeftMouseDown: (1, True), Q.kCGEventLeftMouseUp: (1, False),
                  Q.kCGEventRightMouseDown: (2, True), Q.kCGEventRightMouseUp: (2, False)}

    def set_button(bit, down):
        link.buttons = (link.buttons | bit) if down else (link.buttons & ~bit)
        link.send("B %d" % link.buttons)

    def callback(proxy, etype, event, refcon):
        if etype in (Q.kCGEventTapDisabledByTimeout, Q.kCGEventTapDisabledByUserInput):
            Q.CGEventTapEnable(tap, True)
            return event

        keycode = Q.CGEventGetIntegerValueField(event, Q.kCGKeyboardEventKeycode)
        flags = Q.CGEventGetFlags(event)

        # local hotkeys, always active
        if etype in (Q.kCGEventKeyDown, Q.kCGEventKeyUp) and keycode in (KEY_G, KEY_Q) \
                and (flags & Q.kCGEventFlagMaskControl) and (flags & Q.kCGEventFlagMaskAlternate):
            if etype == Q.kCGEventKeyDown:
                if keycode == KEY_G:
                    set_grab(not state["grabbed"])
                else:
                    Q.CFRunLoopStop(Q.CFRunLoopGetCurrent())
            return None

        if not state["grabbed"]:
            return event

        if etype == Q.kCGEventKeyDown or etype == Q.kCGEventKeyUp:
            if Q.CGEventGetIntegerValueField(event, Q.kCGKeyboardEventAutorepeat):
                return None  # the Amiga does its own key repeat
            code = MAC_TO_LINUX.get(keycode)
            if code is not None:
                link.key(code, etype == Q.kCGEventKeyDown)
        elif etype == Q.kCGEventFlagsChanged:
            if keycode == KEY_CAPSLOCK_MAC:
                link.send("K 58 1")  # latching key: one event per toggle
            elif keycode in MOD_DEVICE_MASK:
                link.key(MAC_TO_LINUX[keycode], bool(flags & MOD_DEVICE_MASK[keycode]))
        elif etype in (Q.kCGEventMouseMoved, Q.kCGEventLeftMouseDragged,
                       Q.kCGEventRightMouseDragged, Q.kCGEventOtherMouseDragged):
            link.add_motion(Q.CGEventGetIntegerValueField(event, Q.kCGMouseEventDeltaX),
                            Q.CGEventGetIntegerValueField(event, Q.kCGMouseEventDeltaY))
        elif etype in button_bit:
            bit, down = button_bit[etype]
            set_button(bit, down)
        elif etype in (Q.kCGEventOtherMouseDown, Q.kCGEventOtherMouseUp):
            if Q.CGEventGetIntegerValueField(event, Q.kCGMouseEventButtonNumber) == 2:
                set_button(4, etype == Q.kCGEventOtherMouseDown)
        elif etype == Q.kCGEventScrollWheel:
            delta = Q.CGEventGetIntegerValueField(event, Q.kCGScrollWheelEventDeltaAxis1)
            if delta:
                link.send("W %d" % max(-5, min(5, delta)))
        return None  # swallow: nothing reaches the Mac while grabbed

    events = [Q.kCGEventKeyDown, Q.kCGEventKeyUp, Q.kCGEventFlagsChanged, Q.kCGEventMouseMoved,
              Q.kCGEventLeftMouseDown, Q.kCGEventLeftMouseUp, Q.kCGEventLeftMouseDragged,
              Q.kCGEventRightMouseDown, Q.kCGEventRightMouseUp, Q.kCGEventRightMouseDragged,
              Q.kCGEventOtherMouseDown, Q.kCGEventOtherMouseUp, Q.kCGEventOtherMouseDragged,
              Q.kCGEventScrollWheel]
    mask = 0
    for ev in events:
        mask |= Q.CGEventMaskBit(ev)

    tap = Q.CGEventTapCreate(Q.kCGSessionEventTap, Q.kCGHeadInsertEventTap,
                             Q.kCGEventTapOptionDefault, mask, callback, None)
    if tap is None:
        sys.exit("Cannot create the event tap: allow this terminal in System Settings > "
                 "Privacy & Security > Input Monitoring and Accessibility, then restart it.")

    source = Q.CFMachPortCreateRunLoopSource(None, tap, 0)
    Q.CFRunLoopAddSource(Q.CFRunLoopGetCurrent(), source, Q.kCFRunLoopCommonModes)
    Q.CGEventTapEnable(tap, True)

    if not args.start_released:
        set_grab(True)
    else:
        print("Started released (Control+Option+G to grab)")
    Q.CFRunLoopRun()


if __name__ == "__main__":
    main()
