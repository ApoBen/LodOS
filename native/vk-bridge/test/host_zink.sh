#!/usr/bin/env bash
# T035: PC-host Mesa Zink (surfaceless EGL + GLES2) over libvulkan_lodos.so -> vkb_server -> host Intel ICD.
# No phone needed. Verifies eglinfo -B -p surfaceless + 256x256 red triangle readback, prints:
#   VKB zink triangle ok renderer=<str> center=0x.. corner=0x..
#   VKB zink calls=<N> eglinfo_calls=<M> gdpa_misses=<K>
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
SOCK=${LODOS_VK_SOCK:-@lodos-vk-zink}
OUT="$ROOT/build/vk-bridge/host-zink"
GEN="$OUT/gen"
rm -rf "$OUT"
mkdir -p "$GEN" "$OUT/cache"

echo "== codegen (cmds_icd.txt)"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")

echo "== build x86_64 vkb_server + libvulkan_lodos.so + host_zink"
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
    "$SRC/test/host_zink.c" -lEGL -lGLESv2 -o "$OUT/host_zink"

srv_pid=""
cleanup() {
    misses="$ROOT/build/vk-bridge/gdpa-misses.txt"
    mkdir -p "$(dirname "$misses")"
    cat "$OUT/eglinfo.err" "$OUT/zink.err" 2>/dev/null \
        | sed -n 's/^\[lodos-icd\] missing GDPA: //p' \
        | sort -u > "$misses"
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

ZINK_ENV=(
    env -u LIBGL_ALWAYS_SOFTWARE
    VK_DRIVER_FILES="$OUT/lodos_icd.json"
    LODOS_VK_SOCK="$SOCK"
    MESA_LOADER_DRIVER_OVERRIDE=zink
    EGL_PLATFORM=surfaceless
    MESA_SHADER_CACHE_DIR="$OUT/cache"
)

echo "== run eglinfo -B -p surfaceless"
"${ZINK_ENV[@]}" LODOS_VK_LOG_MISSING=0 eglinfo -B -p surfaceless >"$OUT/eglinfo.out" 2>"$OUT/eglinfo.err" || {
    cat "$OUT/eglinfo.out"
    cat "$OUT/eglinfo.err" >&2
    cat "$OUT/srv.log" >&2
    exit 1
}
cat "$OUT/eglinfo.out"
grep -Eq 'OpenGL (core|ES) profile renderer: .*zink.*Arc' "$OUT/eglinfo.out" || {
    echo "eglinfo renderer missing zink+Arc" >&2
    exit 1
}

for _ in $(seq 1 40); do
    [ "$(grep -c 'client done,' "$OUT/srv.log" 2>/dev/null || true)" -ge 1 ] && break
    sleep 0.05
done

echo "== run host_zink"
cli_out=$("${ZINK_ENV[@]}" "$OUT/host_zink" "$OUT/out.ppm" 2>"$OUT/zink.err") || {
    cat "$OUT/zink.err" >&2
    cat "$OUT/srv.log" >&2
    exit 1
}
cat "$OUT/zink.err" >&2

for _ in $(seq 1 40); do
    [ "$(grep -c 'client done,' "$OUT/srv.log" 2>/dev/null || true)" -ge 2 ] && break
    sleep 0.05
done

echo "-- server log"
cat "$OUT/srv.log"

test -s "$OUT/out.ppm" || { echo "missing or empty $OUT/out.ppm" >&2; exit 1; }
egl_calls=$(sed -n 's/.*client done, \([0-9][0-9]*\) calls.*/\1/p' "$OUT/srv.log" | head -1)
calls=$(sed -n 's/.*client done, \([0-9][0-9]*\) calls.*/\1/p' "$OUT/srv.log" | tail -1)
nmiss=$(grep -c '^\[lodos-icd\] missing GDPA:' "$OUT/zink.err" || true)
[ -n "$calls" ] || { echo "missing server calls count" >&2; exit 1; }

echo "$cli_out"
echo "VKB zink summary calls=$calls eglinfo_calls=${egl_calls:-0} gdpa_misses=$nmiss"
echo "GDPA miss names: $misses"
