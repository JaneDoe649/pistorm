# SPDX-License-Identifier: MIT
# Builds "PiStorm Remote.app" with py2app. Use ./build_app.sh rather than calling this directly.
from setuptools import setup

OPTIONS = {
    "iconfile": "PiStormRemote.icns",
    "includes": ["remote_core", "AppKit", "Foundation", "Quartz", "PyObjCTools.AppHelper"],
    "plist": {
        "CFBundleName": "PiStorm Remote",
        "CFBundleDisplayName": "PiStorm Remote",
        "CFBundleIdentifier": "org.pistorm.remote",
        "CFBundleVersion": "1.0",
        "CFBundleShortVersionString": "1.0",
        "LSMinimumSystemVersion": "10.13",
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "MIT",
    },
}

setup(
    app=["PiStormRemote.py"],
    name="PiStorm Remote",
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
)
