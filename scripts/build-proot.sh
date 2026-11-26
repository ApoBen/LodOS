#!/usr/bin/env bash
# Build termux/proot + static talloc for arm64 Android (API 29) with the NDK.
# Out: build/proot/arm64/libproot.so (proot exe), build/proot/arm64/libproot-loader.so (loader).
# Runtime: PROOT_LOADER=<path>/libproot-loader.so PROOT_TMP_DIR=<writable dir> ./libproot.so ...
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[ -n "${ANDROID_NDK:-}" ] || source orch/env.sh

API=29
TRIPLE=aarch64-linux-android$API
PROOT_URL=https://github.com/termux/proot.git
PROOT_COMMIT=c0b2d098469a64a88c0a7e240b1a1dc252988cdb
TALLOC_VER=2.4.2
TALLOC_URL=https://www.samba.org/ftp/talloc/talloc-$TALLOC_VER.tar.gz
TALLOC_SHA256=85ecf9e465e20f98f9950a52e9a411e14320bc555fa257d87697b7e7a9b1d8a6

SRC=third_party/src
PROOT_SRC=$SRC/proot
TALLOC_SRC=$SRC/talloc-$TALLOC_VER
OBJ=build/proot/obj-arm64
OUT=build/proot/arm64
JOBS=$(nproc)

export CC=$TRIPLE-clang STRIP=llvm-strip OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump AR=llvm-ar
command -v "$CC" >/dev/null || { echo "NDK clang $CC not in PATH" >&2; exit 1; }

# --- sources ---
mkdir -p "$SRC"
if [ ! -d "$PROOT_SRC/.git" ]; then
  git clone "$PROOT_URL" "$PROOT_SRC"
fi
if [ "$(git -C "$PROOT_SRC" rev-parse HEAD)" != "$PROOT_COMMIT" ]; then
  git -C "$PROOT_SRC" fetch origin "$PROOT_COMMIT" 2>/dev/null || git -C "$PROOT_SRC" fetch origin
  git -C "$PROOT_SRC" -c advice.detachedHead=false checkout "$PROOT_COMMIT"
fi
if [ ! -f "$TALLOC_SRC/talloc.c" ]; then
  tgz=$SRC/talloc-$TALLOC_VER.tar.gz
  curl -fsSL -o "$tgz" "$TALLOC_URL"
  echo "$TALLOC_SHA256  $tgz" | sha256sum -c -
  tar -C "$SRC" -xzf "$tgz"
  rm -f "$tgz"
fi

# --- talloc (static, single TU; replaces waf build) ---
TOBJ=$OBJ/talloc
mkdir -p "$TOBJ/include"
cp scripts/proot/talloc-config.h "$TOBJ/include/config.h"
cp "$TALLOC_SRC/talloc.h" "$TOBJ/include/"
$CC -O2 -fPIC -D_GNU_SOURCE -D__STDC_WANT_LIB_EXT1__=1 \
  -I"$TOBJ/include" -I"$TALLOC_SRC" -I"$TALLOC_SRC/lib/replace" \
  -c "$TALLOC_SRC/talloc.c" -o "$TOBJ/talloc.o"
rm -f "$TOBJ/libtalloc.a"
$AR rcs "$TOBJ/libtalloc.a" "$TOBJ/talloc.o"

# --- proot (termux recipe: unbundled loader; no 32-bit loader) ---
# upstream out-of-tree build is broken ($(SRC)$< doubles abs paths) -> build in a synced copy
POBJ=$ROOT/$OBJ/proot
mkdir -p "$POBJ"
cp -a "$PROOT_SRC/src/." "$POBJ/"
# upstream ashmem_memfd.c lacks <string.h>; NDK clang errors on implicit decls
f=$POBJ/extension/ashmem_memfd/ashmem_memfd.c
grep -q '<string.h>' "$f" || sed -i '1i #include <string.h>' "$f"
# CPPFLAGS/LDFLAGS via env so the makefile's += still applies
CPPFLAGS="-I$ROOT/$TOBJ/include" LDFLAGS="-L$ROOT/$TOBJ" \
make -C "$POBJ" -j"$JOBS" \
  CC="$CC" LD="$CC" STRIP="$STRIP" OBJCOPY="$OBJCOPY" OBJDUMP="$OBJDUMP" \
  GIT="git -C $ROOT/$PROOT_SRC" \
  HAS_LOADER_32BIT= \
  PROOT_UNBUNDLE_LOADER=/data/local/tmp/lodos/proot \
  proot loader/loader

mkdir -p "$OUT"
$STRIP -o "$OUT/libproot.so" "$POBJ/proot"
$STRIP -o "$OUT/libproot-loader.so" "$POBJ/loader/loader"
chmod 755 "$OUT/libproot.so" "$OUT/libproot-loader.so"
file "$OUT/libproot.so" "$OUT/libproot-loader.so"
