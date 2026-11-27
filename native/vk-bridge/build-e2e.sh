#!/usr/bin/env bash
# T018: vk-bridge e2e v0. gen.py (rpc) -> NDK r30 aarch64 build (server: dynamic PIE, dlopens system
# libvulkan.so; client: static) -> adb push -> start server bg, run client, kill server, print outputs.
# HOST=1: also build+run on host against host libvulkan first.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/native/vk-bridge"
VKH="$ROOT/third_party/src/Vulkan-Headers"
TAG=v1.4.369
[ -f "$VKH/registry/vk.xml" ] || git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
OUT="$ROOT/build/vk-bridge/e2e"
GEN="$OUT/gen"
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
PDIR=/data/local/tmp/lodos/vkb-e2e
SOCK=${SOCK:-lodos-vk}
rm -rf "$GEN"; mkdir -p "$GEN"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_e2e.txt" "$GEN"

CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")
SRV=("$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" "$SRC/transport/ring.c" "$SRC/transport/sock.c")
CLI=("$SRC/test/e2e_test.c" "$SRC/client/vkbc.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_client.c" "$SRC/transport/ring.c" "$SRC/transport/sock.c")

if [ "${HOST:-0}" = 1 ]; then
    echo "== host build+run"
    ${CC:-gcc} "${CFLAGS[@]}" "${SRV[@]}" -ldl -lpthread -o "$OUT/vkb_server-host"
    ${CC:-gcc} "${CFLAGS[@]}" "${CLI[@]}" -o "$OUT/e2e_test-host"
    "$OUT/vkb_server-host" -s "$SOCK-host" & hp=$!
    "$OUT/e2e_test-host" "$SOCK-host" || { kill $hp; exit 1; }
    kill $hp; wait $hp 2>/dev/null || true
fi

echo "== android arm64 build (NDK r30)"
[ -n "${ANDROID_NDK:-}" ] || source "$ROOT/orch/env.sh"
ACC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
"$ACC" "${CFLAGS[@]}" -fPIE -pie "${SRV[@]}" -ldl -o "$OUT/vkb_server"
"$ACC" "${CFLAGS[@]}" -static "${CLI[@]}" -o "$OUT/e2e_test"

echo "== phone run"
adb -s "$SERIAL" shell "mkdir -p $PDIR"
adb -s "$SERIAL" push -q "$OUT/vkb_server" "$OUT/e2e_test" "$PDIR/" >/dev/null
adb -s "$SERIAL" shell "cd $PDIR && chmod 755 vkb_server e2e_test && \
    (./vkb_server -s $SOCK >srv.log 2>&1 & echo \$! >srv.pid) && \
    ./e2e_test $SOCK; rc=\$?; kill \$(cat srv.pid) 2>/dev/null; sleep 0.2; echo '-- server log'; cat srv.log; rm -f srv.pid; exit \$rc"
