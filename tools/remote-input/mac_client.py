#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Capture the Mac keyboard and mouse and send them to a PiStorm ("remoteinput" cfg option).
This is the command line client; PiStormRemote.py is the same thing with a window.

Setup (once):
    pip3 install pyobjc-framework-Quartz
    System Settings > Privacy & Security > Input Monitoring AND Accessibility: allow your terminal app.

Usage:
    # let the script open the SSH tunnel itself (ssh asks for the password in the terminal if needed)
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
import subprocess
import sys
import time

import remote_core as core


def open_link(args, on_lost):
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
            return core.Link.open(args.host, args.port, on_lost=on_lost)
        except core.LinkError as e:
            if tunnel and tunnel.poll() is not None:
                sys.exit("SSH tunnel exited (code %s)" % tunnel.returncode)
            if time.time() > deadline:
                sys.exit("%s" % e)
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssh", metavar="USER@HOST", help="open the SSH tunnel to this Raspberry Pi")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--start-released", action="store_true", help="don't grab the inputs at startup")
    args = ap.parse_args()

    if core.Q is None:
        sys.exit("Missing dependency: pip3 install pyobjc-framework-Quartz")
    Q = core.Q

    def stop_loop():
        Q.CFRunLoopStop(Q.CFRunLoopGetMain())

    def on_lost(reason):
        print("Connection lost (%s)" % reason)
        stop_loop()

    link = open_link(args, on_lost)

    def on_grab_changed(on):
        print("Inputs grabbed (Control+Option+G to release)" if on else "Inputs released")

    capture = core.Capture(link, on_grab_changed=on_grab_changed, on_quit=stop_loop)
    try:
        capture.install()
    except core.CaptureError as e:
        sys.exit(str(e))
    atexit.register(capture.uninstall)

    if not args.start_released:
        capture.set_grab(True)
    else:
        print("Started released (Control+Option+G to grab)")
    Q.CFRunLoopRun()


if __name__ == "__main__":
    main()
