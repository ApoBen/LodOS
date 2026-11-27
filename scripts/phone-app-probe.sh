#!/usr/bin/env bash
# T017: build APK with jniLibs (proot, loader, lodos-wl), install, run ProbeActivity at app uid, dump LODOS_PROBE log.
# exit 2 + NEEDS_USER_INSTALL if HyperOS blocks adb install (R7).
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
A="adb -s $SERIAL"
PKG=org.lodos.app
APK=android/app/build/outputs/apk/debug/app-debug.apk

bash scripts/stage-jnilibs.sh
(cd android && ./gradlew -q assembleDebug)
[ -f "$APK" ] || { echo "no apk at $APK"; exit 1; }

if ! out=$($A install -r "$APK" 2>&1); then
  echo "$out"
  echo "NEEDS_USER_INSTALL $(realpath "$APK")"
  exit 2
fi
echo "$out" | tail -1

$A shell am force-stop $PKG || true
$A logcat -c
$A shell am start -W -n $PKG/.ProbeActivity >/dev/null
for i in $(seq 1 60); do
  $A logcat -d -s LODOS_PROBE | grep -q PROBE_DONE && break
  sleep 0.5
done
$A logcat -d -s LODOS_PROBE
$A shell am force-stop $PKG || true
$A logcat -d -s LODOS_PROBE | grep -q PROBE_DONE || { echo "timeout waiting PROBE_DONE"; exit 1; }
