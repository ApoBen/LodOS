#!/usr/bin/env bash
# Build static guest FIFO to Unix socket relay for x86_64 and aarch64 musl.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$ROOT/orch/env.sh" ] && source "$ROOT/orch/env.sh"
case ":$PATH:" in *":$HOME/.cargo/bin:"*) ;; *) export PATH="$HOME/.cargo/bin:$PATH" ;; esac
CRATE="$ROOT/native/lodos-audio-relay"
for ARCH in x86_64 aarch64; do
  TARGET="$ARCH-unknown-linux-musl"
  rustup target list --installed | grep -qx "$TARGET" || rustup target add "$TARGET"
  T_UP="$(echo "$TARGET" | tr 'a-z-' 'A-Z_')"
  export "CARGO_TARGET_${T_UP}_LINKER=rust-lld"
  export "CARGO_TARGET_${T_UP}_RUSTFLAGS=-C target-feature=+crt-static -C relocation-model=static"
  (cd "$CRATE" && cargo build --release --target "$TARGET")
  OUT="$ROOT/build/audio-relay/$ARCH/lodos-audio-relay"
  mkdir -p "$(dirname "$OUT")"
  install -m 0755 "$CRATE/target/$TARGET/release/lodos-audio-relay" "$OUT"
  file -L "$OUT" | grep -q 'statically linked' || { file -L "$OUT" >&2; exit 1; }
  echo "-> $OUT"
done
