# SPDX-License-Identifier: MIT
"""
USB gamepad reading for the PiStorm remote clients, through IOHIDManager (works with generic HID gamepads
that the GameController framework ignores). PyObjC has no IOKit module, so the few calls needed are bound
with ctypes.

PadState is pure logic (HID elements in, Amiga joystick mask out) and can be tested anywhere.
GamepadMonitor needs macOS (and PyObjC for the matching dictionary); check GamepadMonitor.available().

Mask sent to PiStorm ("J <mask>"): 1 up, 2 down, 4 left, 8 right, 16 fire, 32 second button.
"""

UP, DOWN, LEFT, RIGHT, FIRE, BUTTON2 = 1, 2, 4, 8, 16, 32

PAGE_GENERIC_DESKTOP = 0x01
PAGE_BUTTON = 0x09
USAGE_JOYSTICK, USAGE_GAMEPAD = 0x04, 0x05
USAGE_X, USAGE_Y, USAGE_HAT = 0x30, 0x31, 0x39

AXIS_ENTER = 0.5   # fraction of the half range needed to register a direction
AXIS_EXIT = 0.3    # and to release it again (hysteresis)

# hat switch value (0 = up, then clockwise) -> directions
_HAT_DIRS = (UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT)


class PadState:
    """State of one gamepad. Feed it HID values with update(), read the Amiga mask with mask()."""

    def __init__(self):
        self.axis = {USAGE_X: 0, USAGE_Y: 0}  # -1, 0, +1 after hysteresis
        self.hat = 0
        self.fire = False
        self.button2 = False

    def update(self, page, usage, value, lmin, lmax):
        """Returns True if the resulting mask may have changed."""
        if page == PAGE_BUTTON:
            if usage == 1:
                self.fire = bool(value)
            elif usage == 2:
                self.button2 = bool(value)
            else:
                return False
            return True
        if page != PAGE_GENERIC_DESKTOP:
            return False
        if usage in self.axis:
            if lmax <= lmin:
                return False
            n = 2.0 * (value - lmin) / (lmax - lmin) - 1.0  # -1 .. +1
            cur = self.axis[usage]
            if cur == 0:
                new = -1 if n < -AXIS_ENTER else (1 if n > AXIS_ENTER else 0)
            else:
                new = cur if cur * n > AXIS_EXIT else (0 if abs(n) <= AXIS_EXIT else (1 if n > 0 else -1))
            self.axis[usage] = new
            return True
        if usage == USAGE_HAT:
            idx = value - lmin
            if lmax - lmin == 3:  # 4 position hat: up, right, down, left
                idx *= 2
            # anything outside the range (neutral is reported as 8, 15, 0 or -1 depending on the device) = released
            self.hat = _HAT_DIRS[idx] if 0 <= idx < 8 else 0
            return True
        return False

    def mask(self):
        m = self.hat
        x, y = self.axis[USAGE_X], self.axis[USAGE_Y]
        if x < 0:
            m |= LEFT
        elif x > 0:
            m |= RIGHT
        if y < 0:
            m |= UP
        elif y > 0:
            m |= DOWN
        if self.fire:
            m |= FIRE
        if self.button2:
            m |= BUTTON2
        # opposite directions (e.g. d-pad right + stick left) cancel out
        if (m & UP) and (m & DOWN):
            m &= ~(UP | DOWN)
        if (m & LEFT) and (m & RIGHT):
            m &= ~(LEFT | RIGHT)
        return m


_CB = None  # ctypes callback type, built on first use


def _load():
    """ctypes bindings to the few IOHIDManager calls we need (PyObjC has no IOKit module)."""
    import ctypes
    import ctypes.util
    global _CB
    iokit = ctypes.cdll.LoadLibrary(ctypes.util.find_library("IOKit"))
    cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    vp, i32, u32, lng = ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32, ctypes.c_long
    _CB = ctypes.CFUNCTYPE(None, vp, i32, vp, vp)  # (context, result, sender, device or value)
    sigs = {
        "IOHIDManagerCreate": (vp, [vp, u32]),
        "IOHIDManagerSetDeviceMatchingMultiple": (None, [vp, vp]),
        "IOHIDManagerRegisterDeviceMatchingCallback": (None, [vp, _CB, vp]),
        "IOHIDManagerRegisterDeviceRemovalCallback": (None, [vp, _CB, vp]),
        "IOHIDManagerRegisterInputValueCallback": (None, [vp, _CB, vp]),
        "IOHIDManagerScheduleWithRunLoop": (None, [vp, vp, vp]),
        "IOHIDManagerUnscheduleFromRunLoop": (None, [vp, vp, vp]),
        "IOHIDManagerOpen": (i32, [vp, u32]),
        "IOHIDManagerClose": (i32, [vp, u32]),
        "IOHIDDeviceGetProperty": (vp, [vp, vp]),
        "IOHIDValueGetElement": (vp, [vp]),
        "IOHIDValueGetIntegerValue": (lng, [vp]),
        "IOHIDElementGetDevice": (vp, [vp]),
        "IOHIDElementGetUsagePage": (u32, [vp]),
        "IOHIDElementGetUsage": (u32, [vp]),
        "IOHIDElementGetLogicalMin": (lng, [vp]),
        "IOHIDElementGetLogicalMax": (lng, [vp]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(iokit, name)
        fn.restype, fn.argtypes = res, args
    cf.CFRunLoopGetCurrent.restype = vp
    cf.CFRunLoopGetCurrent.argtypes = []
    return iokit, cf, vp.in_dll(cf, "kCFRunLoopCommonModes")


class GamepadMonitor:
    """
    Watches every USB/Bluetooth joystick or gamepad and reports the OR of their masks through on_change(mask).
    on_devices(names) is called when a pad is connected or removed. start() must run on the thread whose run
    loop will deliver the events (the main thread), after the run loop is, or is about to be, running.
    """

    def __init__(self, on_change, on_devices=None):
        self.on_change = on_change
        self.on_devices = on_devices
        self._pads = {}    # IOHIDDeviceRef address -> (name, PadState)
        self._last = 0
        self._manager = None

    @staticmethod
    def available():
        import sys
        if sys.platform != "darwin":
            return False
        try:
            import ctypes.util
            import objc  # noqa: F401
            return bool(ctypes.util.find_library("IOKit"))
        except ImportError:
            return False

    def device_names(self):
        return [name for name, _ in self._pads.values()]

    def mask(self):
        m = 0
        for _, st in self._pads.values():
            m |= st.mask()
        return m

    def _emit(self):
        m = self.mask()
        if m != self._last:
            self._last = m
            self.on_change(m)

    def start(self):
        import ctypes
        import objc
        from Foundation import NSArray, NSString

        iokit, cf, modes = _load()
        self._iokit, self._cf, self._modes = iokit, cf, modes
        ref = lambda o: ctypes.c_void_p(objc.pyobjc_id(o))  # toll-free bridged CF object -> raw pointer

        def device_name(device):
            ptr = iokit.IOHIDDeviceGetProperty(device, ref(NSString.stringWithString_("Product")))
            return str(objc.objc_object(c_void_p=ptr)) if ptr else "Manette"

        def attached(ctx, result, sender, device):
            self._pads[device] = (device_name(device), PadState())
            if self.on_devices:
                self.on_devices(self.device_names())

        def removed(ctx, result, sender, device):
            self._pads.pop(device, None)
            if self.on_devices:
                self.on_devices(self.device_names())
            self._emit()

        def value(ctx, result, sender, val):
            element = iokit.IOHIDValueGetElement(val)
            entry = self._pads.get(iokit.IOHIDElementGetDevice(element))
            if entry is None:
                return
            if entry[1].update(iokit.IOHIDElementGetUsagePage(element), iokit.IOHIDElementGetUsage(element),
                               iokit.IOHIDValueGetIntegerValue(val), iokit.IOHIDElementGetLogicalMin(element),
                               iokit.IOHIDElementGetLogicalMax(element)):
                self._emit()

        # exceptions must not escape a C callback; keep the ctypes wrappers alive for as long as the manager
        def guarded(fn):
            def wrapper(*args):
                try:
                    fn(*args)
                except Exception:  # noqa: BLE001
                    import traceback
                    traceback.print_exc()
            return _CB(wrapper)

        self._callbacks = (guarded(attached), guarded(removed), guarded(value))

        matching = NSArray.arrayWithArray_([
            {"DeviceUsagePage": PAGE_GENERIC_DESKTOP, "DeviceUsage": u} for u in (USAGE_JOYSTICK, USAGE_GAMEPAD)])
        mgr = iokit.IOHIDManagerCreate(None, 0)
        if not mgr:
            raise RuntimeError("IOHIDManagerCreate a échoué")
        iokit.IOHIDManagerSetDeviceMatchingMultiple(mgr, ref(matching))
        iokit.IOHIDManagerRegisterDeviceMatchingCallback(mgr, self._callbacks[0], None)
        iokit.IOHIDManagerRegisterDeviceRemovalCallback(mgr, self._callbacks[1], None)
        iokit.IOHIDManagerRegisterInputValueCallback(mgr, self._callbacks[2], None)
        iokit.IOHIDManagerScheduleWithRunLoop(mgr, cf.CFRunLoopGetCurrent(), modes)
        if iokit.IOHIDManagerOpen(mgr, 0) != 0:
            raise RuntimeError("accès aux manettes refusé (Surveillance de l'entrée)")
        self._manager = mgr

    def stop(self):
        if self._manager is not None:
            self._iokit.IOHIDManagerUnscheduleFromRunLoop(self._manager, self._cf.CFRunLoopGetCurrent(), self._modes)
            self._iokit.IOHIDManagerClose(self._manager, 0)
            self._manager = None
        self._pads.clear()
        self._emit()
