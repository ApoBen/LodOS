#!/usr/bin/env bash
# T029: PC-host e2e Vulkan compute through libvulkan_lodos.so -> vk-bridge -> host Intel ICD.
# No phone needed. Prints: VKB host compute ok n=<N> calls=<C>
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
SOCK=${LODOS_VK_SOCK:-@lodos-vk-test}
OUT="$ROOT/build/vk-bridge/host-compute"
GEN="$OUT/gen"
rm -rf "$OUT"
mkdir -p "$GEN"

echo "== codegen (cmds_icd.txt)"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")

echo "== build x86_64 vkb_server + libvulkan_lodos.so + host_compute"
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
    -I "$VKH/include" "$SRC/test/host_compute.c" -lvulkan -o "$OUT/host_compute"

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

echo "== run host_compute (VK_DRIVER_FILES=$OUT/lodos_icd.json, LODOS_VK_SOCK=$SOCK)"
cli_out=$(VK_DRIVER_FILES="$OUT/lodos_icd.json" LODOS_VK_SOCK="$SOCK" "$OUT/host_compute")
echo "$cli_out"

for _ in $(seq 1 40); do
    grep -q 'client done,' "$OUT/srv.log" 2>/dev/null && break
    sleep 0.05
done

echo "-- server log"
cat "$OUT/srv.log"

n=$(printf '%s\n' "$cli_out" | sed -n 's/.*VKB client compute verified n=\([0-9][0-9]*\).*/\1/p')
calls=$(sed -n 's/.*client done, \([0-9][0-9]*\) calls.*/\1/p' "$OUT/srv.log" | tail -1)
[ -n "$n" ] && [ -n "$calls" ] || { echo "missing n or calls"; exit 1; }
echo "VKB host compute ok n=$n calls=$calls"
