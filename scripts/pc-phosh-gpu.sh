#!/usr/bin/env bash
# T073: phosh through phoc GLES/Zink -> guest LodOS ICD -> host Vulkan GPU.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
OUT=$ROOT/build/pc-guest-gpu; RFS=$ROOT/build/pc-rootfs
RUN=/tmp/lodos-run-$(id -u)/lodos-gpu
SOCK=lodos-gpu-wl; W=720; H=1600
mkdir -p "$OUT" "$RUN" docs/screens
chmod 700 "$RUN"
HOST_ICD=${HOST_ICD:-/usr/share/vulkan/icd.d/intel_icd.json}
VKH=$ROOT/third_party/src/Vulkan-Headers
[ -f "$VKH/registry/vk.xml" ] || { mkdir -p third_party/src; git clone -q --depth 1 --branch v1.4.369 https://github.com/KhronosGroup/Vulkan-Headers "$VKH"; }

# Build the private Mesa against trixie's own glibc and dependencies.
if [ ! -f "$OUT/mesa-install/lib/libEGL_mesa.so.0" ]; then
  bash scripts/pc-guest.sh run /bin/sh -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y -qq --no-install-recommends git meson ninja-build gcc g++ pkg-config python3-mako python3-yaml python3-ply flex bison libexpat1-dev libzstd-dev zlib1g-dev libdrm-dev libwayland-dev libwayland-egl-backend-dev libelf-dev libx11-dev libxrandr-dev libxext-dev libxxf86vm-dev libxdamage-dev libxfixes-dev libx11-xcb-dev libxcb-keysyms1-dev libxshmfence-dev libxcb-glx0-dev libxcb-shm0-dev libxcb-randr0-dev libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-xfixes0-dev'
  EXTRA_BINDS="-b $ROOT:$ROOT" bash scripts/pc-guest.sh run "$ROOT/scripts/pc-build-mesa-gpu-guest.sh"
fi

GEN=$OUT/gen; mkdir -p "$GEN"
python3 native/vk-bridge/gen/gen.py "$VKH/registry/vk.xml" native/vk-bridge/gen/cmds_icd.txt "$GEN"
CC=${CC:-gcc}; SRC=native/vk-bridge
CF=(-std=c11 -O2 -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")
"$CC" "${CF[@]}" "$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" "$SRC/transport/ring.c" "$SRC/transport/sock.c" -ldl -lpthread -o "$OUT/vkb_server"
"$CC" "${CF[@]}" -fPIC -shared -Wl,--version-script="$SRC/icd/icd.map" "$SRC/icd/icd.c" "$SRC/client/vkbc.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_client.c" "$SRC/transport/ring.c" "$SRC/transport/sock.c" -lpthread -ldl -o "$OUT/libvulkan_lodos.so"
cat >"$OUT/lodos_icd.json" <<EOF
{"file_format_version":"1.0.0","ICD":{"library_path":"$OUT/libvulkan_lodos.so","api_version":"1.3"}}
EOF
cat >"$OUT/guest_icd.json" <<EOF
{"file_format_version":"1.0.0","ICD":{"library_path":"/tmp/lodos-gpu/libvulkan_lodos.so","api_version":"1.3"}}
EOF
cp "$OUT/libvulkan_lodos.so" "$RFS/tmp/libvulkan_lodos.so"
cp "$OUT/guest_icd.json" "$RFS/tmp/lodos_icd.json"
(cd native/lodos-wl && cargo build --release --features headless -q)
WL=$ROOT/native/lodos-wl/target/release/lodos-wl
rm -f "$RUN/$SOCK" "$RUN/$SOCK.lock" "$OUT/fb.ppm"
PIDS=()
cleanup(){ set +e; for p in "${PIDS[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; done; for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done; rm -f "$RUN/$SOCK" "$RUN/$SOCK.lock"; }
trap cleanup EXIT INT TERM
VK_DRIVER_FILES="$HOST_ICD" VK_ICD_FILENAMES="$HOST_ICD" LODOS_VK_SOCK=@lodos-vk-zinkwsi "$OUT/vkb_server" >"$OUT/vkb.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 100); do grep -q listening "$OUT/vkb.log" && break; sleep .05; done
grep -q listening "$OUT/vkb.log" || { cat "$OUT/vkb.log"; exit 1; }
LODOS_WL_WIDTH=$W LODOS_WL_HEIGHT=$H LODOS_WL_NO_DMABUF=1 LODOS_WL_DUMP="$OUT/fb.ppm" LODOS_WL_CTL="$RUN/ctl" "$WL" "$RUN/$SOCK" >"$OUT/wl.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 100); do [ -S "$RUN/$SOCK" ] && break; sleep .05; done
printf '[core]\nxwayland=false\n\n[output:WL-1]\nmode=%dx%d\nscale=1\n' "$W" "$H" >"$RFS/tmp/lodos-gpu-phoc.ini"
bash scripts/pc-guest-init.sh >/dev/null
printf 'auth sufficient pam_permit.so\naccount sufficient pam_permit.so\n' >"$RFS/etc/pam.d/phosh"
GENV=(WAYLAND_DISPLAY="$SOCK" WLR_BACKENDS=wayland WLR_RENDERER=gles2 WLR_NO_HARDWARE_CURSORS=1 LIBGL_KOPPER_DRI2=1 MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=/tmp/lodos-gpu/lodos_icd.json VK_ICD_FILENAMES=/tmp/lodos-gpu/lodos_icd.json LODOS_VK_SOCK=@lodos-vk-zinkwsi LD_LIBRARY_PATH="/tmp/lodos-gpu/mesa-install/lib:/usr/lib/x86_64-linux-gnu" LIBGL_DRIVERS_PATH=/tmp/lodos-gpu/mesa-install/lib/dri __EGL_VENDOR_LIBRARY_FILENAMES=/tmp/lodos-gpu/mesa-install/share/glvnd/egl_vendor.d/50_mesa.json)
(
  unset WAYLAND_DISPLAY DISPLAY WAYLAND_SOCKET
  XDG_RUNTIME_DIR=$RUN EXTRA_BINDS="-b $OUT:/tmp/lodos-gpu" bash scripts/pc-guest.sh run /usr/bin/env "${GENV[@]}" /sbin/lodos-init --target=lodos-phosh.target --log=/tmp/lodos-desk/init.log --stop-timeout-ms=3000
) >"$OUT/guest.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 500); do grep -q 'Phosh ready' "$OUT/guest.log" && break; kill -0 "${PIDS[2]}" 2>/dev/null || break; sleep .1; done
grep -q 'Phosh ready' "$OUT/guest.log" || { tail -60 "$OUT/guest.log"; cat "$OUT/vkb.log"; exit 1; }
grep -Ei 'renderer|zink|GLES' "$OUT/guest.log" | tee "$OUT/renderer.log" || true
R=$(sed -nE 's/.*(zink[^,]*).*/\1/ip' "$OUT/guest.log" | head -1)
if [ -z "$R" ]; then
  R=$(EXTRA_BINDS="-b $OUT:/tmp/lodos-gpu" XDG_RUNTIME_DIR=$RUN bash scripts/pc-guest.sh run /usr/bin/env "${GENV[@]}" eglinfo -B -p wayland 2>&1 | sed -nE 's/^OpenGL (core|ES) profile renderer:[[:space:]]*//p' | head -1)
fi
[ -n "$R" ] || { cat "$OUT/guest.log"; echo 'unable to identify GL renderer'; exit 1; }
echo "GPU_RENDERER=$R"
sleep 4
python3 -I tools/wlctl.py "$RUN/ctl" dump "$OUT/fb.ppm"
python3 scripts/ppm2png.py "$OUT/fb.ppm" docs/screens/T073-phosh-gpu.png
kill -TERM "${PIDS[2]}" 2>/dev/null || true
for _ in $(seq 1 100); do kill -0 "${PIDS[2]}" 2>/dev/null || break; sleep .1; done
echo 'GPU desktop screenshot: docs/screens/T073-phosh-gpu.png'
