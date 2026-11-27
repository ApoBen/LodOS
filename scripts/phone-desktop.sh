#!/usr/bin/env bash
# T043: phosh nested on Android lodos-wl headless, rendered by pixman.
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-<phone-serial>}
A=(adb -s "$SERIAL")
L=/data/local/tmp/lodos
RUN=$L/run
RFS=$L/rootfs/debian
OUT=build/phone-desktop
mkdir -p "$OUT" docs/screens
BIN=build/rs/arm64/lodos-wl

bash scripts/phone-rootfs.sh
XKB_CONFIG_ROOT=/data/local/tmp/lodos/rootfs/debian/usr/share/X11/xkb bash scripts/build-xkbcommon.sh
bash scripts/build-rs.sh native/lodos-wl --features headless
U2A=build/u2a/libunix2abstract.so
ESHIM=build/elogind-shim/libelogind_shim.so
mkdir -p build/u2a build/elogind-shim
clang --target=aarch64-linux-gnu -O2 -Wall -fPIC -shared -nostdlib -fno-builtin -fuse-ld=lld -o "$U2A" native/unix2abstract/unix2abstract.c
clang --target=aarch64-linux-gnu -O2 -Wall -fPIC -shared -nostdlib -fno-builtin -fuse-ld=lld -o "$ESHIM" native/elogind-shim/elogind_shim.c

"${A[@]}" shell "mkdir -p $L/bin $RUN $RFS/usr/local/lib/lodos"
"${A[@]}" push "$BIN" "$L/bin/lodos-wl" >/dev/null
"${A[@]}" push "$U2A" "$ESHIM" "$L/rootfs/debian/usr/local/lib/lodos/" >/dev/null
"${A[@]}" push scripts/phone-phosh-session.sh "$RFS/tmp/lodos-phone-phosh.sh" >/dev/null
"${A[@]}" shell "chmod 755 $L/bin/lodos-wl $RFS/tmp/lodos-phone-phosh.sh"

# Prepare the same systemd-free dbus/elogind base used by the earlier phone spike.
bash scripts/guest-init.sh >"$OUT/guest-init.log" 2>&1
export EXTRA_BINDS="-b $RUN:/run/host-xdg"
bash scripts/phone-run.sh 'set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends phoc phosh foot dbus elogind gnome-settings-daemon-common librsvg2-common xkb-data
apt-get clean
' >"$OUT/packages.log" 2>&1

W=${PHONE_DESKTOP_W:-720}; H=${PHONE_DESKTOP_H:-1600}
T0=$(date +%s)
"${A[@]}" shell "rm -f $RUN/lodos-wl $RUN/lodos-wl.lock $RUN/T043.ppm $RUN/lodos-wl.log $RUN/lodos-wl.pid"
"${A[@]}" shell "XKB_CONFIG_ROOT=$RFS/usr/share/X11/xkb LODOS_WL_WIDTH=$W LODOS_WL_HEIGHT=$H LODOS_WL_DUMP=$RUN/T043.ppm nohup $L/bin/lodos-wl $RUN/lodos-wl >$RUN/lodos-wl.log 2>&1 </dev/null & echo \$! >$RUN/lodos-wl.pid"
cleanup() {
  set +e
  "${A[@]}" shell "p=\$(cat $RUN/lodos-wl.pid 2>/dev/null); [ -n \"\$p\" ] && kill \$p 2>/dev/null; rm -f $RUN/lodos-wl.pid"
  # Reap only daemons from this guest rootfs, including a session left after failure.
  EXTRA_BINDS="-b $RUN:/run/host-xdg" bash scripts/phone-run.sh 'pkill -x phoc 2>/dev/null || true; pkill -x phosh 2>/dev/null || true; pkill -x elogind 2>/dev/null || true; pkill -x dbus-daemon 2>/dev/null || true' >/dev/null 2>&1
}
trap cleanup EXIT INT TERM
for _ in $(seq 1 100); do
  "${A[@]}" shell "grep -q 'listening on abstract @lodos-lodos-wl' $RUN/lodos-wl.log" && break
  sleep .1
done
"${A[@]}" shell "grep -q 'listening on abstract @lodos-lodos-wl' $RUN/lodos-wl.log" || { "${A[@]}" shell cat "$RUN/lodos-wl.log"; exit 1; }
printf '[core]\nxwayland=false\n\n[output:WL-1]\nmode=%dx%d\nscale=1\n' "$W" "$H" >"$OUT/phoc.ini"
"${A[@]}" push "$OUT/phoc.ini" "$RFS/tmp/lodos-phone-phoc.ini" >/dev/null
set +e
bash scripts/phone-run.sh '/tmp/lodos-phone-phosh.sh' >"$OUT/guest.log" 2>&1 &
GUEST_PID=$!
set -e
for _ in $(seq 1 150); do
  if "${A[@]}" shell "grep -q 'Phosh ready after' $RFS/tmp/lodos-phone-desk/phoc-phosh.log 2>/dev/null"; then break; fi
  kill -0 "$GUEST_PID" 2>/dev/null || { wait "$GUEST_PID" || true; tail -40 "$OUT/guest.log"; exit 1; }
  sleep .2
done
sleep "${PHONE_DESKTOP_WAIT:-8}"
READY=$("${A[@]}" shell "grep 'Phosh ready after' $RFS/tmp/lodos-phone-desk/phoc-phosh.log 2>/dev/null | tail -1" | tr -d '\r' || true)
echo "phone phosh first-frame marker: ${READY:-not found}; wall=$(( $(date +%s) - T0 ))s" | tee "$OUT/timing.txt"
PID=$("${A[@]}" shell "cat $RUN/lodos-wl.pid" | tr -d '\r')
"${A[@]}" shell "kill -USR1 $PID"
for _ in $(seq 1 100); do "${A[@]}" shell "test -s $RUN/T043.ppm" && break; sleep .1; done
"${A[@]}" pull "$RUN/T043.ppm" "$OUT/T043-phone-phosh.ppm" >/dev/null
python3 -I scripts/ppm2png.py "$OUT/T043-phone-phosh.ppm" docs/screens/T043-phone-phosh.png
kill "$GUEST_PID" 2>/dev/null || true
wait "$GUEST_PID" 2>/dev/null || true
cleanup
trap - EXIT INT TERM
"${A[@]}" shell "pgrep -f 'phoc|phosh|elogind|dbus-daemon' 2>/dev/null || true"
echo "done: docs/screens/T043-phone-phosh.png"
