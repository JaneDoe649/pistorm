#!/bin/bash
# Builds "PiStorm Remote.app" (double-clickable from the Finder). macOS only.
#
#   ./build_app.sh            standalone application in dist/
#   ./build_app.sh --alias    development build that runs the sources in place (no copy, quick to iterate)
#   ./build_app.sh --install  also copies the application to /Applications
set -euo pipefail
cd "$(dirname "$0")"

if [ "$(uname)" != "Darwin" ]; then
  echo "This script must be run on macOS." >&2
  exit 1
fi

MODE=standalone
INSTALL=0
for arg in "$@"; do
  case "$arg" in
    --alias) MODE=alias ;;
    --install) INSTALL=1 ;;
    *) echo "Unknown option $arg" >&2; exit 2 ;;
  esac
done

APP="PiStorm Remote.app"

python3 -m venv .build-venv
# shellcheck disable=SC1091
. .build-venv/bin/activate
pip install --quiet --upgrade pip setuptools py2app pyobjc-framework-Cocoa pyobjc-framework-Quartz

# application icon: icon.png -> PiStormRemote.icns
[ -f icon.png ] || python3 make_icon.py icon.png
rm -rf PiStormRemote.iconset
mkdir PiStormRemote.iconset
for s in 16 32 128 256 512; do
  sips -z "$s" "$s" icon.png --out "PiStormRemote.iconset/icon_${s}x${s}.png" >/dev/null
  sips -z "$((s * 2))" "$((s * 2))" icon.png --out "PiStormRemote.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns PiStormRemote.iconset -o PiStormRemote.icns

rm -rf build dist
if [ "$MODE" = alias ]; then
  python3 setup.py py2app -A
else
  python3 setup.py py2app
fi

# ad hoc signature; required on Apple Silicon for the bundle to launch
codesign --force --deep --sign - "dist/$APP"

if [ "$INSTALL" = 1 ]; then
  rm -rf "/Applications/$APP"
  cp -R "dist/$APP" /Applications/
  echo "Installed in /Applications/$APP"
else
  echo "Built dist/$APP (drag it to /Applications, or re-run with --install)"
fi
echo "On first launch macOS asks for the Accessibility and Input Monitoring permissions: allow PiStorm Remote."
