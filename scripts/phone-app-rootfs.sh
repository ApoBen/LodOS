#!/usr/bin/env bash
# T020: build APK with jniLibs, push Debian trixie rootfs tar.gz, run ProbeActivity(probe=rootfs)
# at app uid (untrusted_app) -> extract rootfs, apt-get update/install hello, probe /dev/dma_heap/system.
# Env: ROOTFS=rootfs2 selects an alternate app rootfs; FRESH=1 clears it first.
# FORCE=1 retains the original T020 behavior and clears rootfs.
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
A="adb -s $SERIAL"
PKG=org.lodos.app
APK=android/app/build/outputs/apk/debug/app-debug.apk
L=/data/local/tmp/lodos
REMOTE_TGZ=$L/debian-trixie-arm64.tar.gz
ROOTFS=${ROOTFS:-rootfs}
[[ "$ROOTFS" == rootfs || "$ROOTFS" == rootfs2 ]] || { echo "invalid ROOTFS=$ROOTFS" >&2; exit 2; }

# 1. Stage jniLibs & build APK
bash scripts/stage-jnilibs.sh
(cd android && ./gradlew --no-daemon --console=plain -q assembleDebug </dev/null)
[ -f "$APK" ] || { echo "no apk at $APK"; exit 1; }

if ! out=$($A install -r "$APK" 2>&1); then
  echo "$out"
  echo "NEEDS_USER_INSTALL $(realpath "$APK")"
  exit 2
fi
echo "$out" | tail -1

# 2. Ensure Debian trixie .tar.gz exists on PC (same cache as scripts/phone-rootfs.sh)
TB=build/rootfs/debian-trixie-arm64.tar.xz
[ -f "$TB" ] || bash scripts/fetch-rootfs.sh
SHA=$(sha256sum "$TB" | cut -d' ' -f1)
TGZ=build/rootfs/debian-trixie-arm64.tar.gz
if [ ! -f "$TGZ" ] || [ "$(cat "$TGZ.src-sha" 2>/dev/null || true)" != "$SHA" ]; then
  echo "recompressing xz -> gz..."
  xz -dc "$TB" | gzip -1 > "$TGZ.part" && mv "$TGZ.part" "$TGZ" && echo "$SHA" > "$TGZ.src-sha"
fi

# 3. Handle FORCE=1 or check if app already has extracted rootfs
if [ "${FORCE:-0}" = 1 ] || [ "${FRESH:-0}" = 1 ]; then
  echo "fresh extraction: wiping app $ROOTFS..."
  $A shell "run-as $PKG sh -c 'chmod -R u+rwx files/$ROOTFS files/$ROOTFS.tmp 2>/dev/null; rm -rf files/$ROOTFS files/$ROOTFS.tmp'" || true
fi

has_rootfs=0
if $A shell "run-as $PKG test -f files/$ROOTFS/etc/debian_version" 2>/dev/null; then
  has_rootfs=1
  echo "app rootfs already present (skipping tarball push)"
fi

if [ "$has_rootfs" = 0 ]; then
  $A shell "mkdir -p $L"
  local_sz=$(stat -c%s "$TGZ")
  remote_sz=$($A shell "stat -c%s $REMOTE_TGZ 2>/dev/null" | tr -d '\r' || echo 0)
  if [ "$local_sz" != "$remote_sz" ]; then
    echo "pushing $TGZ ($local_sz bytes) to $REMOTE_TGZ..."
    $A push "$TGZ" "$REMOTE_TGZ" >/dev/null
  else
    echo "tarball already on device ($remote_sz bytes)"
  fi
  $A shell "chmod 644 $REMOTE_TGZ"
fi

# 4. Run ProbeActivity in rootfs mode at app uid
$A shell am force-stop $PKG || true
$A logcat -c
$A shell am start -W -n $PKG/.ProbeActivity -e probe rootfs -e rootfs "$ROOTFS" -e tarball "$REMOTE_TGZ" >/dev/null

# Wait up to 15m (450 * 2s) for RFS_DONE
done_flag=0
for i in $(seq 1 450); do
  if $A logcat -d -s LODOS_PROBE | grep -q 'RFS_DONE'; then
    done_flag=1
    break
  fi
  sleep 2
done

$A logcat -d -s LODOS_PROBE
$A shell am force-stop $PKG || true
[ "$done_flag" = 1 ] || { echo "timeout waiting RFS_DONE"; exit 1; }
