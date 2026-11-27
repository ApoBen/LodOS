#!/usr/bin/env bash
# T022: build & stage liblodos_wl.so + libshmfill.so, installDebug APK, launch DisplayActivity,
# wait 5s, capture screenshot to /tmp/lodos-t022.png, verify centre pixel is solid green.
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh

SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
export ANDROID_SERIAL="$SERIAL"
A="adb -s $SERIAL"
PKG=org.lodos.app
PNG=/tmp/lodos-t022.png

# 1. Ensure native Wayland JNI lib + CLI bin + shm_fill test client are up-to-date
if [ ! -f build/xkbcommon/arm64/lib/libxkbcommon.a ]; then
  bash scripts/build-xkbcommon.sh
fi
if [ ! -f build/rs/arm64/liblodos_wl.so ] || [ ! -x build/rs/arm64/lodos-wl ] || \
   [ -n "$(find native/lodos-wl/src native/lodos-wl/Cargo.toml native/lodos-wl/build.rs -newer build/rs/arm64/liblodos_wl.so 2>/dev/null)" ]; then
  bash scripts/build-rs.sh native/lodos-wl
fi
if [ ! -x build/lodos-wl/arm64/shm_fill ] || [ native/lodos-wl/test/shm_fill.c -nt build/lodos-wl/arm64/shm_fill ]; then
  bash scripts/build-shm-fill.sh
fi

# 2. Stage jniLibs & installDebug via Gradle
bash scripts/stage-jnilibs.sh
if ! out=$( (cd android && ./gradlew --no-daemon --console=plain installDebug </dev/null) 2>&1); then
  echo "$out" | tail -20
  APK=android/app/build/outputs/apk/debug/app-debug.apk
  if [ -f "$APK" ] && $A install -r "$APK"; then
    echo "adb install -r fallback succeeded"
  else
    echo "NEEDS_USER_INSTALL $(realpath "$APK" 2>/dev/null || echo "$APK")"
    exit 2
  fi
fi

# 3. Start DisplayActivity, wait 5s, screencap
$A shell am force-stop "$PKG" || true
$A logcat -c
$A shell am start -W -n "$PKG/.DisplayActivity"
sleep 5
rm -f "$PNG"
$A exec-out screencap -p > "$PNG"

echo "== device logs:"
$A logcat -d -s LODOS_DISPLAY LODOS_WL SHM_FILL

$A shell am force-stop "$PKG" || true

# 4. Check centre pixel greenish (g>200, r<60, b<60)
python3 - "$PNG" <<'PY'
import struct, sys, zlib

path = sys.argv[1]
try:
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    r, g, b = im.getpixel((w // 2, h // 2))
except ImportError:
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    w = h = bit_depth = color_type = 0
    idat = bytearray()
    while pos < len(data):
        ln, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if ctype == b"IHDR":
            w, h, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    bpp = 4 if color_type == 6 else 3
    stride = 1 + w * bpp
    prev = bytearray(w * bpp)
    cy, cx = h // 2, w // 2
    for y in range(cy + 1):
        flt = raw[y * stride]
        row = bytearray(raw[y * stride + 1:(y + 1) * stride])
        if flt == 1:
            for i in range(bpp, len(row)):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif flt == 2:
            for i in range(len(row)):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif flt == 3:
            for i in range(len(row)):
                a = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif flt == 4:
            for i in range(len(row)):
                a = row[i - bpp] if i >= bpp else 0
                b0 = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b0 - c
                pa, pb, pc = abs(p - a), abs(p - b0), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b0 if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        prev = row
    r, g, b = prev[cx * bpp], prev[cx * bpp + 1], prev[cx * bpp + 2]

if g > 200 and r < 60 and b < 60:
    print(f"DISPLAY green ok ({w}x{h} centre=({w//2},{h//2}) rgb=({r},{g},{b}))")
    sys.exit(0)
else:
    print(f"DISPLAY fail rgb=({r},{g},{b}) size={w}x{h}")
    sys.exit(1)
PY
