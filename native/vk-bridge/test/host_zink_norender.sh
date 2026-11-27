#!/usr/bin/env bash
# T067: Zink Kopper Wayland EGL over LodOS ICD with no compositor DRM feedback.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
PREFIX="$ROOT/build/mesa-pc/install"
if [ ! -f "$PREFIX/lib/libEGL_mesa.so.0" ]; then
    "$ROOT/scripts/build-mesa-pc.sh"
fi
[ -f "$PREFIX/lib/libEGL_mesa.so.0" ] || { echo "private Mesa EGL missing at $PREFIX" >&2; exit 1; }
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="$PREFIX/lib/dri"
export __EGL_VENDOR_LIBRARY_FILENAMES="$PREFIX/share/glvnd/egl_vendor.d/50_mesa.json"
export LIBGL_KOPPER_DRI2=1
# Keep the bridge proof runnable on headless PCs with no /dev/dri. Zink still
# traverses the LodOS ICD; SwiftShader is the Vulkan device behind its server.
if [ -z "${HOST_ICD:-}" ]; then
    SWIFTSHADER_ICD=$(find /usr/lib -name vk_swiftshader_icd.json -print -quit 2>/dev/null || true)
    [ -z "$SWIFTSHADER_ICD" ] || export HOST_ICD="$SWIFTSHADER_ICD"
fi
export ZW_NO_DMABUF=1
export ZW_PNG="$ROOT/docs/screens/T067-eglgears-norender.png"
export ZW_FRAMES=${ZW_FRAMES:-120}
export ZW_DUMP_AFTER=${ZW_DUMP_AFTER:-100}
bash "$ROOT/native/vk-bridge/test/host_zinkwsi.sh"
[ -s "$ZW_PNG" ] || { echo "no-render screenshot missing: $ZW_PNG" >&2; exit 1; }
echo "NORENDER ok"
