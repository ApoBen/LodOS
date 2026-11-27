#!/usr/bin/env bash
# T047: PC-host guest-side WSI. vkcube --wsi wayland -> libvulkan_lodos.so (VK_KHR_wayland_surface + swapchain
# implemented in the ICD: bridged image -> readback -> wl_shm) -> vkb_server -> host Intel ICD,
# presented on lodos-wl headless; PPM dumped after N commits -> docs/screens/T047-wsi.png.
# No phone needed. Prints: VKB wsi ok frames=<presents> / nonblack=<pixels>
# env: WSI_FRAMES (vkcube frames, 120), WSI_DUMP_AFTER (compositor commit to dump at, 60)
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC="$ROOT/native/vk-bridge"
VKH="$ROOT/third_party/src/Vulkan-Headers"
TAG=v1.4.369
if [ ! -f "$VKH/registry/vk.xml" ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
fi
command -v vkcube >/dev/null || { echo "vkcube missing (install vulkan-tools)" >&2; exit 1; }

HOST_ICD=${HOST_ICD:-/usr/share/vulkan/icd.d/intel_icd.json}
SOCK=${LODOS_VK_SOCK:-@lodos-vk-wsi}
FRAMES=${WSI_FRAMES:-120}
DUMP_AFTER=${WSI_DUMP_AFTER:-60}
W=640; H=480
OUT="$ROOT/build/vk-bridge/host-wsi"
GEN="$OUT/gen"
PNG="$ROOT/docs/screens/T047-wsi.png"
rm -rf "$OUT"
mkdir -p "$GEN" "$ROOT/docs/screens"

echo "== codegen (cmds_icd.txt)"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")

echo "== build x86_64 vkb_server + libvulkan_lodos.so (+wsi_wayland.c)"
"$CC" "${CFLAGS[@]}" \
    "$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" \
    -ldl -lpthread -o "$OUT/vkb_server"
"$CC" "${CFLAGS[@]}" -fPIC -shared \
    -Wl,--version-script="$SRC/icd/icd.map" \
    "$SRC/icd/icd.c" "$SRC/client/vkbc.c" \
    "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_client.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" \
    -lpthread -ldl -o "$OUT/libvulkan_lodos.so"
cat > "$OUT/lodos_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$OUT/libvulkan_lodos.so",
        "api_version": "1.3"
    }
}
EOF

echo "== build lodos-wl (headless)"
(cd "$ROOT/native/lodos-wl" && cargo build --release --features headless -q)
WL="$ROOT/native/lodos-wl/target/release/lodos-wl"

RUN=$(mktemp -d "${TMPDIR:-/tmp}/lodos-wsi.XXXXXX")
WLSOCK=lodos-wl-wsi
srv_pid=""; wl_pid=""
cleanup() {
    for p in $srv_pid $wl_pid; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    rm -rf "$RUN"
}
trap cleanup EXIT

echo "== start vkb_server (VK_DRIVER_FILES=$HOST_ICD, LODOS_VK_SOCK=$SOCK)"
VK_DRIVER_FILES="$HOST_ICD" LODOS_VK_SOCK="$SOCK" "$OUT/vkb_server" >"$OUT/srv.log" 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do grep -q 'listening' "$OUT/srv.log" 2>/dev/null && break; sleep 0.05; done
grep -q 'listening' "$OUT/srv.log" || { cat "$OUT/srv.log"; exit 1; }

echo "== start lodos-wl headless ${W}x${H} (dump after $DUMP_AFTER commits)"
LODOS_WL_WIDTH=$W LODOS_WL_HEIGHT=$H LODOS_WL_DUMP="$OUT/fb.ppm" LODOS_WL_DUMP_AFTER=$DUMP_AFTER \
    "$WL" "$RUN/$WLSOCK" >"$OUT/lodos-wl.log" 2>&1 &
wl_pid=$!
for _ in $(seq 1 50); do [ -S "$RUN/$WLSOCK" ] && break; sleep 0.1; done
[ -S "$RUN/$WLSOCK" ] || { cat "$OUT/lodos-wl.log" >&2; exit 1; }

echo "== run vkcube --wsi wayland --c $FRAMES via lodos ICD"
set +e
env -u DISPLAY -u WAYLAND_SOCKET XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=$WLSOCK \
    VK_DRIVER_FILES="$OUT/lodos_icd.json" VK_LOADER_LAYERS_DISABLE='*' LODOS_VK_SOCK="$SOCK" LODOS_VK_WSI_STATS=1 LODOS_VK_LOG_MISSING=0 \
    timeout 300 vkcube --wsi wayland --c "$FRAMES" --width 512 --height 512 --suppress_popups \
    >"$OUT/vkcube.out" 2>"$OUT/vkcube.err"
rc=$?
set -e
cat "$OUT/vkcube.out"; cat "$OUT/vkcube.err" >&2
[ $rc -eq 0 ] || { echo "vkcube rc=$rc" >&2; tail -20 "$OUT/srv.log" >&2; cat "$OUT/lodos-wl.log" >&2; exit 1; }

for _ in $(seq 1 40); do [ -s "$OUT/fb.ppm" ] && break; sleep 0.05; done
echo "-- lodos-wl log"; tail -5 "$OUT/lodos-wl.log"
test -s "$OUT/fb.ppm" || { echo "no PPM dump (commits < $DUMP_AFTER?)" >&2; exit 1; }
frames=$(sed -n 's/.*wsi presents=\([0-9][0-9]*\).*/\1/p' "$OUT/vkcube.err" | tail -1)
[ -n "$frames" ] || { echo "missing wsi presents stat" >&2; exit 1; }

python3 "$ROOT/scripts/ppm2png.py" "$OUT/fb.ppm" "$PNG"
nonblack=$(python3 - "$OUT/fb.ppm" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
toks, i = [], 0
while len(toks) < 4:
    while d[i:i+1].isspace(): i += 1
    if d[i:i+1] == b'#':
        while d[i:i+1] != b'\n': i += 1
        continue
    j = i
    while not d[j:j+1].isspace(): j += 1
    toks.append(d[i:j]); i = j
px = d[i+1:]
print(sum(1 for k in range(0, len(px) - 2, 3) if px[k] | px[k+1] | px[k+2]))
EOF
)
echo "VKB wsi ok frames=$frames png=$PNG"
echo "nonblack=$nonblack"
