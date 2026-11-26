#!/usr/bin/env bash
# Build static libxkbcommon for Android arm64 (needed by smithay wl_seat -> xkbcommon crate).
# Out: build/xkbcommon/arm64/lib/libxkbcommon.a
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source orch/env.sh
SRC=$ROOT/third_party/src/libxkbcommon
TAG=xkbcommon-1.11.0
COMMIT=0f9cefb1fedb23433666fa5d9045e48ff030c006
B=$ROOT/build/xkbcommon
OUT=$B/arm64
[ -f "$OUT/lib/libxkbcommon.a" ] && { echo "-> $OUT/lib/libxkbcommon.a (cached)"; exit 0; }
[ -d "$SRC/.git" ] || git clone -q --depth 1 --branch "$TAG" https://github.com/xkbcommon/libxkbcommon.git "$SRC"
[ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo "xkbcommon commit mismatch" >&2; exit 1; }
rm -rf "$B/meson"
meson setup "$B/meson" "$SRC" --cross-file scripts/cross/android-arm64.ini \
  --prefix="$OUT" --libdir=lib --default-library=static --buildtype=release \
  -Denable-tools=false -Denable-x11=false -Denable-docs=false -Denable-wayland=false \
  -Denable-xkbregistry=false -Denable-bash-completion=false \
  -Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale
ninja -C "$B/meson" install >/dev/null
echo "-> $OUT/lib/libxkbcommon.a"
