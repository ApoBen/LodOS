#!/usr/bin/env bash
# Build static (musl) lodos-init + systemctl shim for the glibc proot guest (R18).
# Usage: scripts/build-init.sh <x86_64|aarch64>   Out: build/init/<arch>/{lodos-init,systemctl}
# Toolchain: rustup musl targets (self-contained crt/libc.a) linked by rust-lld; no sudo,
# no musl-gcc/zig. crt-static + relocation-model=static -> plain "statically linked"
# (not static-pie). Bionic build stays in scripts/build-rs.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${1:?usage: build-init.sh <x86_64|aarch64>}"
case "$ARCH" in
  x86_64|aarch64) ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 2 ;;
esac
[ -f "$ROOT/orch/env.sh" ] && source "$ROOT/orch/env.sh"
case ":$PATH:" in *":$HOME/.cargo/bin:"*) ;; *) export PATH="$HOME/.cargo/bin:$PATH" ;; esac
TARGET="$ARCH-unknown-linux-musl"
rustup target list --installed | grep -qx "$TARGET" || rustup target add "$TARGET"
T_UP="$(echo "$TARGET" | tr 'a-z-' 'A-Z_')"
export "CARGO_TARGET_${T_UP}_LINKER=rust-lld"
export "CARGO_TARGET_${T_UP}_RUSTFLAGS=-C target-feature=+crt-static -C relocation-model=static"
CRATE="$ROOT/native/lodos-init"
OUT="$ROOT/build/init/$ARCH"
mkdir -p "$OUT"
cd "$CRATE"
cargo build --release --target "$TARGET"
REL="$CRATE/target/$TARGET/release"
for b in lodos-init systemctl; do
  install -m 0755 "$REL/$b" "$OUT/$b"
  file -L "$OUT/$b" | grep -q 'statically linked' || { echo "not static: $OUT/$b" >&2; file -L "$OUT/$b" >&2; exit 1; }
  echo "-> $OUT/$b"
done
