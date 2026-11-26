#!/usr/bin/env bash
# T017/D4: stage native executables into APK jniLibs as lib*.so (exec'd from nativeLibraryDir).
set -euo pipefail
cd "$(dirname "$0")/.."
D=android/app/src/main/jniLibs/arm64-v8a
declare -A M=(
  [build/proot/arm64/libproot.so]=libproot.so
  [build/proot/arm64/libproot-loader.so]=libproot_loader.so
  [build/rs/arm64/lodos-wl]=liblodos_wl.so
)
for s in "${!M[@]}"; do [ -f "$s" ] || { echo "missing $s (build it first)" >&2; exit 1; }; done
mkdir -p "$D"
for s in "${!M[@]}"; do install -m 755 "$s" "$D/${M[$s]}"; done
ls -l "$D"
