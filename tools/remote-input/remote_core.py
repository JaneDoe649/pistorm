# SPDX-License-Identifier: MIT
"""
Shared code of the PiStorm remote input clients (command line and GUI).

Nothing here needs Quartz at import time, so the non-UI parts can be imported and tested anywhere.
"""

import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import asdict, dataclass

try:
    import Quartz as Q
except ImportError:  # not on macOS, or PyObjC missing
    Q = None

# test hooks, the defaults are the macOS tools
SSH_BIN = os.environ.get("PISTORM_SSH_BIN", "/usr/bin/ssh")
SECURITY_BIN = os.environ.get("PISTORM_SECURITY_BIN", "/usr/bin/security")

APP_DIR_NAME = "PiStormRemote"
KEYCHAIN_SERVICE = "PiStorm Remote"
MOUSE_RATE_HZ = 60  # the Amiga samples the mouse once per frame (50 Hz), more is useless

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


# --------------------------------------------------------------------------------------------------
# Settings and Keychain
# --------------------------------------------------------------------------------------------------

def settings_path():
    base = os.path.expanduser("~/Library/Application Support")
    return os.path.join(base, APP_DIR_NAME, "settings.json")


@dataclass
class Settings:
    host: str = ""
    user: str = ""
    ssh_port: int = 22
    remote_port: int = 7777
    auto_capture: bool = True
    remember_password: bool = True

    def keychain_account(self):
        return "%s@%s:%d" % (self.user, self.host, self.ssh_port)

    @classmethod
    def load(cls, path=None):
        try:
            with open(path or settings_path(), "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return cls()
        fields = cls.__dataclass_fields__
        clean = {}
        for key, value in data.items() if isinstance(data, dict) else []:
            if key in fields and isinstance(value, type(fields[key].default)):
                clean[key] = value
        return cls(**clean)

    def save(self, path=None):
        # the password is NOT part of this file, it only lives in the Keychain
        path = path or settings_path()
        os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
        tmp = path + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(asdict(self), f, indent=2)
        os.replace(tmp, path)


class KeychainError(Exception):
    pass


def _security(args, timeout=10):
    try:
        return subprocess.run([SECURITY_BIN] + args, stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise KeychainError(str(e))


def keychain_get(account):
    """Returns the stored password, or None if there is none."""
    r = _security(["find-generic-password", "-s", KEYCHAIN_SERVICE, "-a", account, "-w"])
    if r.returncode == 0:
        return r.stdout.rstrip("\n")
    if r.returncode == 44:  # errSecItemNotFound
        return None
    raise KeychainError(r.stderr.strip() or "security exited with %d" % r.returncode)


def keychain_set(account, password):
    # n.b. `security` only accepts the secret as an argument; it is visible to the same user's `ps` for the
    # few milliseconds the command runs, which is why callers only write it when it actually changed
    r = _security(["add-generic-password", "-U", "-s", KEYCHAIN_SERVICE, "-a", account, "-w", password])
    if r.returncode != 0:
        raise KeychainError(r.stderr.strip() or "security exited with %d" % r.returncode)


def keychain_delete(account):
    r = _security(["delete-generic-password", "-s", KEYCHAIN_SERVICE, "-a", account])
    if r.returncode not in (0, 44):
        raise KeychainError(r.stderr.strip() or "security exited with %d" % r.returncode)


# --------------------------------------------------------------------------------------------------
# SSH tunnel
# --------------------------------------------------------------------------------------------------

class TunnelError(Exception):
    pass


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _friendly_ssh_error(err, rc):
    low = err.lower()
    if "permission denied" in low:
        return "Authentification refusée (utilisateur ou mot de passe incorrect)"
    if "host key verification failed" in low or "remote host identification has changed" in low:
        return "La clé de l'hôte a changé ou est refusée (voir ~/.ssh/known_hosts)"
    if "could not resolve" in low or "name or service not known" in low or "nodename nor servname" in low:
        return "Hôte introuvable"
    if "connection refused" in low:
        return "Connexion refusée (SSH est-il actif sur le Raspberry Pi ?)"
    if "timed out" in low or "no route to host" in low or "network is unreachable" in low:
        return "Hôte injoignable"
    if "address already in use" in low or "cannot listen" in low:
        return "Port local indisponible"
    lines = [l.strip() for l in err.splitlines() if l.strip()]
    return lines[-1] if lines else "ssh a échoué (code %d)" % rc


class SSHTunnel:
    """
    Opens `ssh -L <local>:127.0.0.1:<remote>` to the Pi with the system ssh, as a ControlMaster so that we know
    when it is up (-f only returns once the forward is established) and can close it cleanly (-O exit).
    The password, if any, is handed to ssh through SSH_ASKPASS, never on a command line.
    """

    def __init__(self, host, user, ssh_port, remote_port, password=None):
        self.host, self.user = host, user
        self.ssh_port, self.remote_port = ssh_port, remote_port
        self.password = password
        self.local_port = None
        self._dir = None
        self._ctl = None
        self._proc = None
        self._cancelled = False

    def _destination(self):
        return "%s@%s" % (self.user, self.host)

    def start(self, timeout=30):
        self._dir = tempfile.mkdtemp(prefix="pistorm-")  # mode 0700
        self._ctl = os.path.join(self._dir, "c")
        local = _free_port()

        env = dict(os.environ)
        cmd = [SSH_BIN, "-f", "-N", "-T", "-M", "-S", self._ctl,
               "-o", "ExitOnForwardFailure=yes",
               "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
               "-o", "ConnectTimeout=10",
               # trust on first use; a *changed* key is still refused
               "-o", "StrictHostKeyChecking=accept-new",
               "-o", "NumberOfPasswordPrompts=1",
               "-p", str(self.ssh_port),
               "-L", "%d:127.0.0.1:%d" % (local, self.remote_port)]
        if self.password:
            askpass = os.path.join(self._dir, "askpass.sh")
            fd = os.open(askpass, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o700)
            with os.fdopen(fd, "w") as f:
                f.write("#!/bin/sh\nprintf '%s\\n' \"$PISTORM_SSH_PASSWORD\"\n")
            env.update(SSH_ASKPASS=askpass, SSH_ASKPASS_REQUIRE="force",
                       PISTORM_SSH_PASSWORD=self.password, DISPLAY=env.get("DISPLAY") or ":0")
        else:
            cmd += ["-o", "BatchMode=yes"]  # keys / ssh-agent only, never hang on a prompt
        cmd.append(self._destination())

        with open(os.path.join(self._dir, "err"), "w+") as errf:
            try:
                # no controlling terminal (new session), so ssh can only ask through SSH_ASKPASS
                self._proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                              stderr=errf, env=env, start_new_session=True)
            except OSError as e:
                self.stop()
                raise TunnelError("Impossible de lancer ssh : %s" % e)
            try:
                rc = self._proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
                self.stop()
                raise TunnelError("Délai dépassé pendant la connexion SSH")
            errf.seek(0)
            err = errf.read()

        if self._cancelled:
            self.stop()
            raise TunnelError("Connexion annulée")
        if rc != 0:
            self.stop()
            raise TunnelError(_friendly_ssh_error(err, rc))
        self.local_port = local
        return local

    def cancel(self):
        self._cancelled = True
        proc = self._proc
        if proc is not None and proc.poll() is None:
            proc.kill()

    def stop(self):
        if self._ctl and os.path.exists(self._ctl):
            try:
                subprocess.run([SSH_BIN, "-S", self._ctl, "-O", "exit", "-p", str(self.ssh_port),
                                self._destination()], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if self._dir:
            shutil.rmtree(self._dir, ignore_errors=True)
            self._dir = self._ctl = None


# --------------------------------------------------------------------------------------------------
# Connection to the PiStorm remote input server
# --------------------------------------------------------------------------------------------------

class LinkError(Exception):
    pass


class Link:
    def __init__(self, sock, on_lost=None):
        self.sock = sock
        self.on_lost = on_lost
        self.lock = threading.Lock()
        self.dx = self.dy = 0
        self.buttons = 0
        self.joy = 0
        self.held = set()
        self.alive = True
        self._closing = False

    @classmethod
    def open(cls, host, port, on_lost=None, timeout=5):
        try:
            s = socket.create_connection((host, port), timeout=timeout)
        except OSError as e:
            raise LinkError("Connexion impossible à %s:%d (%s)" % (host, port, e))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.settimeout(3)
            s.sendall(b"P\n")
            data = s.recv(16)
        except OSError:
            data = b""
        if not data.startswith(b"PONG"):
            s.close()
            raise LinkError("Le serveur PiStorm ne répond pas (option « remoteinput » activée dans le .cfg ?)")
        s.settimeout(None)
        link = cls(s, on_lost)
        threading.Thread(target=link._reader, daemon=True).start()
        return link

    def _reader(self):
        reason = "connexion fermée"
        try:
            while self.sock.recv(256):
                pass
        except OSError as e:
            reason = str(e)
        self._lost(reason)

    def _lost(self, reason):
        was_alive, self.alive = self.alive, False
        if was_alive and not self._closing and self.on_lost:
            self.on_lost(reason)

    def send(self, line):
        try:
            with self.lock:
                self.sock.sendall((line + "\n").encode())
        except OSError as e:
            self._lost(str(e))

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
        self.set_joystick(0)

    def set_joystick(self, mask):
        """Amiga port 2 joystick state (see gamepad.py for the bits); only sent when it changes."""
        if mask != self.joy:
            self.joy = mask
            self.send("J %d" % mask)

    def add_motion(self, dx, dy):
        self.dx += dx
        self.dy += dy

    def flush_motion(self):
        dx, dy, self.dx, self.dy = self.dx, self.dy, 0, 0
        if dx or dy:
            self.send("M %d %d" % (max(-1000, min(1000, dx)), max(-1000, min(1000, dy))))

    def close(self):
        self._closing = True
        if self.alive:
            self.release_all()  # best effort; the server also releases everything when we disconnect
        self.alive = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


# --------------------------------------------------------------------------------------------------
# Keyboard and mouse capture (macOS only)
# --------------------------------------------------------------------------------------------------

class CaptureError(Exception):
    pass


class Capture:
    """
    Grabs the keyboard and mouse with a CGEventTap and forwards them through a Link. Must be created, used and
    uninstalled on the main thread (the tap is attached to the current run loop).

    Hotkeys, always active: Control+Option+G toggles the grab, Control+Option+Q calls on_quit.
    """

    def __init__(self, link, on_grab_changed=None, on_quit=None):
        self.link = link
        self.on_grab_changed = on_grab_changed
        self.on_quit = on_quit
        self.grabbed = False
        self.pad_mask = 0
        self._tap = None
        self._source = None
        self._runloop = None
        self._stop = threading.Event()

    def install(self):
        if Q is None:
            raise CaptureError("PyObjC (Quartz) n'est pas disponible")
        events = [Q.kCGEventKeyDown, Q.kCGEventKeyUp, Q.kCGEventFlagsChanged, Q.kCGEventMouseMoved,
                  Q.kCGEventLeftMouseDown, Q.kCGEventLeftMouseUp, Q.kCGEventLeftMouseDragged,
                  Q.kCGEventRightMouseDown, Q.kCGEventRightMouseUp, Q.kCGEventRightMouseDragged,
                  Q.kCGEventOtherMouseDown, Q.kCGEventOtherMouseUp, Q.kCGEventOtherMouseDragged,
                  Q.kCGEventScrollWheel]
        mask = 0
        for ev in events:
            mask |= Q.CGEventMaskBit(ev)

        self._button_bit = {Q.kCGEventLeftMouseDown: (1, True), Q.kCGEventLeftMouseUp: (1, False),
                            Q.kCGEventRightMouseDown: (2, True), Q.kCGEventRightMouseUp: (2, False)}
        self._tap = Q.CGEventTapCreate(Q.kCGSessionEventTap, Q.kCGHeadInsertEventTap,
                                       Q.kCGEventTapOptionDefault, mask, self._callback, None)
        if self._tap is None:
            if hasattr(Q, "CGRequestListenEventAccess"):
                Q.CGRequestListenEventAccess()
            raise CaptureError("Capture refusée : autorisez cette application dans Réglages Système > "
                               "Confidentialité et sécurité > Accessibilité et Surveillance de l'entrée, "
                               "puis relancez-la")
        self._runloop = Q.CFRunLoopGetCurrent()
        self._source = Q.CFMachPortCreateRunLoopSource(None, self._tap, 0)
        Q.CFRunLoopAddSource(self._runloop, self._source, Q.kCFRunLoopCommonModes)
        Q.CGEventTapEnable(self._tap, True)
        self._stop.clear()
        threading.Thread(target=self._motion_loop, daemon=True).start()

    def uninstall(self):
        self.set_grab(False)
        self._stop.set()
        if self._tap is not None:
            Q.CGEventTapEnable(self._tap, False)
            Q.CFRunLoopRemoveSource(self._runloop, self._source, Q.kCFRunLoopCommonModes)
            Q.CFMachPortInvalidate(self._tap)
            self._tap = self._source = None

    def set_grab(self, on):
        if on == self.grabbed or self._tap is None and on:
            return
        self.grabbed = on
        if on:
            Q.CGAssociateMouseAndMouseCursorPosition(False)
            Q.CGDisplayHideCursor(Q.CGMainDisplayID())
            self.link.set_joystick(self.pad_mask)
        else:
            self.link.release_all()
            Q.CGAssociateMouseAndMouseCursorPosition(True)
            Q.CGDisplayShowCursor(Q.CGMainDisplayID())
        if self.on_grab_changed:
            self.on_grab_changed(on)

    def set_pad_mask(self, mask):
        """Gamepad state. It is only forwarded to the Amiga while the inputs are grabbed."""
        self.pad_mask = mask
        if self.grabbed and self.link.alive:
            self.link.set_joystick(mask)

    def _motion_loop(self):
        while not self._stop.wait(1.0 / MOUSE_RATE_HZ):
            if self.grabbed and self.link.alive:
                self.link.flush_motion()

    def _set_button(self, bit, down):
        link = self.link
        link.buttons = (link.buttons | bit) if down else (link.buttons & ~bit)
        link.send("B %d" % link.buttons)

    def _callback(self, proxy, etype, event, refcon):
        if etype in (Q.kCGEventTapDisabledByTimeout, Q.kCGEventTapDisabledByUserInput):
            if self._tap is not None:
                Q.CGEventTapEnable(self._tap, True)
            return event

        link = self.link
        keycode = Q.CGEventGetIntegerValueField(event, Q.kCGKeyboardEventKeycode)
        flags = Q.CGEventGetFlags(event)

        # local hotkeys, always active
        if etype in (Q.kCGEventKeyDown, Q.kCGEventKeyUp) and keycode in (KEY_G, KEY_Q) \
                and (flags & Q.kCGEventFlagMaskControl) and (flags & Q.kCGEventFlagMaskAlternate):
            if etype == Q.kCGEventKeyDown:
                if keycode == KEY_G:
                    self.set_grab(not self.grabbed)
                elif self.on_quit:
                    self.on_quit()
            return None

        if not self.grabbed:
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
        elif etype in self._button_bit:
            bit, down = self._button_bit[etype]
            self._set_button(bit, down)
        elif etype in (Q.kCGEventOtherMouseDown, Q.kCGEventOtherMouseUp):
            if Q.CGEventGetIntegerValueField(event, Q.kCGMouseEventButtonNumber) == 2:
                self._set_button(4, etype == Q.kCGEventOtherMouseDown)
        # the scroll wheel is not forwarded (few Amiga programs use it) but it is still swallowed, so that it does not
        # scroll the Mac while the inputs are grabbed
        return None  # swallow: nothing reaches the Mac while grabbed
