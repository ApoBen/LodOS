#!/usr/bin/env bash
# PC desktop bring-up: lodos-wl headless (host) -> phoc nested (guest, wlroots wayland backend, pixman) -> foot.
# usage: scripts/pc-desktop.sh [foot|phosh|phosh-home|init-phosh|apps|xwayland]
#   foot  (default): phoc -E foot running a colour demo -> docs/screens/T037-phoc-foot.png
#   phosh: guest system dbus + elogind, then session dbus + phoc -E phosh -> docs/screens/T038-phosh.png,
#          guest stderr (phoc/phosh) -> build/T038-phosh.log
#   phosh-home: phosh session + lodos-wl control socket (LODOS_WL_CTL, driven by tools/wlctl.py):
#          swipe-unlock (guest /etc/pam.d/phosh = pam_permit), app grid, tap foot ->
#          docs/screens/T042-phosh-{unlocked,grid,foot}.png, guest log -> build/T042-phosh.log
#   init-phosh: T045 guest boots via /sbin/lodos-init (scripts/pc-guest-init.sh units: dbus -> elogind ->
#          session bus -> phoc -> phosh) -> docs/screens/T045-init-phosh.png, then SIGTERM lodos-init and
#          check every guest proc exited (no orphans); lodos-init log -> build/T045-init-phosh.log
#   apps: T050 app smoke on the init-phosh boot: unlock, then per app (foot, gnome-text-editor, gnome-calculator)
#          launch from the app grid search, wait mapped (guest wlrctl foreign-toplevel app_ids), PNG
#          docs/screens/T050-<app>.png, close (wlrctl toplevel close), verify gone; prints `APP <name> ok`;
#          then the init-phosh clean shutdown. driver: scripts/pc-phosh-apps.sh; log -> build/T050-apps.log
#   xwayland: init-phosh boot with phoc Xwayland enabled, red xterm on phoc's selected DISPLAY, toplevel check and PNG
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
  init-phosh) PNG=docs/screens/T045-init-phosh.png; CLIENT=phosh; SETTLE=${PC_DESKTOP_WAIT:-8}
         PHOSH_LOG=$ROOT/build/T045-init-phosh.log;;
  apps)  PNG=; CLIENT=phosh; SETTLE=${PC_DESKTOP_WAIT:-8}
         PHOSH_LOG=$ROOT/build/T050-apps.log;;
  xwayland) PNG=docs/screens/T065-xterm.png; CLIENT=phosh; SETTLE=${PC_DESKTOP_WAIT:-8}
         PHOSH_LOG=$ROOT/build/T065-xwayland.log;;
  *) log "unknown mode $MODE"; exit 2;;
esac
[ -x "$RFS/usr/bin/phoc" ] || { log "guest missing phoc -> run scripts/pc-guest.sh setup"; exit 1; }
if [ "$MODE" != foot ] && ! [ -e "$RFS/usr/share/glib-2.0/schemas/org.gnome.settings-daemon.peripherals.gschema.xml" ]; then
  log "guest missing gnome-settings-daemon-common (phosh aborts without it) -> pc-guest.sh setup"
  bash scripts/pc-guest.sh setup >&2
fi
if [ "$MODE" = xwayland ] && ! bash scripts/pc-guest.sh run /bin/sh -c '
  for p in xwayland xterm x11-apps; do
    dpkg -s "$p" 2>/dev/null | grep -q "^Status: install ok installed" || exit 1
  done
'; then
  log "guest missing Xwayland/X11 packages -> pc-guest.sh setup"
  bash scripts/pc-guest.sh setup >&2
fi

log "building lodos-wl (headless)"
(cd native/lodos-wl && cargo build --release --features headless -q)
WL=$ROOT/native/lodos-wl/target/release/lodos-wl

mkdir -p "$RUN"; chmod 700 "$RUN"
CTL=$RUN/ctl
rm -f "$RUN/$SOCK" "$RUN/$SOCK.lock" "$OUT/fb.ppm" "$CTL"

WL_PID=; G_PID=; X_PID=
descendants() { local c; for c in $(pgrep -P "$1" 2>/dev/null); do echo "$c"; descendants "$c"; done; }
cleanup() {
  set +e
  if [ -n "$X_PID" ]; then
    kill -TERM -- -"$X_PID" 2>/dev/null
    for _ in $(seq 30); do kill -0 "$X_PID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL -- -"$X_PID" 2>/dev/null; wait "$X_PID" 2>/dev/null
  fi
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
  WL_PID=; G_PID=; X_PID=
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
XWAYLAND=false
[ "$MODE" = xwayland ] && XWAYLAND=true
printf '[core]\nxwayland=%s\n\n[output:WL-1]\nmode=%dx%d\nscale=1\n' "$XWAYLAND" "$W" "$H" >"$RFS/tmp/lodos-desk-phoc.ini"

GENV=()
[ "$MODE" = phosh-home ] && GENV=(LODOS_PHOSH_NOAUTH=1)
if [ "$MODE" = init-phosh ] || [ "$MODE" = apps ] || [ "$MODE" = xwayland ]; then
  bash scripts/pc-guest-init.sh
  # lockscreen auth always succeeds (guest-only dev setting, same as T042 LODOS_PHOSH_NOAUTH)
  printf 'auth sufficient pam_permit.so\naccount sufficient pam_permit.so\n' >"$RFS/etc/pam.d/phosh"
  mkdir -p "$RFS/tmp/lodos-desk"; rm -f "$RFS/tmp/lodos-desk/init.log"
  GCMD=(/sbin/lodos-init --target=lodos-phosh.target --log=/tmp/lodos-desk/init.log --stop-timeout-ms=3000)
elif [ "$MODE" != foot ]; then
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

if [ "$MODE" = apps ]; then
  bash scripts/pc-phosh-apps.sh "$CTL" "$W" "$H" "$OUT" "$RUN"
elif [ "$MODE" = xwayland ]; then
  # Unlock the init-managed phosh lockscreen (same swipe/keypad sequence as T050 apps).
  python3 -I tools/wlctl.py "$CTL" swipe "$((W / 2))" "$((H * 94 / 100))" "$((W / 2))" "$((H * 19 / 100))" 400
  sleep 1
  python3 -I tools/wlctl.py "$CTL" tap "$((W * 38 / 100))" "$((H * 464 / 1000))"
  sleep 0.3
  python3 -I tools/wlctl.py "$CTL" tap "$((W / 2))" "$((H * 682 / 1000))"
  sleep 3
  # Read DISPLAY from this session's phoc/phosh environment. Xwayland can be
  # started lazily, so there may be no Xwayland argv or socket to inspect yet.
  XDISPLAY=$(python3 -I - "$G_PID" <<'PY'
import glob, os, sys

root = int(sys.argv[1])
parents = {}
for path in glob.glob('/proc/[0-9]*/stat'):
    try:
        raw = open(path).read()
        parents[int(path.split('/')[2])] = int(raw[raw.rfind(')') + 2:].split()[1])
    except (OSError, ValueError, IndexError):
        pass
def in_session(pid):
    seen = set()
    while pid and pid not in seen:
        if pid == root: return True
        seen.add(pid); pid = parents.get(pid, 0)
    return False
for path in glob.glob('/proc/[0-9]*/environ'):
    try:
        pid = int(path.split('/')[2])
        if not in_session(pid): continue
        env = open(path, 'rb').read().split(b'\\0')
        display = next((x.split(b'=', 1)[1].decode() for x in env if x.startswith(b'DISPLAY=')), '')
        if display.startswith(':'):
            print(display); sys.exit(0)
    except (OSError, ValueError):
        pass
sys.exit(1)
PY
  ) || XDISPLAY=
  if [ -z "$XDISPLAY" ]; then
    for sock in "$RFS"/tmp/.X11-unix/X[0-9]*; do
      [ -S "$sock" ] || continue
      XDISPLAY=":${sock##*X}"
      break
    done
  fi
  # wlroots normally selects :0 for this isolated guest runtime. If phoc
  # doesn't publish DISPLAY, this is the first display to probe.
  XDISPLAY=${XDISPLAY:-:0}
  log "trying Xwayland display $XDISPLAY (no XAUTHORITY; lazy startup enabled)"
  setsid bash scripts/pc-guest.sh run /usr/bin/env XDG_RUNTIME_DIR=/run/host-xdg DISPLAY="$XDISPLAY" \
    xterm -title xterm -bg red -geometry 60x20 >"$OUT/xterm.log" 2>&1 &
  X_PID=$!
  wait_xterm() {
    local started=$SECONDS
    until XDG_RUNTIME_DIR=$RUN bash scripts/pc-guest.sh run /usr/bin/env WAYLAND_DISPLAY=wayland-0 \
        wlrctl toplevel list 2>/dev/null | grep -qi xterm; do
      kill -0 "$X_PID" 2>/dev/null || return 1
      [ $((SECONDS - started)) -lt 20 ] || return 1
      sleep 0.5
    done
    return 0
  }
  if ! wait_xterm; then
    log "xterm did not map on $XDISPLAY; trying standalone Xwayland :77"
    kill -TERM -- -"$X_PID" 2>/dev/null || true
    wait "$X_PID" 2>/dev/null || true
    setsid bash -c '
      bash scripts/pc-guest.sh run /usr/bin/env XDG_RUNTIME_DIR=/run/host-xdg WAYLAND_DISPLAY=wayland-0 \
        XDG_CONFIG_HOME=/tmp XDG_CACHE_HOME=/tmp Xwayland :77 -rootless -noreset &
      xp=$!
      trap "kill \$xp 2>/dev/null; wait \$xp 2>/dev/null" EXIT TERM INT
      sleep 1
      exec bash scripts/pc-guest.sh run /usr/bin/env XDG_RUNTIME_DIR=/run/host-xdg DISPLAY=:77 \
        xterm -title xterm -bg red -geometry 60x20
    ' >"$OUT/xterm-fallback.log" 2>&1 &
    X_PID=$!
    wait_xterm || { log "xterm failed on both phoc Xwayland and standalone :77"; cat "$OUT/xterm.log" "$OUT/xterm-fallback.log" >&2; exit 1; }
    log "standalone Xwayland fallback mapped xterm"
  fi
  log "xterm toplevel: $(XDG_RUNTIME_DIR=$RUN bash scripts/pc-guest.sh run /usr/bin/env WAYLAND_DISPLAY=wayland-0 wlrctl toplevel list 2>/dev/null | grep -i xterm | head -1)"
  python3 -I tools/wlctl.py "$CTL" dump "$OUT/T065-xterm.ppm"
  python3 -I scripts/ppm2png.py "$OUT/T065-xterm.ppm" "$PNG"
  echo "XWAYLAND ok"
else
  n0=$(grep -c "dumped PPM" "$OUT/lodos-wl.log" || true)
  kill -USR1 "$WL_PID"
  for _ in $(seq 50); do [ "$(grep -c "dumped PPM" "$OUT/lodos-wl.log" || true)" -gt "$n0" ] && break; sleep 0.1; done
  [ -s "$OUT/fb.ppm" ] || { log "no framebuffer dump"; cat "$OUT/lodos-wl.log" >&2; exit 1; }
  python3 -I scripts/ppm2png.py "$OUT/fb.ppm" "$PNG"
fi

if [ "$MODE" = init-phosh ] || [ "$MODE" = apps ] || [ "$MODE" = xwayland ]; then
  # clean shutdown: SIGTERM lodos-init -> reverse-order unit stop; proot exits once init exits
  gpids=$(descendants "$G_PID" | tr "\n" " " || true)
  ipid=; for p in $gpids; do [ "$(ps -o comm= -p "$p" 2>/dev/null || true)" = lodos-init ] && { ipid=$p; break; }; done
  [ -n "$ipid" ] || { log "lodos-init pid not found"; exit 1; }
  log "SIGTERM lodos-init (host pid $ipid); guest procs: $(echo $gpids | wc -w)"
  t1=$SECONDS; kill -TERM "$ipid"
  for _ in $(seq 300); do kill -0 "$G_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$G_PID" 2>/dev/null; then log "guest still running 30s after SIGTERM"; exit 1; fi
  wait "$G_PID" 2>/dev/null || true; G_PID=
  log "guest exited $((SECONDS - t1))s after SIGTERM"
  left=; for p in $gpids; do kill -0 "$p" 2>/dev/null && left+="$(ps -o pid=,comm= -p "$p" || echo "$p") "; done
  [ -z "$left" ] || { log "orphans left: $left"; exit 1; }
  ILOG=$RFS/tmp/lodos-desk/init.log
  grep -q "lodos-init exited cleanly" "$ILOG" || { log "lodos-init did not exit cleanly"; tail -20 "$ILOG" >&2; exit 1; }
  cleanup
  { cat "$ILOG"; echo "== guest stdout =="; cat "$OUT/guest.log"; } >"$PHOSH_LOG"; log "init log: $PHOSH_LOG"
  if pgrep -x 'lodos-init|phosh|phoc|gnome-calculator|gnome-text-edit' >/dev/null; then log "leftover guest procs"; exit 1; fi
  log "done: ${PNG:-docs/screens/T050-*.png}"
  exit 0
fi

cleanup
if [ -n "${PHOSH_LOG:-}" ]; then cp "$OUT/guest.log" "$PHOSH_LOG"; log "phosh log: $PHOSH_LOG"; fi
pgrep -x phoc >/dev/null && log "WARN: a phoc process is still running (not ours?)"
log "done: $PNG"
