#!/usr/bin/env bash
# PC desktop bring-up: lodos-wl headless (host) -> phoc nested (guest, wlroots wayland backend, pixman) -> foot.
# usage: scripts/pc-desktop.sh [foot|phosh|phosh-home]
#   foot  (default): phoc -E foot running a colour demo -> docs/screens/T037-phoc-foot.png
#   phosh: guest system dbus + elogind, then session dbus + phoc -E phosh -> docs/screens/T038-phosh.png,
#          guest stderr (phoc/phosh) -> build/T038-phosh.log
#   phosh-home: phosh session + lodos-wl control socket (LODOS_WL_CTL, driven by tools/wlctl.py):
#          swipe-unlock (guest /etc/pam.d/phosh = pam_permit), app grid, tap foot ->
#          docs/screens/T042-phosh-{unlocked,grid,foot}.png, guest log -> build/T042-phosh.log
# out:   PNG above (+ build/pc-desktop/{fb.ppm,lodos-wl.log,guest.log})
# env:   PC_DESKTOP_W/H (720x1600), PC_DESKTOP_WAIT (s after client maps; 4 foot / 8 phosh), PC_DESKTOP_TIMEOUT (60)
#        PC_DESKTOP_HOLD=1 (phosh-home: skip the scripted gestures, keep session up until killed; ctl at $RUN/ctl)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODE=${1:-foot}
W=${PC_DESKTOP_W:-720}; H=${PC_DESKTOP_H:-1600}
TMO=${PC_DESKTOP_TIMEOUT:-60}
OUT=$ROOT/build/pc-desktop; mkdir -p "$OUT" docs/screens
RFS=$ROOT/build/pc-rootfs
# private runtime dir: guest sees it as /run/host-xdg, so phoc's own wayland-N socket can't collide with the host session
BASE=${XDG_RUNTIME_DIR:-/tmp/lodos-run-$(id -u)}
RUN=$BASE/lodos-desk
SOCK=lodos-wl
log() { echo "[pc-desktop] $*" >&2; }

case "$MODE" in
  foot)  PNG=docs/screens/T037-phoc-foot.png; CLIENT=foot;  SETTLE=${PC_DESKTOP_WAIT:-4};;
  phosh) PNG=docs/screens/T038-phosh.png;     CLIENT=phosh; SETTLE=${PC_DESKTOP_WAIT:-8}
         PHOSH_LOG=$ROOT/build/T038-phosh.log;;
  phosh-home) PNG=; CLIENT=phosh; SETTLE=${PC_DESKTOP_WAIT:-8}
         PHOSH_LOG=$ROOT/build/T042-phosh.log;;
  *) log "unknown mode $MODE"; exit 2;;
esac
[ -x "$RFS/usr/bin/phoc" ] || { log "guest missing phoc -> run scripts/pc-guest.sh setup"; exit 1; }
if [ "$MODE" != foot ] && ! [ -e "$RFS/usr/share/glib-2.0/schemas/org.gnome.settings-daemon.peripherals.gschema.xml" ]; then
  log "guest missing gnome-settings-daemon-common (phosh aborts without it) -> pc-guest.sh setup"
  bash scripts/pc-guest.sh setup >&2
fi

log "building lodos-wl (headless)"
(cd native/lodos-wl && cargo build --release --features headless -q)
WL=$ROOT/native/lodos-wl/target/release/lodos-wl

mkdir -p "$RUN"; chmod 700 "$RUN"
CTL=$RUN/ctl
rm -f "$RUN/$SOCK" "$RUN/$SOCK.lock" "$OUT/fb.ppm" "$CTL"

WL_PID=; G_PID=
descendants() { local c; for c in $(pgrep -P "$1" 2>/dev/null); do echo "$c"; descendants "$c"; done; }
cleanup() {
  set +e
  if [ -n "$G_PID" ]; then
    local pids; pids="$G_PID $(descendants "$G_PID")"
    kill -TERM -- -"$G_PID" $pids 2>/dev/null
    for _ in $(seq 50); do kill -0 $pids 2>/dev/null || break; sleep 0.1; done
    kill -KILL -- -"$G_PID" $pids 2>/dev/null
    wait "$G_PID" 2>/dev/null
  fi
  if [ -n "$WL_PID" ]; then
    kill -TERM "$WL_PID" 2>/dev/null
    for _ in $(seq 30); do kill -0 "$WL_PID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$WL_PID" 2>/dev/null; wait "$WL_PID" 2>/dev/null
  fi
  rm -f "$RUN/$SOCK" "$RUN/$SOCK.lock" "$CTL"
  WL_PID=; G_PID=
}
trap cleanup EXIT
trap 'exit 130' INT TERM

log "lodos-wl headless ${W}x${H} on $RUN/$SOCK"
LODOS_WL_WIDTH=$W LODOS_WL_HEIGHT=$H LODOS_WL_DUMP=$OUT/fb.ppm LODOS_WL_CTL=$CTL "$WL" "$RUN/$SOCK" >"$OUT/lodos-wl.log" 2>&1 &
WL_PID=$!
for _ in $(seq 50); do [ -S "$RUN/$SOCK" ] && break; sleep 0.1; done
[ -S "$RUN/$SOCK" ] || { log "lodos-wl socket missing"; cat "$OUT/lodos-wl.log" >&2; exit 1; }

# foot payload: visible proof (text + 256-colour blocks), then stay alive until killed
cat >"$RFS/tmp/lodos-desk-demo.sh" <<'EOF'
#!/bin/sh
printf '\033[1mLodOS T037\033[0m  phoc (wlroots wayland/pixman) nested on lodos-wl headless\n'
printf 'kernel: %s  WAYLAND_DISPLAY=%s\n\n' "$(uname -srm)" "$WAYLAND_DISPLAY"
for row in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  for col in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    printf '\033[48;5;%dm    ' $((row * 16 + col))
  done
  printf '\033[0m\n'
done
printf '\n'
for c in 31 32 33 34 35 36; do printf '\033[1;%dm  colour %d  ' "$c" "$c"; done
printf '\033[0m\n\n'
ls -l --color=always /usr/bin | head -40
exec sleep 100000
EOF
chmod 755 "$RFS/tmp/lodos-desk-demo.sh"
# phoc 0.46 ignores wlroots output request_state (host configure size) -> pin the nested output mode via config
printf '[core]\nxwayland=false\n\n[output:WL-1]\nmode=%dx%d\nscale=1\n' "$W" "$H" >"$RFS/tmp/lodos-desk-phoc.ini"

GENV=()
[ "$MODE" = phosh-home ] && GENV=(LODOS_PHOSH_NOAUTH=1)
if [ "$MODE" != foot ]; then
  install -m 755 scripts/pc-phosh-session.sh "$RFS/tmp/lodos-desk-phosh.sh"
  GCMD=(/tmp/lodos-desk-phosh.sh /tmp/lodos-desk-phoc.ini)
else
  GCMD=(phoc -C /tmp/lodos-desk-phoc.ini -E "foot /tmp/lodos-desk-demo.sh")
fi
log "guest: $MODE"
(
  unset WAYLAND_DISPLAY DISPLAY WAYLAND_SOCKET
  export XDG_RUNTIME_DIR=$RUN
  exec setsid bash scripts/pc-guest.sh run /usr/bin/env \
    WAYLAND_DISPLAY=$SOCK WLR_BACKENDS=wayland WLR_RENDERER=pixman WLR_NO_HARDWARE_CURSORS=1 "${GENV[@]}" \
    "${GCMD[@]}"
) >"$OUT/guest.log" 2>&1 &
G_PID=$!

# wait until the client is mapped inside phoc and phoc has committed frames to lodos-wl
t0=$SECONDS
until descendants "$G_PID" | xargs -r ps -o comm= -p 2>/dev/null | grep -qx "$CLIENT" \
      && grep -q "new xdg_toplevel" "$OUT/lodos-wl.log"; do
  kill -0 "$G_PID" 2>/dev/null || { log "guest exited early"; tail -30 "$OUT/guest.log" >&2; exit 1; }
  [ $((SECONDS - t0)) -lt "$TMO" ] || { log "timeout waiting for $CLIENT"; tail -30 "$OUT/guest.log" >&2; exit 1; }
  sleep 0.5
done
log "$CLIENT up after $((SECONDS - t0))s; settling ${SETTLE}s"
sleep "$SETTLE"

if [ "$MODE" = phosh-home ]; then
  bash scripts/pc-phosh-home.sh "$CTL" "$W" "$H" "$OUT"
  cleanup
  cp "$OUT/guest.log" "$PHOSH_LOG"; log "phosh log: $PHOSH_LOG"
  pgrep -x phoc >/dev/null && log "WARN: a phoc process is still running (not ours?)"
  log "done: docs/screens/T042-phosh-*.png"
  exit 0
fi

n0=$(grep -c "dumped PPM" "$OUT/lodos-wl.log" || true)
kill -USR1 "$WL_PID"
for _ in $(seq 50); do [ "$(grep -c "dumped PPM" "$OUT/lodos-wl.log" || true)" -gt "$n0" ] && break; sleep 0.1; done
[ -s "$OUT/fb.ppm" ] || { log "no framebuffer dump"; cat "$OUT/lodos-wl.log" >&2; exit 1; }
python3 -I scripts/ppm2png.py "$OUT/fb.ppm" "$PNG"

cleanup
if [ -n "${PHOSH_LOG:-}" ]; then cp "$OUT/guest.log" "$PHOSH_LOG"; log "phosh log: $PHOSH_LOG"; fi
pgrep -x phoc >/dev/null && log "WARN: a phoc process is still running (not ours?)"
log "done: $PNG"
