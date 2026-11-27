#!/bin/sh
# Phone guest session payload. Host lodos-wl is exposed via /run/host-xdg.
set -eu
R=/tmp/lodos-phone-desk
mkdir -p "$R" /run/dbus /run/systemd/inhibit /var/lib/dbus
rm -f /run/dbus/pid
export DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=/run/dbus/system_bus_socket
export LODOS_U2A_PREFIX=/run/host-xdg:/run/systemd

dbus-daemon --system --nofork --nopidfile >"$R/system-bus.log" 2>&1 &
DBUS_PID=$!
cleanup() {
  kill "$PHOC_PID" "$ELOGIND_PID" "$DBUS_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
PHOC_PID= ELOGIND_PID=
trap cleanup EXIT INT TERM
for _ in $(seq 1 100); do
  [ -S /run/dbus/system_bus_socket ] || sleep 0.1
  busctl --system list >/dev/null 2>&1 && break
done
LD_PRELOAD=/usr/local/lib/lodos/libelogind_shim.so:/usr/local/lib/lodos/libunix2abstract.so \
  /usr/libexec/elogind >"$R/elogind.log" 2>&1 &
ELOGIND_PID=$!
for _ in $(seq 1 100); do
  busctl --system list 2>/dev/null | grep -q org.freedesktop.login1 && break
  sleep 0.1
done
export XDG_RUNTIME_DIR=/run/host-xdg
export GSETTINGS_BACKEND=memory XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=phosh
export WAYLAND_DISPLAY=lodos-wl
unset WLR_WAYLAND_DISPLAY
export WLR_BACKENDS=wayland WLR_RENDERER=pixman WLR_NO_HARDWARE_CURSORS=1
export LD_PRELOAD=/usr/local/lib/lodos/libunix2abstract.so
echo "PHONE_PHOSH_START $(date +%s)"
dbus-run-session -- phoc -C /tmp/lodos-phone-phoc.ini -E /usr/libexec/phosh >"$R/phoc-phosh.log" 2>&1 &
PHOC_PID=$!
wait "$PHOC_PID"
