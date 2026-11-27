#!/usr/bin/env bash
# Build smoke.c (NDK), push it + libvulkan_lodos_mali.so to /data/local/tmp/lodos/vk/, run on phone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
source orch/env.sh
SER=${ANDROID_SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
LIB=build/vk-layer/arm64/libvulkan_lodos_mali.so
OUT=build/vk-layer/arm64/vklayer-smoke
D=/data/local/tmp/lodos/vk
[ -f "$LIB" ] || bash scripts/build-vk-layer.sh
aarch64-linux-android29-clang -O2 -Wall -o "$OUT" tools/vklayer-smoke/smoke.c -ldl
adb -s "$SER" shell mkdir -p "$D"
adb -s "$SER" push -q "$LIB" "$OUT" "$D/" >/dev/null
adb -s "$SER" shell "cd $D && chmod 755 vklayer-smoke && LD_LIBRARY_PATH=$D ./vklayer-smoke $D/libvulkan_lodos_mali.so"
