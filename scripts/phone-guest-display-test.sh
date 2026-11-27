#!/usr/bin/env bash
# T028: run Debian's wl_shm demo through the app-uid proot guest and capture the SurfaceView.
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh

SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
export ANDROID_SERIAL="$SERIAL"
A="adb -s $SERIAL"
PKG=org.lodos.app
PNG=/tmp/lodos-t028.png

# Install the existing app-uid Debian rootfs if this device has not completed T020 yet.
if ! $A shell "run-as $PKG test -f files/rootfs/etc/debian_version" 2>/dev/null; then
  bash scripts/phone-app-rootfs.sh
fi

if [ ! -f build/xkbcommon/arm64/lib/libxkbcommon.a ]; then bash scripts/build-xkbcommon.sh; fi
if [ ! -f build/rs/arm64/liblodos_wl.so ] || [ -n "$(find native/lodos-wl/src native/lodos-wl/Cargo.toml native/lodos-wl/build.rs -newer build/rs/arm64/liblodos_wl.so 2>/dev/null)" ]; then
  bash scripts/build-rs.sh native/lodos-wl
fi
if [ ! -x build/lodos-wl/arm64/shm_fill ]; then bash scripts/build-shm-fill.sh; fi
bash scripts/stage-jnilibs.sh
if ! out=$((cd android && ./gradlew --no-daemon --console=plain installDebug </dev/null) 2>&1); then
  echo "$out" | tail -20
  APK=android/app/build/outputs/apk/debug/app-debug.apk
  $A install -r "$APK"
fi

$A shell am force-stop "$PKG" || true
$A logcat -c
$A shell am start -W -n "$PKG/.DisplayActivity" --es client guest
# First run may apt-install Weston in the app's private rootfs. Wait for repeated
# native presentations, or for the guest process to finish, before capturing.
for _ in $(seq 1 180); do
  seen=$($A logcat -d -s LODOS_WL | sed -nE 's/.*present #([0-9]+).*/\1/p' | sort -n | tail -1)
  if [ "${seen:-0}" -ge 2 ]; then break; fi
  sleep 2
done
rm -f "$PNG"
$A exec-out screencap -p > "$PNG"
$A logcat -d -s LODOS_DISPLAY LODOS_WL

frames=$($A logcat -d -s LODOS_WL | sed -nE 's/.*present #([0-9]+).*/\1/p' | sort -n | tail -1)
frames=${frames:-0}
python3 - "$PNG" <<'PY'
import sys
try:
    from PIL import Image
except ImportError:
    raise SystemExit("Pillow is required to inspect the guest display capture")
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
# Inspect a central patch: simple-shm animates a checker/pattern rather than a flat fill.
box = im.crop((w * 2 // 5, h * 2 // 5, w * 3 // 5, h * 3 // 5))
colors = set(box.getdata())
nonblack = sum(1 for p in box.getdata() if max(p) > 24)
if len(colors) < 2 or nonblack < box.width * box.height // 4:
    print(f"GUEST DISPLAY fail size={w}x{h} colors={len(colors)} nonblack={nonblack}")
    raise SystemExit(1)
print(f"GUEST DISPLAY pixels ok size={w}x{h} colors={len(colors)}")
PY
if [ "$frames" -lt 2 ]; then echo "GUEST DISPLAY fail frames=$frames"; exit 1; fi
echo "GUEST DISPLAY ok client=weston-simple-shm frames=$frames"
$A shell am force-stop "$PKG" || true
