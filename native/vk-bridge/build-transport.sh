#!/usr/bin/env bash
# T015: build vk-bridge transport test for host (gcc) + android arm64 (NDK clang), run both.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/native/vk-bridge"
OUT="$ROOT/build/vk-bridge/transport"
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
PDIR=/data/local/tmp/lodos
mkdir -p "$OUT"
FILES=("$SRC/transport/ring.c" "$SRC/transport/sock.c" "$SRC/test/transport_test.c")
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror)

echo "== host build"
${CC:-gcc} "${CFLAGS[@]}" "${FILES[@]}" -o "$OUT/transport_test-host"
echo "== host run"
"$OUT/transport_test-host"

echo "== android arm64 build"
[ -n "${ANDROID_NDK:-}" ] || source "$ROOT/orch/env.sh"
ACC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
"$ACC" "${CFLAGS[@]}" -static "${FILES[@]}" -o "$OUT/transport_test-arm64"
echo "== phone run"
adb -s "$SERIAL" shell "mkdir -p $PDIR"
adb -s "$SERIAL" push -q "$OUT/transport_test-arm64" "$PDIR/transport_test" >/dev/null
adb -s "$SERIAL" shell "chmod 755 $PDIR/transport_test && $PDIR/transport_test"
