#!/usr/bin/env bash
# Build native/lodos-wl/test/shm_fill.c for arm64 Android (API 29) -> build/lodos-wl/arm64/shm_fill
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
source "$ROOT/orch/env.sh"

OUT="$ROOT/build/lodos-wl/arm64"
mkdir -p "$OUT"
CC="aarch64-linux-android29-clang"
STRIP="llvm-strip"

"$CC" -O2 -Wall -Wextra -fPIE -pie \
  native/lodos-wl/test/shm_fill.c \
  -llog \
  -o "$OUT/shm_fill.unstripped"
"$STRIP" -o "$OUT/shm_fill" "$OUT/shm_fill.unstripped"
rm -f "$OUT/shm_fill.unstripped"
chmod 755 "$OUT/shm_fill"
echo "-> $OUT/shm_fill ($(stat -c%s "$OUT/shm_fill") bytes)"
