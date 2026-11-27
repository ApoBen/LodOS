#!/bin/bash
# Guest-side (PC x86_64 trixie, proot -0) phosh session payload, run by scripts/pc-desktop.sh phosh.
# D3 sysvinit-less bring-up (T021 learnings, PC variant: path AF_UNIX sockets work, netlink ok -> no LD_PRELOAD shims):
#   system dbus (stock system.conf minus <user>/pidfile) -> elogind -> dbus-run-session phoc -E phosh.
# Daemons stay children of this shell (no --fork/daemonize) so proot --kill-on-exit / pgid kill reaps them.
# env in: WAYLAND_DISPLAY (host lodos-wl socket name), XDG_RUNTIME_DIR, WLR_* ; arg1: phoc ini path
set -u
INI=${1:-/tmp/lodos-desk-phoc.ini}
L=/tmp/lodos-desk
mkdir -p $L /run/dbus /run/systemd/inhibit /var/lib/dbus
rm -f /run/dbus/system_bus_socket /run/dbus/pid
# rootfs ships machine-id "uninitialized" -> gio/dbus refuse it
grep -qE '^[0-9a-f]{32}$' /etc/machine-id 2>/dev/null || dbus-uuidgen >/etc/machine-id
ln -sf /etc/machine-id /var/lib/dbus/machine-id

sed -e '/<user>messagebus<\/user>/d' -e '/<pidfile>/d' \
    -e 's#<includedir>system.d</includedir>#<includedir>/usr/share/dbus-1/system.d</includedir>#' \
    /usr/share/dbus-1/system.conf >$L/system.conf
dbus-daemon --config-file=$L/system.conf --nofork --nopidfile >$L/sysbus.log 2>&1 &
for _ in $(seq 50); do [ -S /run/dbus/system_bus_socket ] && break; sleep 0.1; done
if [ -S /run/dbus/system_bus_socket ]; then echo "LODOS system-bus ok"; else echo "LODOS system-bus FAIL"; cat $L/sysbus.log; fi
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket

/usr/libexec/elogind >$L/elogind.log 2>&1 &
for _ in $(seq 50); do busctl --system list 2>/dev/null | grep -q org.freedesktop.login1 && break; sleep 0.1; done
if busctl --system list 2>/dev/null | grep -q org.freedesktop.login1; then echo "LODOS login1 ok"
else echo "LODOS login1 FAIL"; cat $L/elogind.log; fi

export GSETTINGS_BACKEND=memory XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=phosh
echo "LODOS starting phoc -E phosh"
dbus-run-session -- phoc -C "$INI" -E /usr/libexec/phosh 2>&1
echo "LODOS phoc exited rc=$?"
echo "== elogind =="; cat $L/elogind.log; echo "== sysbus =="; cat $L/sysbus.log
