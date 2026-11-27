#!/usr/bin/env bash
# Push proot + Debian rootfs tarball to phone, extract under proot (link2symlink) into
# /data/local/tmp/lodos/rootfs/debian. Idempotent: skips if extracted sha matches. FORCE=1 re-extracts.
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
A="adb -s $SERIAL"
L=/data/local/tmp/lodos
PROOT_DIR=$L/proot
RFS=$L/rootfs/debian
TB=build/rootfs/debian-trixie-arm64.tar.xz
[ -f "$TB" ] || bash scripts/fetch-rootfs.sh
[ -f build/proot/arm64/libproot.so ] || bash scripts/build-proot.sh
SHA=$(sha256sum "$TB" | cut -d' ' -f1)
# phone toybox tar has no xz (no xz binary on Android) but has gzip -> recompress on PC (cached by sha)
TGZ=build/rootfs/debian-trixie-arm64.tar.gz
if [ ! -f "$TGZ" ] || [ "$(cat "$TGZ.src-sha" 2>/dev/null)" != "$SHA" ]; then
  echo "recompressing xz -> gz..."
  xz -dc "$TB" | gzip -1 > "$TGZ.part" && mv "$TGZ.part" "$TGZ" && echo "$SHA" > "$TGZ.src-sha"
fi

# proot binaries (push only if changed)
$A shell "mkdir -p $PROOT_DIR $L/tmp $L/rootfs"
for f in libproot.so libproot-loader.so; do
  ls=$(sha256sum build/proot/arm64/$f | cut -d' ' -f1)
  rs=$($A shell "sha256sum $PROOT_DIR/$f 2>/dev/null" | cut -d' ' -f1 || true)
  if [ "$ls" != "$rs" ]; then $A push build/proot/arm64/$f $PROOT_DIR/$f >/dev/null; fi
  $A shell "chmod 755 $PROOT_DIR/$f"
done

done_sha=$($A shell "cat $RFS/.lodos-rootfs-sha 2>/dev/null" | tr -d '\r' || true)
if [ "${FORCE:-0}" != 1 ] && [ "$done_sha" = "$SHA" ]; then
  echo "rootfs already extracted ($SHA)"
else
  echo "pushing rootfs tarball..."
  $A push "$TGZ" $L/rootfs/debian.tar.gz >/dev/null
  echo "extracting on phone under proot..."
  # proot -0 fakes chown; --link2symlink emulates hardlinks. Old tree may have read-only dirs -> chmod first.
  $A shell "cd $L/rootfs && { [ -d debian ] && chmod -R u+rwx debian 2>/dev/null; rm -rf debian; true; } && mkdir debian && \
    PROOT_LOADER=$PROOT_DIR/libproot-loader.so PROOT_TMP_DIR=$L/tmp \
    $PROOT_DIR/libproot.so --link2symlink -0 /system/bin/tar -xzf debian.tar.gz -C debian; echo tar_rc=\$?"
  $A shell "test -x $RFS/bin/sh" || { echo "extract failed: no $RFS/bin/sh"; exit 1; }
  $A shell "rm -f $L/rootfs/debian.tar.gz"
  # DNS for apt (rootfs resolv.conf may be a symlink to systemd-resolved)
  $A shell "rm -f $RFS/etc/resolv.conf; printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > $RFS/etc/resolv.conf"
  $A shell "echo $SHA > $RFS/.lodos-rootfs-sha"
  echo "rootfs extracted to $RFS"
fi
