#!/usr/bin/env bash
# Build leegao/bionic-vulkan-wrapper (Mesa "wrapper" vk driver) for Android arm64 (NDK r30, API 29).
# Output: build/vk-layer/arm64/libvulkan_lodos_mali.so  (wraps /system/lib64/libvulkan.so, BCn via compute)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
source orch/env.sh
SRC=$ROOT/third_party/src/bionic-vulkan-wrapper
SPV=$ROOT/third_party/src/SPIRV-Tools
B=$ROOT/build/vk-layer
WRAP_COMMIT=3abf48704a4d9303967a0811c0dc8ff8567d613b
SPV_COMMIT=20a5b4305f22d94f78281d7fd9a185ae89590d4f
SPVH_COMMIT=97e96f9e9defeb4bba3cfbd034dec516671dd7a3
JOBS=$(nproc)

for t in meson ninja glslangValidator python3; do command -v $t >/dev/null || { echo "missing $t"; exit 1; }; done
python3 -c 'import mako, yaml' 2>/dev/null || pip install --user mako pyyaml

clone() { # url dir commit
  if [ ! -d "$2/.git" ]; then git clone -q "$1" "$2"; fi
  if [ "$(git -C "$2" rev-parse HEAD)" != "$3" ]; then
    git -C "$2" fetch -q origin "$3" 2>/dev/null || git -C "$2" fetch -q --unshallow 2>/dev/null || true
    git -C "$2" checkout -q "$3"
  fi
}
clone https://github.com/leegao/bionic-vulkan-wrapper.git "$SRC" "$WRAP_COMMIT"
clone https://github.com/leegao/SPIRV-Tools.git "$SPV" "$SPV_COMMIT"
clone https://github.com/KhronosGroup/SPIRV-Headers.git "$SPV/external/spirv-headers" "$SPVH_COMMIT"

# patches (idempotent)
for p in "$ROOT"/patches/bionic-vulkan-wrapper/*.patch; do
  if git -C "$SRC" apply --reverse --check "$p" 2>/dev/null; then continue; fi
  git -C "$SRC" apply "$p"
done

# 1) SPIRV-Tools (leegao fork w/ Mali passes), static, arm64
if [ ! -f "$B/spirv-tools/source/opt/libSPIRV-Tools-opt.a" ]; then
  cmake -S "$SPV" -B "$B/spirv-tools" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections -g0" \
    -DSPIRV_CHECK_CONTEXT=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF \
    -DBUILD_SHARED_LIBS=OFF >/dev/null
  cmake --build "$B/spirv-tools" -j"$JOBS" --target SPIRV-Tools-static SPIRV-Tools-opt
fi
mkdir -p "$SRC/src/vulkan/wrapper/lib"
cp -u "$B/spirv-tools/source/libSPIRV-Tools.a" "$B/spirv-tools/source/opt/libSPIRV-Tools-opt.a" "$SRC/src/vulkan/wrapper/lib/"

# 2) meson cross build (no X11/DRI3/wayland WSI; __TERMUX__ selects the AHardwareBuffer WSI/bionic paths)
if [ ! -f "$B/meson/build.ninja" ]; then
  meson setup "$B/meson" "$SRC" --cross-file "$ROOT/scripts/cross/android-arm64.ini" \
    --buildtype=release -Db_ndebug=true \
    -Dc_args=-D__TERMUX__ -Dcpp_args=-D__TERMUX__ \
    -Dplatforms= -Dvulkan-drivers=wrapper -Dgallium-drivers= -Dopengl=false \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dplatform-sdk-version=29 -Dandroid-libbacktrace=disabled -Dvulkan-layers= -Dtools= \
    -Dbuild-tests=false -Dshared-glapi=disabled -Ddri3=disabled
fi
ninja -C "$B/meson" -j"$JOBS" src/vulkan/wrapper/libvulkan_wrapper.so

mkdir -p "$B/arm64"
llvm-strip --strip-unneeded -o "$B/arm64/libvulkan_lodos_mali.so" "$B/meson/src/vulkan/wrapper/libvulkan_wrapper.so"
echo "deps:"; llvm-readelf -d "$B/arm64/libvulkan_lodos_mali.so" | grep NEEDED
ls -la "$B/arm64/libvulkan_lodos_mali.so"
