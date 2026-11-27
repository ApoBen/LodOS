#!/usr/bin/env bash
# Build a private x86_64 Mesa EGL + Zink stack for the no-render-node proof.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/src/mesa-kopper"
BUILD="$ROOT/build/mesa-pc"
TAG=mesa-26.2.5
COMMIT=ddaf85e353442d9d7f192fdcf1823e652e3f2ee3
PATCH="$ROOT/patches/mesa/0001-egl-wayland-zink-kopper-without-render-node.patch"

if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone -q --depth 1 --branch "$TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
actual=$(git -C "$SRC" rev-parse HEAD)
[ "$actual" = "$COMMIT" ] || { echo "Mesa source mismatch: $actual (expected $COMMIT)" >&2; exit 1; }
if ! git -C "$SRC" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    git -C "$SRC" apply --check "$PATCH"
    git -C "$SRC" apply "$PATCH"
fi
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" "$SRC" \
        --prefix="$BUILD/install" --libdir=lib --buildtype=release \
        -Dplatforms=wayland,x11 -Dgallium-drivers=zink -Dvulkan-drivers= \
        -Dopengl=true -Degl=enabled -Dglx=dri -Dgles1=disabled -Dgles2=enabled \
        -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=enabled \
        -Dgallium-va=disabled \
        -Dvideo-codecs= -Dvulkan-layers= -Dvalgrind=disabled -Dlmsensors=disabled \
        -Dlibunwind=disabled -Dspirv-tools=disabled -Dexpat=enabled -Dzstd=enabled
else
    meson configure "$BUILD"
fi
ninja -C "$BUILD"
ninja -C "$BUILD" install
printf 'Mesa PC install: %s\n' "$BUILD/install"
