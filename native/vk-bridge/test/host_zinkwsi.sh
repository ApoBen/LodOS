#!/usr/bin/env bash
# T051: PC-host zink GL on wayland. eglgears_wayland (EGL wayland, MESA_LOADER_DRIVER_OVERRIDE=zink -> kopper)
# -> libvulkan_lodos.so (T047 ICD WSI: wl_shm) -> vkb_server -> host Intel ICD, presented on lodos-wl headless;
# PPM dumped after N commits -> docs/screens/T051-zink-wsi.png. No phone needed.
# Prints: GL_RENDERER line, VKB zinkwsi ok frames=<presents> renderer=<str>
# env: ZW_FRAMES (min presents before stop, 120), ZW_DUMP_AFTER (compositor commit to dump at, 100), ZW_APP
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC="$ROOT/native/vk-bridge"
VKH="$ROOT/third_party/src/Vulkan-Headers"
TAG=v1.4.369
if [ ! -f "$VKH/registry/vk.xml" ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
fi
APP=${ZW_APP:-eglgears_wayland}
command -v "$APP" >/dev/null || { echo "$APP missing (install mesa-utils)" >&2; exit 1; }

HOST_ICD=${HOST_ICD:-/usr/share/vulkan/icd.d/intel_icd.json}
SOCK=${LODOS_VK_SOCK:-@lodos-vk-zinkwsi}
FRAMES=${ZW_FRAMES:-120}
DUMP_AFTER=${ZW_DUMP_AFTER:-100}
W=640; H=480
OUT="$ROOT/build/vk-bridge/host-zinkwsi"
GEN="$OUT/gen"
PNG="$ROOT/docs/screens/T051-zink-wsi.png"
rm -rf "$OUT"
mkdir -p "$GEN" "$OUT/cache" "$ROOT/docs/screens"

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

RUN=$(mktemp -d "${TMPDIR:-/tmp}/lodos-zinkwsi.XXXXXX")
WLSOCK=lodos-wl-zinkwsi
srv_pid=""; wl_pid=""; app_pid=""
cleanup() {
    for p in $app_pid $srv_pid $wl_pid; do
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
RENDER_NODE=${LODOS_WL_DMABUF_DEV:-$(ls /dev/dri/renderD* 2>/dev/null | head -1 || true)}
LODOS_WL_WIDTH=$W LODOS_WL_HEIGHT=$H LODOS_WL_DMABUF_DEV="$RENDER_NODE" \
    LODOS_WL_DUMP="$OUT/fb.ppm" LODOS_WL_DUMP_AFTER=$DUMP_AFTER \
    "$WL" "$RUN/$WLSOCK" >"$OUT/lodos-wl.log" 2>&1 &
wl_pid=$!
for _ in $(seq 1 50); do [ -S "$RUN/$WLSOCK" ] && break; sleep 0.1; done
[ -S "$RUN/$WLSOCK" ] || { cat "$OUT/lodos-wl.log" >&2; exit 1; }

ZINK_WL_ENV=(
    env -u DISPLAY -u WAYLAND_SOCKET -u LIBGL_ALWAYS_SOFTWARE
    XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY="$WLSOCK"
    MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=wayland MESA_SHADER_CACHE_DIR="$OUT/cache" vblank_mode=0
    VK_DRIVER_FILES="$OUT/lodos_icd.json" VK_ICD_FILENAMES="$OUT/lodos_icd.json" VK_LOADER_LAYERS_DISABLE='*'
    LODOS_VK_SOCK="$SOCK"
)

renderer=""
if command -v eglinfo >/dev/null 2>&1; then
    echo "== verify Wayland EGL renderer via eglinfo -B -p wayland"
    "${ZINK_WL_ENV[@]}" LODOS_VK_LOG_MISSING=0 eglinfo -B -p wayland >"$OUT/eglinfo.out" 2>"$OUT/eglinfo.err" || {
        cat "$OUT/eglinfo.out"; cat "$OUT/eglinfo.err" >&2; exit 1;
    }
    renderer=$(sed -n 's/^OpenGL core profile renderer:[[:space:]]*//p' "$OUT/eglinfo.out" | head -1)
    [ -n "$renderer" ] || renderer=$(sed -n 's/^OpenGL ES profile renderer:[[:space:]]*//p' "$OUT/eglinfo.out" | head -1)
fi

echo "== run $APP -info via zink -> lodos ICD (until $FRAMES presents)"
"${ZINK_WL_ENV[@]}" LODOS_VK_WSI_STATS=1 LODOS_VK_WSI_STATS_EVERY=10 LODOS_VK_LOG_MISSING=1 \
    timeout 300 "$APP" -info >"$OUT/app.out" 2>"$OUT/app.err" &
app_pid=$!
frames=0
for _ in $(seq 1 1200); do
    frames=$(sed -n 's/.*wsi presents=\([0-9][0-9]*\).*/\1/p' "$OUT/app.err" | tail -1)
    frames=${frames:-0}
    [ "$frames" -ge "$FRAMES" ] && [ -s "$OUT/fb.ppm" ] && break
    kill -0 "$app_pid" 2>/dev/null || break
    sleep 0.1
done
pkill -TERM -P "$app_pid" 2>/dev/null || true
kill -TERM "$app_pid" 2>/dev/null || true
wait "$app_pid" 2>/dev/null || true
app_pid=""
frames=$(sed -n 's/.*wsi presents=\([0-9][0-9]*\).*/\1/p' "$OUT/app.err" | tail -1)
frames=${frames:-0}
head -20 "$OUT/app.out" || true
{ grep -v 'wsi presents=' "$OUT/app.err" | sort | uniq -c | head -30 || true; } >&2
echo "-- lodos-wl log"; tail -5 "$OUT/lodos-wl.log"
[ "$frames" -ge "$FRAMES" ] || { echo "only $frames presents (< $FRAMES)" >&2; tail -20 "$OUT/srv.log" >&2; exit 1; }
test -s "$OUT/fb.ppm" || { echo "no PPM dump (commits < $DUMP_AFTER?)" >&2; exit 1; }
app_renderer=$(sed -n 's/^GL_RENDERER[[:space:]]*=[[:space:]]*//p' "$OUT/app.out" | head -1)
[ -n "$app_renderer" ] && renderer="$app_renderer"
echo "GL_RENDERER = $renderer"
case "$renderer" in *zink*) ;; *) echo "renderer is not zink" >&2; exit 1;; esac

python3 "$ROOT/scripts/ppm2png.py" "$OUT/fb.ppm" "$PNG"
echo "VKB zinkwsi ok frames=$frames renderer=$renderer"
