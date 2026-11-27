#!/usr/bin/env bash
# Build termux/proot (same commit as T006) + static talloc for the PC host (x86_64, glibc), host cc, static.
# Loader bundled into the exe (no PROOT_LOADER needed). 32-bit loader skipped.
# Out: build/proot/x86_64/proot   (used by scripts/pc-guest.sh)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PROOT_URL=https://github.com/termux/proot.git
PROOT_COMMIT=c0b2d098469a64a88c0a7e240b1a1dc252988cdb
TALLOC_VER=2.4.2
TALLOC_URL=https://www.samba.org/ftp/talloc/talloc-$TALLOC_VER.tar.gz
TALLOC_SHA256=85ecf9e465e20f98f9950a52e9a411e14320bc555fa257d87697b7e7a9b1d8a6
SRC=third_party/src
PROOT_SRC=$SRC/proot
TALLOC_SRC=$SRC/talloc-$TALLOC_VER
OBJ=build/proot/obj-x86_64
OUT=build/proot/x86_64
CC=${CC:-cc}

mkdir -p "$SRC"
[ -d "$PROOT_SRC/.git" ] || git clone "$PROOT_URL" "$PROOT_SRC"
if [ "$(git -C "$PROOT_SRC" rev-parse HEAD)" != "$PROOT_COMMIT" ]; then
  git -C "$PROOT_SRC" fetch origin "$PROOT_COMMIT" 2>/dev/null || git -C "$PROOT_SRC" fetch origin
  git -C "$PROOT_SRC" -c advice.detachedHead=false checkout "$PROOT_COMMIT"
fi
if [ ! -f "$TALLOC_SRC/talloc.c" ]; then
  tgz=$SRC/talloc-$TALLOC_VER.tar.gz
  curl -fsSL -o "$tgz" "$TALLOC_URL"
  echo "$TALLOC_SHA256  $tgz" | sha256sum -c -
  tar -C "$SRC" -xzf "$tgz"; rm -f "$tgz"
fi

TOBJ=$OBJ/talloc
mkdir -p "$TOBJ/include"
cp scripts/proot/talloc-config.h "$TOBJ/include/config.h"
cp "$TALLOC_SRC/talloc.h" "$TOBJ/include/"
$CC -O2 -fPIC -D_GNU_SOURCE -D__STDC_WANT_LIB_EXT1__=1 -I"$TOBJ/include" -I"$TALLOC_SRC" -I"$TALLOC_SRC/lib/replace" \
  -c "$TALLOC_SRC/talloc.c" -o "$TOBJ/talloc.o"
rm -f "$TOBJ/libtalloc.a"; ar rcs "$TOBJ/libtalloc.a" "$TOBJ/talloc.o"

POBJ=$ROOT/$OBJ/proot
rm -rf "$POBJ"; mkdir -p "$POBJ"
cp -a "$PROOT_SRC/src/." "$POBJ/"
f=$POBJ/extension/ashmem_memfd/ashmem_memfd.c
grep -q '<string.h>' "$f" || sed -i '1i #include <string.h>' "$f"
CPPFLAGS="-I$ROOT/$TOBJ/include" LDFLAGS="-L$ROOT/$TOBJ -static" \
make -C "$POBJ" -j"$(nproc)" CC="$CC" LD="$CC" GIT="git -C $ROOT/$PROOT_SRC" HAS_LOADER_32BIT= proot

mkdir -p "$OUT"
strip -o "$OUT/proot" "$POBJ/proot"; chmod 755 "$OUT/proot"
file "$OUT/proot"
