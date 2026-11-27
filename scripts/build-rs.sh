#!/usr/bin/env bash
# Build a Rust crate for arm64 Android (API 29) via cargo-ndk.
# Usage: scripts/build-rs.sh <crate-dir> [cargo args...]   Out: build/rs/arm64/<bin(s) + lib*.so>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CRATE="$(cd "${1:?usage: build-rs.sh <crate-dir>}" && pwd)"
shift
source "$ROOT/orch/env.sh"
OUT="$ROOT/build/rs/arm64"
mkdir -p "$OUT"
cd "$CRATE"
cargo ndk -t arm64-v8a -P 29 build --release "$@"
REL="$CRATE/target/aarch64-linux-android/release"
n=0
for f in "$REL"/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in
    *.d|*.rlib|*.rmeta|*.a) continue ;;
  esac
  if [ -x "$f" ] || [[ "$f" == *.so ]]; then
    cp "$f" "$OUT/"
    echo "-> $OUT/$(basename "$f")"
    n=$((n+1))
  fi
done
[ "$n" -gt 0 ] || { echo "no outputs in $REL" >&2; exit 1; }
