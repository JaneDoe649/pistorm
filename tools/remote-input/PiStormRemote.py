#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
PiStorm Remote: a small window to control an Amiga running PiStorm from this Mac.

It opens the SSH tunnel to the Raspberry Pi, then captures the keyboard and mouse (see remote_core.py).
Settings are saved in ~/Library/Application Support/PiStormRemote/settings.json, the password in the Keychain.

Run it with `python3 PiStormRemote.py`, or build the double-clickable application with ./build_app.sh.
"""

import threading

from AppKit import (NSApplication, NSButton, NSColor, NSFont, NSMakeRect, NSMenu, NSMenuItem,
                    NSSecureTextField, NSTextField, NSWindow)
from Foundation import NSObject
from PyObjCTools import AppHelper

import remote_core as core

WIN_W, WIN_H = 380, 360
LABEL_X, LABEL_W = 20, 110
FIELD_X, FIELD_W = 140, 220

# states
IDLE, CONNECTING, CONNECTED, CAPTURING, ERROR = "idle", "connecting", "connected", "capturing", "error"

DOT_COLORS = {
    IDLE: NSColor.systemGrayColor,
    CONNECTING: NSColor.systemOrangeColor,
    CONNECTED: NSColor.systemBlueColor,
    CAPTURING: NSColor.systemGreenColor,
    ERROR: NSColor.systemRedColor,
}


class Controller:
    """Connection logic, independent from the widgets. Everything here runs on the main thread, except workers."""

    def __init__(self, view):
        self.view = view
        self.state = IDLE
        self.settings = None
        self.tunnel = self.link = self.capture = None
        self._gen = 0  # incremented on every cancel / disconnect so that late callbacks can be ignored

    # ---- user actions
    def connect(self, settings, password):
        if self.state not in (IDLE, ERROR):
            return
        self.settings = settings
        self._gen += 1
        gen = self._gen
        self._set(CONNECTING, "Connexion à %s@%s…" % (settings.user, settings.host))
        tunnel = core.SSHTunnel(settings.host, settings.user, settings.ssh_port, settings.remote_port, password)
        self.tunnel = tunnel
        threading.Thread(target=self._connect_worker, args=(gen, tunnel), daemon=True).start()

    def cancel(self):
        if self.state == CONNECTING and self.tunnel:
            self._gen += 1
            self.tunnel.cancel()
            self.tunnel = None
            self._set(IDLE, "Connexion annulée")

    def disconnect(self):
        self._gen += 1
        self._teardown()
        self._set(IDLE, "Déconnecté")

    def toggle_grab(self):
        if self.capture and self.state in (CONNECTED, CAPTURING):
            self.view.release_focus()
            self.capture.set_grab(not self.capture.grabbed)

    def shutdown(self):
        self._gen += 1
        self._teardown()

    # ---- worker thread
    def _connect_worker(self, gen, tunnel):
        link = None
        try:
            local_port = tunnel.start()
            link = core.Link.open("127.0.0.1", local_port,
                                  on_lost=lambda why: AppHelper.callAfter(self._lost, gen, why))
        except (core.TunnelError, core.LinkError) as e:
            tunnel.stop()
            AppHelper.callAfter(self._failed, gen, str(e))
            return
        AppHelper.callAfter(self._connected, gen, tunnel, link)

    # ---- callbacks, main thread
    def _failed(self, gen, message):
        if gen != self._gen:
            return
        self.tunnel = None
        self._set(ERROR, message)

    def _connected(self, gen, tunnel, link):
        if gen != self._gen:  # cancelled while connecting
            link.close()
            tunnel.stop()
            return
        self.link = link
        self.capture = core.Capture(link, on_grab_changed=self._grab_changed,
                                    on_quit=lambda: AppHelper.callAfter(self.disconnect))
        try:
            self.capture.install()
        except core.CaptureError as e:
            self._teardown()
            self._set(ERROR, str(e))
            return
        self._set(CONNECTED, "Connecté, capture inactive")
        if self.settings.auto_capture:
            self.view.release_focus()  # an active password field would block the keyboard capture
            self.capture.set_grab(True)

    def _grab_changed(self, on):
        if self.state in (CONNECTED, CAPTURING):
            if on:
                self._set(CAPTURING, "Capture active. Ctrl+Option+G pour libérer")
            else:
                self._set(CONNECTED, "Connecté, capture inactive")

    def _lost(self, gen, why):
        if gen != self._gen:
            return
        self._gen += 1
        self._teardown()
        self._set(ERROR, "Connexion perdue (%s)" % why)

    # ---- helpers
    def _teardown(self):
        capture, link, tunnel = self.capture, self.link, self.tunnel
        self.capture = self.link = self.tunnel = None
        if capture:
            capture.uninstall()
        if link:
            link.close()
        if tunnel:
            tunnel.stop()

    def _set(self, state, text):
        self.state = state
        self.view.render(state, text)


class MainWindow:
    """The window and everything it shows. A plain Python class on purpose: methods of an NSObject subclass are
    turned into Objective-C selectors by PyObjC, which breaks multi-argument helpers."""

    def __init__(self, target):
        self.target = target  # NSObject receiving the button actions
        self.controller = Controller(self)
        self.loaded_account = None
        self.keychain_pw = None
        self._build_menu()
        self._build_window()
        self._load_settings()
        self.render(IDLE, "Déconnecté")
        NSApplication.sharedApplication().activateIgnoringOtherApps_(True)

    # ---- UI construction
    def _build_menu(self):
        app = NSApplication.sharedApplication()
        main = NSMenu.alloc().init()

        def submenu(title, items):
            holder = NSMenuItem.alloc().init()
            menu = NSMenu.alloc().initWithTitle_(title)
            for item_title, action, key in items:
                menu.addItem_(NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(item_title, action, key))
            holder.setSubmenu_(menu)
            main.addItem_(holder)

        submenu("PiStorm Remote", [("Quitter PiStorm Remote", "terminate:", "q")])
        # without an Edit menu there is no Cmd+C / Cmd+V in the text fields
        submenu("Édition", [("Couper", "cut:", "x"), ("Copier", "copy:", "c"),
                            ("Coller", "paste:", "v"), ("Tout sélectionner", "selectAll:", "a")])
        app.setMainMenu_(main)

    def _label(self, text, x, y, w, h=20, bold=False):
        t = NSTextField.alloc().initWithFrame_(NSMakeRect(x, y, w, h))
        t.setStringValue_(text)
        t.setBezeled_(False)
        t.setDrawsBackground_(False)
        t.setEditable_(False)
        t.setSelectable_(False)
        if bold:
            t.setFont_(NSFont.boldSystemFontOfSize_(13))
        self.win.contentView().addSubview_(t)
        return t

    def _field(self, y, secure=False, placeholder=""):
        cls = NSSecureTextField if secure else NSTextField
        f = cls.alloc().initWithFrame_(NSMakeRect(FIELD_X, y, FIELD_W, 24))
        f.setPlaceholderString_(placeholder)
        self.win.contentView().addSubview_(f)
        return f

    def _button(self, title, x, y, w, action, checkbox=False):
        b = NSButton.alloc().initWithFrame_(NSMakeRect(x, y, w, 28 if not checkbox else 22))
        b.setTitle_(title)
        if checkbox:
            b.setButtonType_(3)  # NSButtonTypeSwitch
        else:
            b.setBezelStyle_(1)  # NSBezelStyleRounded
        if action:
            b.setTarget_(self.target)
            b.setAction_(action)
        self.win.contentView().addSubview_(b)
        return b

    def _build_window(self):
        # 1 titled | 2 closable | 4 miniaturizable
        self.win = NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(
            NSMakeRect(0, 0, WIN_W, WIN_H), 1 | 2 | 4, 2, False)
        self.win.setTitle_("PiStorm Remote")
        self.win.setReleasedWhenClosed_(False)

        def row_y(i):
            return WIN_H - 52 - i * 34

        self._label("Raspberry Pi", LABEL_X, WIN_H - 28, 200, bold=True)
        rows = [("Hôte", False, "raspberrypi.local"), ("Utilisateur", False, "pi"),
                ("Port SSH", False, "22"), ("Port PiStorm", False, "7777"), ("Mot de passe", True, "")]
        self.fields = []
        for i, (title, secure, hint) in enumerate(rows):
            self._label(title, LABEL_X, row_y(i) + 2, LABEL_W)
            self.fields.append(self._field(row_y(i), secure, hint))
        self.host_f, self.user_f, self.sshport_f, self.port_f, self.pw_f = self.fields

        self.remember_b = self._button("Mémoriser le mot de passe (Trousseau)", FIELD_X - 4, row_y(5) + 2, 250,
                                       None, checkbox=True)
        self.auto_b = self._button("Capturer dès la connexion", FIELD_X - 4, row_y(6) + 2, 250, None,
                                   checkbox=True)

        self.connect_b = self._button("Connecter", 20, 64, 165, "connectClicked:")
        self.connect_b.setKeyEquivalent_("\r")
        self.grab_b = self._button("Capturer", 195, 64, 165, "grabClicked:")

        self.dot = self._label("●", 20, 22, 22, 28)
        self.dot.setFont_(NSFont.systemFontOfSize_(22))
        self.status = self._label("", 46, 8, WIN_W - 66, 50)
        self.status.setFont_(NSFont.systemFontOfSize_(13))
        self.status.cell().setWraps_(True)
        self.status.setUsesSingleLineMode_(False)

        self.win.center()
        self.win.makeKeyAndOrderFront_(None)

    # ---- settings
    def _load_settings(self):
        s = core.Settings.load()
        self.host_f.setStringValue_(s.host)
        self.user_f.setStringValue_(s.user)
        self.sshport_f.setStringValue_(str(s.ssh_port))
        self.port_f.setStringValue_(str(s.remote_port))
        self.remember_b.setState_(1 if s.remember_password else 0)
        self.auto_b.setState_(1 if s.auto_capture else 0)
        if s.remember_password and s.host and s.user:
            self.loaded_account = s.keychain_account()
            try:
                self.keychain_pw = core.keychain_get(self.loaded_account)
            except core.KeychainError:
                self.keychain_pw = None
            if self.keychain_pw:
                self.pw_f.setStringValue_(self.keychain_pw)

    def _read_settings(self):
        """Returns (settings, error message)."""
        def port(field, name):
            try:
                n = int(str(field.stringValue()).strip())
            except ValueError:
                n = 0
            if not 1 <= n <= 65535:
                raise ValueError("%s invalide" % name)
            return n
        host = str(self.host_f.stringValue()).strip()
        user = str(self.user_f.stringValue()).strip()
        try:
            if not host:
                raise ValueError("L'hôte est obligatoire")
            if not user:
                raise ValueError("L'utilisateur est obligatoire")
            s = core.Settings(host=host, user=user, ssh_port=port(self.sshport_f, "Port SSH"),
                              remote_port=port(self.port_f, "Port PiStorm"),
                              auto_capture=self.auto_b.state() == 1,
                              remember_password=self.remember_b.state() == 1)
        except ValueError as e:
            return None, str(e)
        return s, None

    def _persist(self, s, password):
        """Saves the settings; the password goes to the Keychain (or is removed from it)."""
        notes = []
        try:
            s.save()
        except OSError as e:
            notes.append("Réglages non enregistrés (%s)" % e)
        account = s.keychain_account()
        try:
            if self.loaded_account and self.loaded_account != account:
                core.keychain_delete(self.loaded_account)  # host or user changed: drop the old entry
                self.keychain_pw = None
            if s.remember_password and password:
                if password != self.keychain_pw:  # only write when it changed
                    core.keychain_set(account, password)
                    self.keychain_pw = password
            elif not s.remember_password:
                core.keychain_delete(account)
                self.keychain_pw = None
        except core.KeychainError as e:
            notes.append("Trousseau : %s" % e)
        self.loaded_account = account if s.remember_password else None
        return notes

    # ---- actions
    def on_connect(self):
        state = self.controller.state
        if state == CONNECTING:
            self.controller.cancel()
            return
        if state in (CONNECTED, CAPTURING):
            self.controller.disconnect()
            return
        s, error = self._read_settings()
        if error:
            self.render(ERROR, error)
            return
        password = str(self.pw_f.stringValue())
        self._persist(s, password)
        self.controller.connect(s, password or None)

    def release_focus(self):
        # leaving the secure password field matters: while it is being edited, macOS blocks event taps
        self.win.makeFirstResponder_(None)

    # ---- view
    def render(self, state, text):
        self.dot.setTextColor_(DOT_COLORS[state]())
        self.status.setStringValue_(text)
        editable = state in (IDLE, ERROR)
        for f in self.fields:
            f.setEnabled_(editable)
        self.remember_b.setEnabled_(editable)
        self.auto_b.setEnabled_(editable)
        self.connect_b.setTitle_({CONNECTING: "Annuler", CONNECTED: "Déconnecter",
                                  CAPTURING: "Déconnecter"}.get(state, "Connecter"))
        self.grab_b.setEnabled_(state in (CONNECTED, CAPTURING))
        self.grab_b.setTitle_("Libérer" if state == CAPTURING else "Capturer")
        self.win.setTitle_("PiStorm Remote, capture active" if state == CAPTURING else "PiStorm Remote")


class AppDelegate(NSObject):
    def applicationDidFinishLaunching_(self, notification):
        self.ui = MainWindow(self)

    def applicationShouldTerminateAfterLastWindowClosed_(self, app):
        return True

    def applicationWillTerminate_(self, notification):
        self.ui.controller.shutdown()

    def connectClicked_(self, sender):
        self.ui.on_connect()

    def grabClicked_(self, sender):
        self.ui.controller.toggle_grab()


def main():
    app = NSApplication.sharedApplication()
    delegate = AppDelegate.alloc().init()
    app.setDelegate_(delegate)
    app.setActivationPolicy_(0)  # NSApplicationActivationPolicyRegular
    AppHelper.runEventLoop()


if __name__ == "__main__":
    main()
