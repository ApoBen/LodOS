set -e
source /mnt/SSD/LodOS/orch/env.sh
test -d "$ANDROID_NDK"
maj=$(sed -n 's/^Pkg.Revision *= *\([0-9]*\).*/\1/p' "$ANDROID_NDK/source.properties")
echo "NDK major=$maj"; [ "$maj" -ge 27 ]
"$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang" --version | head -1
command -v cmake && cmake --version | head -1
echo PASS
