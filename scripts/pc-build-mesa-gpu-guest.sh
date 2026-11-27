#!/usr/bin/env bash
# Invoked inside build/pc-rootfs with the repository bind-mounted at its host path.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/third_party/src/mesa-kopper
OUT=$ROOT/build/pc-guest-gpu
TAG=mesa-26.2.5
COMMIT=ddaf85e353442d9d7f192fdcf1823e652e3f2ee3
PATCH=$ROOT/patches/mesa/0001-egl-wayland-zink-kopper-without-render-node.patch
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$(dirname "$SRC")"
  git clone -q --depth 1 --branch "$TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
[ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo 'Mesa source commit mismatch' >&2; exit 1; }
if ! git -C "$SRC" apply --reverse --check "$PATCH" >/dev/null 2>&1; then git -C "$SRC" apply --check "$PATCH"; git -C "$SRC" apply "$PATCH"; fi
mkdir -p "$OUT"
if [ ! -f "$OUT/build.ninja" ]; then
  meson setup "$OUT" "$SRC" --prefix="$OUT/mesa-install" --libdir=lib --buildtype=release \
    -Dplatforms=wayland,x11 -Dgallium-drivers=zink -Dvulkan-drivers= \
    -Dopengl=true -Degl=enabled -Dglx=dri -Dgles1=disabled -Dgles2=enabled \
    -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=enabled -Dgallium-va=disabled \
    -Dvideo-codecs= -Dvulkan-layers= -Dvalgrind=disabled -Dlmsensors=disabled \
    -Dlibunwind=disabled -Dspirv-tools=disabled -Dexpat=enabled -Dzstd=enabled
else meson configure "$OUT"; fi
ninja -C "$OUT"
ninja -C "$OUT" install
echo "Mesa guest install: $OUT/mesa-install"
