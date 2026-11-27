#!/usr/bin/env bash
# T041: PC-host external semaphore/fence fd round-trip through libvulkan_lodos.so -> vk-bridge -> host Intel ICD.
# No phone needed. Prints: VKB syncfd ok sem=<type> fence=<type|skip> calls=<C>
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC="$ROOT/native/vk-bridge"
VKH="$ROOT/third_party/src/Vulkan-Headers"
TAG=v1.4.369
if [ ! -f "$VKH/registry/vk.xml" ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
fi

HOST_ICD=${HOST_ICD:-/usr/share/vulkan/icd.d/intel_icd.json}
SOCK=${LODOS_VK_SOCK:-@lodos-vk-syncfd}
OUT="$ROOT/build/vk-bridge/host-syncfd"
GEN="$OUT/gen"
rm -rf "$OUT"
mkdir -p "$GEN"

echo "== codegen (cmds_icd.txt)"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")

echo "== build x86_64 vkb_server + libvulkan_lodos.so + host_syncfd"
"$CC" "${CFLAGS[@]}" \
    "$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" \
    -ldl -lpthread -o "$OUT/vkb_server"

"$CC" "${CFLAGS[@]}" -fPIC -shared \
    -Wl,--version-script="$SRC/icd/icd.map" \
    "$SRC/icd/icd.c" "$SRC/client/vkbc.c" \
    "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_client.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" \
    -lpthread -o "$OUT/libvulkan_lodos.so"

cat > "$OUT/lodos_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$OUT/libvulkan_lodos.so",
        "api_version": "1.3"
    }
}
EOF

"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I "$VKH/include" "$SRC/test/host_syncfd.c" -lvulkan -o "$OUT/host_syncfd"

srv_pid=""
cleanup() {
    if [ -n "$srv_pid" ]; then
        kill "$srv_pid" 2>/dev/null || true
        wait "$srv_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "== start vkb_server (VK_DRIVER_FILES=$HOST_ICD, LODOS_VK_SOCK=$SOCK)"
VK_DRIVER_FILES="$HOST_ICD" LODOS_VK_SOCK="$SOCK" "$OUT/vkb_server" >"$OUT/srv.log" 2>&1 &
srv_pid=$!

for _ in $(seq 1 50); do
    grep -q 'listening' "$OUT/srv.log" 2>/dev/null && break
    sleep 0.05
done
grep -q 'listening' "$OUT/srv.log" || { cat "$OUT/srv.log"; exit 1; }

echo "== run host_syncfd (VK_DRIVER_FILES=$OUT/lodos_icd.json, LODOS_VK_SOCK=$SOCK)"
echo "-- pass 1: OPAQUE_FD (forced)"
opq_out=$(VK_DRIVER_FILES="$OUT/lodos_icd.json" LODOS_VK_SOCK="$SOCK" VKB_SYNCFD_OPAQUE=1 "$OUT/host_syncfd")
echo "$opq_out"
echo "$opq_out" | grep -q '^VKB syncfd ok sem=OPAQUE_FD' || { cat "$OUT/srv.log"; echo "OPAQUE_FD pass failed"; exit 1; }
echo "-- pass 2: preferred (SYNC_FD if host supports)"
cli_out=$(VK_DRIVER_FILES="$OUT/lodos_icd.json" LODOS_VK_SOCK="$SOCK" "$OUT/host_syncfd")
echo "$cli_out"

for _ in $(seq 1 40); do
    [ "$(grep -c 'client done,' "$OUT/srv.log" 2>/dev/null || true)" -ge 2 ] && break
    sleep 0.05
done

echo "-- server log"
cat "$OUT/srv.log"

calls=$(sed -n 's/.*client done, \([0-9][0-9]*\) calls.*/\1/p' "$OUT/srv.log" | tail -1)
line=$(printf '%s\n' "$cli_out" | grep '^VKB syncfd ok') || { echo "missing VKB syncfd ok"; exit 1; }
echo "$line calls=${calls:-?}"
