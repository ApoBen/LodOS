#!/usr/bin/env bash
# T017/T022/D4: stage native libs and executables into APK jniLibs as lib*.so.
set -euo pipefail
cd "$(dirname "$0")/.."
D=android/app/src/main/jniLibs/arm64-v8a
# Build the in-process Android rootfs extractor with its JNI exports enabled.
bash scripts/build-rs.sh native/lodos-rootfs --features jni
bash scripts/build-rs.sh native/lodos-audio --features android
bash scripts/build-audio-relay.sh
declare -A M=(
  [build/proot/arm64/libproot.so]=libproot.so
  [build/proot/arm64/libproot-loader.so]=libproot_loader.so
  [build/rs/arm64/liblodos_wl.so]=liblodos_wl.so
  [build/rs/arm64/lodos-wl]=liblodos_wl_bin.so
  [build/lodos-wl/arm64/shm_fill]=libshmfill.so
  [build/rs/arm64/liblodos_rootfs.so]=liblodos_rootfs.so
  [build/rs/arm64/lodos-audio]=liblodos_audio.so
  [build/audio-relay/aarch64/lodos-audio-relay]=liblodos_audio_relay.so
)
for s in "${!M[@]}"; do [ -f "$s" ] || { echo "missing $s (build it first)" >&2; exit 1; }; done
mkdir -p "$D"
for s in "${!M[@]}"; do install -m 755 "$s" "$D/${M[$s]}"; done
ls -l "$D"
