#!/usr/bin/env bash
# T032: PC-host e2e offscreen triangle through libvulkan_lodos.so -> vk-bridge -> host Intel ICD.
# No phone needed. Writes build/vk-bridge/host-triangle/out.ppm and prints:
#   VKB host triangle ok center=<rgba> corner=<rgba> calls=<n>
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
SOCK=${LODOS_VK_SOCK:-@lodos-vk-tri}
OUT="$ROOT/build/vk-bridge/host-triangle"
GEN="$OUT/gen"
rm -rf "$OUT"
mkdir -p "$GEN"

echo "== codegen (cmds_icd.txt)"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"

SPV_DEFS=()
if command -v glslangValidator >/dev/null 2>&1; then
    echo "== compile SPIR-V via glslangValidator"
    cat > "$OUT/tri.vert" <<'EOF'
#version 450
void main() {
    const vec2 pos[3] = vec2[3](
        vec2( 0.0, -0.6),
        vec2( 0.6,  0.6),
        vec2(-0.6,  0.6)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
EOF
    cat > "$OUT/tri.frag" <<'EOF'
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
}
EOF
    glslangValidator -V --vn k_vert_spv "$OUT/tri.vert" -o "$OUT/vert_spv.h" >/dev/null
    glslangValidator -V --vn k_frag_spv "$OUT/tri.frag" -o "$OUT/frag_spv.h" >/dev/null
    SPV_DEFS=(-DLODOS_TRIANGLE_VERT_SPV="\"$OUT/vert_spv.h\"" -DLODOS_TRIANGLE_FRAG_SPV="\"$OUT/frag_spv.h\"")
else
    echo "== glslangValidator not found; using checked-in SPIR-V u32 arrays"
fi

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")

echo "== build x86_64 vkb_server + libvulkan_lodos.so + host_triangle"
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

"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror "${SPV_DEFS[@]}" \
    -I "$VKH/include" "$SRC/test/host_triangle.c" -lvulkan -o "$OUT/host_triangle"

srv_pid=""
cleanup() {
    if [ -n "$srv_pid" ]; then
        kill "$srv_pid" 2>/dev/null || true
        wait "$srv_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

run_mode() {
    mode=$1; no_batch=$2
    echo "== start vkb_server ($mode, VK_DRIVER_FILES=$HOST_ICD, LODOS_VK_SOCK=$SOCK)" >&2
    VK_DRIVER_FILES="$HOST_ICD" LODOS_VK_SOCK="$SOCK" "$OUT/vkb_server" >"$OUT/srv-$mode.log" 2>&1 &
    srv_pid=$!
    for _ in $(seq 1 50); do
        grep -q 'listening' "$OUT/srv-$mode.log" 2>/dev/null && break
        sleep 0.05
    done
    grep -q 'listening' "$OUT/srv-$mode.log" || { cat "$OUT/srv-$mode.log"; exit 1; }
    cli_out=$(VKB_NO_BATCH="$no_batch" VK_DRIVER_FILES="$OUT/lodos_icd.json" LODOS_VK_SOCK="$SOCK" "$OUT/host_triangle" "$OUT/out.ppm")
    echo "$cli_out" >&2
    for _ in $(seq 1 40); do
        grep -q 'client done,' "$OUT/srv-$mode.log" 2>/dev/null && break
        sleep 0.05
    done
    kill "$srv_pid" 2>/dev/null || true
    wait "$srv_pid" 2>/dev/null || true
    srv_pid=""
    test -s "$OUT/out.ppm" || { echo "missing or empty $OUT/out.ppm"; exit 1; }
    center=$(printf '%s\n' "$cli_out" | sed -n 's/.*center=\([^ ]*\).*/\1/p')
    corner=$(printf '%s\n' "$cli_out" | sed -n 's/.*corner=\([^ ]*\).*/\1/p')
    calls=$(sed -n 's/.*client done, \([0-9][0-9]*\) calls.*/\1/p' "$OUT/srv-$mode.log" | tail -1)
    [ -n "$center" ] && [ -n "$corner" ] && [ -n "$calls" ] || { cat "$OUT/srv-$mode.log"; echo "missing center/corner/calls"; exit 1; }
    echo "VKB host triangle ok center=$center corner=$corner calls=$calls" >&2
    printf '%s' "$calls"
}

batched=$(run_mode batched 0)
unbatched=$(run_mode unbatched 1)
echo "VKB batch calls batched=$batched unbatched=$unbatched"
