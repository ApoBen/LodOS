#!/usr/bin/env bash
# T045: install lodos-init (musl x86_64, D3) + phosh session units into the PC guest rootfs.
# usage: scripts/pc-guest-init.sh            (idempotent; host side, no root)
# installs: /sbin/lodos-init, /bin/systemctl.lodos (shim, not on the default path),
#           /usr/local/lib/lodos/{wait-for,sysbus-prep},
#           /etc/systemd/system/{dbus,elogind,lodos-sessbus,lodos-phoc,lodos-phosh}.service
#           /etc/systemd/system/lodos-phosh.target.wants/* (enable links)
# run:   lodos-init --target=lodos-phosh.target (scripts/pc-desktop.sh init-phosh)
# Boot order (D3): dbus (system bus) -> elogind -> session bus -> phoc (nested on host lodos-wl,
# pixman, ini /tmp/lodos-desk-phoc.ini written by pc-desktop.sh) -> phosh (phoc's wayland-0).
# dbus/elogind override the stock units by name (etc/ beats usr/lib); /sbin/init is untouched.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RFS=$ROOT/build/pc-rootfs
[ -x "$RFS/usr/bin/phoc" ] || { echo "[pc-guest-init] guest missing phoc -> scripts/pc-guest.sh setup" >&2; exit 1; }

bash scripts/build-init.sh x86_64 >&2
install -m 755 build/init/x86_64/lodos-init "$RFS/sbin/lodos-init"
install -m 755 build/init/x86_64/systemctl "$RFS/bin/systemctl.lodos"

L=$RFS/usr/local/lib/lodos; U=$RFS/etc/systemd/system
mkdir -p "$L" "$U/lodos-phosh.target.wants"

# readiness helper: lodos-init treats simple/dbus units as up at spawn -> dependents poll in ExecStartPre
cat >"$L/wait-for" <<'EOF'
#!/bin/sh
# wait-for sock <path> | bus <system|session> <name>   [LODOS_WAIT_S, default 15]
n=$(( ${LODOS_WAIT_S:-15} * 10 ))
while [ "$n" -gt 0 ]; do
  case "$1" in
    sock) [ -S "$2" ] && exit 0;;
    bus)  busctl --"$2" list 2>/dev/null | grep -q "^$3 " && exit 0;;
    *) echo "wait-for: bad mode $1" >&2; exit 2;;
  esac
  sleep 0.1; n=$((n - 1))
done
echo "wait-for: timeout $*" >&2; exit 1
EOF
# system bus prep (T038 learnings): machine-id, stock system.conf minus <user>/<pidfile>, absolute includedir
cat >"$L/sysbus-prep" <<'EOF'
#!/bin/sh
set -e
mkdir -p /run/dbus /run/lodos /run/systemd/inhibit /var/lib/dbus
rm -f /run/dbus/system_bus_socket /run/dbus/pid /run/lodos/session_bus
grep -qE '^[0-9a-f]{32}$' /etc/machine-id 2>/dev/null || dbus-uuidgen >/etc/machine-id
ln -sf /etc/machine-id /var/lib/dbus/machine-id
sed -e '/<user>messagebus<\/user>/d' -e '/<pidfile>/d' \
    -e 's#<includedir>system.d</includedir>#<includedir>/usr/share/dbus-1/system.d</includedir>#' \
    /usr/share/dbus-1/system.conf >/run/lodos/system.conf
EOF
chmod 755 "$L/wait-for" "$L/sysbus-prep"

SYSBUS=DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
SESBUS=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/lodos/session_bus
DESK="GSETTINGS_BACKEND=memory XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=phosh"

cat >"$U/dbus.service" <<EOF
# lodos-init (T045) override of the stock unit: no socket activation, no messagebus user
[Unit]
Description=D-Bus System Message Bus (lodos)
[Service]
Type=simple
ExecStartPre=/usr/local/lib/lodos/sysbus-prep
ExecStart=/usr/bin/dbus-daemon --config-file=/run/lodos/system.conf --nofork --nopidfile
[Install]
WantedBy=lodos-phosh.target
EOF
cat >"$U/elogind.service" <<EOF
[Unit]
Description=elogind (lodos)
After=dbus.service
Requires=dbus.service
[Service]
Type=simple
Environment=$SYSBUS
ExecStartPre=/usr/local/lib/lodos/wait-for sock /run/dbus/system_bus_socket
ExecStart=/usr/libexec/elogind
[Install]
WantedBy=lodos-phosh.target
EOF
cat >"$U/lodos-sessbus.service" <<EOF
[Unit]
Description=D-Bus session bus for the phosh session (lodos)
After=dbus.service
[Service]
Type=simple
Environment=$SYSBUS
ExecStart=/usr/bin/dbus-daemon --session --address=unix:path=/run/lodos/session_bus --nofork --nopidfile
[Install]
WantedBy=lodos-phosh.target
EOF
cat >"$U/lodos-phoc.service" <<EOF
# WAYLAND_DISPLAY (host lodos-wl socket name), XDG_RUNTIME_DIR, WLR_* come from lodos-init's env
[Unit]
Description=phoc nested on host lodos-wl (lodos)
After=elogind.service lodos-sessbus.service
Requires=elogind.service lodos-sessbus.service
[Service]
Type=simple
Environment=$SYSBUS $SESBUS $DESK
ExecStartPre=/usr/local/lib/lodos/wait-for bus system org.freedesktop.login1
ExecStartPre=/usr/local/lib/lodos/wait-for sock /run/lodos/session_bus
ExecStartPre=/bin/sh -c 'rm -f "\$XDG_RUNTIME_DIR"/wayland-0 "\$XDG_RUNTIME_DIR"/wayland-0.lock; exec /usr/local/lib/lodos/wait-for sock "\$XDG_RUNTIME_DIR/\$WAYLAND_DISPLAY"'
ExecStart=/usr/bin/phoc -C /tmp/lodos-desk-phoc.ini
[Install]
WantedBy=lodos-phosh.target
EOF
cat >"$U/lodos-phosh.service" <<EOF
[Unit]
Description=phosh shell on phoc wayland-0 (lodos)
After=lodos-phoc.service
Requires=lodos-phoc.service
[Service]
Type=simple
Environment=$SYSBUS $SESBUS $DESK WAYLAND_DISPLAY=wayland-0
ExecStartPre=/bin/sh -c 'exec /usr/local/lib/lodos/wait-for sock "\$XDG_RUNTIME_DIR/wayland-0"'
ExecStart=/usr/libexec/phosh
[Install]
WantedBy=lodos-phosh.target
EOF

for u in dbus elogind lodos-sessbus lodos-phoc lodos-phosh; do
  ln -sfn "/etc/systemd/system/$u.service" "$U/lodos-phosh.target.wants/$u.service"
done
echo "[pc-guest-init] installed lodos-init + units (target lodos-phosh.target) into $RFS" >&2
