#!/usr/bin/env bash
# Idempotent HOST-side driver: swap guest systemd-sysv -> sysvinit-core + elogind + dbus-x11 (D3),
# configure & start system dbus + elogind (with preload shims for Android shell-uid limits),
# verify org.freedesktop.login1, and re-run headless phoc+phosh (10s) to count login1 warnings.
# usage: scripts/guest-init.sh   (needs scripts/guest-setup.sh + scripts/guest-phosh.sh done)
# Logs: /tmp/lodos-guest-init.log (host)
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh

R="bash scripts/phone-run.sh"
L=/data/local/tmp/lodos
SERIAL=${SERIAL:-<phone-serial>}
LOG=${LOG:-/tmp/lodos-guest-init.log}
: > "$LOG"

# A regular directory is writable by Android's shell uid and is exposed as a
# cgroup2-shaped tree by the elogind preload shim (statfs + control files).
CGROUP_HOST=$L/fake-cgroup
adb -s "$SERIAL" shell "mkdir -p $CGROUP_HOST && chmod 0777 $CGROUP_HOST && : > $CGROUP_HOST/cgroup.controllers && : > $CGROUP_HOST/cgroup.procs && : > $CGROUP_HOST/cgroup.subtree_control && chmod 0666 $CGROUP_HOST/cgroup.*"
export EXTRA_BINDS="-b $CGROUP_HOST:/sys/fs/cgroup"

# Chosen systemd-free package set (Debian 13 trixie arm64):
#   install: sysvinit-core elogind libpam-elogind dbus-x11 libglib2.0-bin
#            (pulls initscripts insserv startpar sysv-rc systemd-standalone-sysusers)
#   remove:  systemd systemd-sysv dbus-user-session libpam-systemd
#            (also removes libnss-systemd systemd-resolved; keeps libsystemd0 which elogind uses)
WANT_PKGS="sysvinit-core elogind libpam-elogind dbus-x11 libglib2.0-bin"
DROP_PKGS="systemd systemd-sysv dbus-user-session libpam-systemd"

t0=$(date +%s)
$R "
set -e
need_apt=0
for p in $WANT_PKGS; do
  dpkg -s \$p 2>/dev/null | grep -q '^Status: install ok installed' || need_apt=1
done
for p in $DROP_PKGS; do
  if dpkg -s \$p 2>/dev/null | grep -q '^Status: install ok installed'; then need_apt=1; fi
done
if [ \"\$need_apt\" -eq 0 ]; then
  echo 'elogind/sysvinit pkgs already configured (systemd-free)'
  exit 0
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
drop_args=''
for p in $DROP_PKGS; do drop_args=\"\$drop_args \${p}-\"; done
echo \"swapping systemd -> elogind: +[$WANT_PKGS] -[$DROP_PKGS]\"
apt-get install -y --allow-remove-essential --no-install-recommends $WANT_PKGS \$drop_args
apt-get clean
" 2>&1 | tee -a "$LOG"
t1=$(date +%s)
echo "pkg-swap: $((t1-t0))s" | tee -a "$LOG"

# Build & push guest preload shims (NDK clang, header-free, guest glibc aarch64):
#  1) libunix2abstract.so: AF_UNIX path under $LODOS_U2A_PREFIX -> abstract (R13 shell-uid workaround)
#  2) libelogind_shim.so:  NETLINK_KOBJECT_UEVENT EACCES -> idle AF_UNIX SOCK_DGRAM (Android SELinux)
U2A_SO=build/u2a/libunix2abstract.so
U2A_SRC=native/unix2abstract/unix2abstract.c
if [ ! -f "$U2A_SO" ] || [ "$U2A_SRC" -nt "$U2A_SO" ]; then
  mkdir -p build/u2a
  clang --target=aarch64-linux-gnu -O2 -Wall -fPIC -shared -nostdlib -fno-builtin -fuse-ld=lld \
    -o "$U2A_SO" "$U2A_SRC"
fi

ESHIM_SO=build/elogind-shim/libelogind_shim.so
ESHIM_SRC=native/elogind-shim/elogind_shim.c
if [ ! -f "$ESHIM_SO" ] || [ "$ESHIM_SRC" -nt "$ESHIM_SO" ]; then
  mkdir -p build/elogind-shim
  clang --target=aarch64-linux-gnu -O2 -Wall -fPIC -shared -nostdlib -fno-builtin -fuse-ld=lld \
    -o "$ESHIM_SO" "$ESHIM_SRC"
fi

adb -s "$SERIAL" shell "mkdir -p $L/rootfs/debian/usr/local/lib/lodos"
adb -s "$SERIAL" push "$U2A_SO" "$ESHIM_SO" "$L/rootfs/debian/usr/local/lib/lodos/" >/dev/null

# Guest fixups (idempotent): machine-id + system.conf & session.conf abstract socket diversions
$R '
set -e
grep -qE "^[0-9a-f]{32}$" /etc/machine-id 2>/dev/null || dbus-uuidgen > /etc/machine-id
mkdir -p /var/lib/dbus; [ -e /var/lib/dbus/machine-id ] || ln -s /etc/machine-id /var/lib/dbus/machine-id

# system.conf: abstract socket (R13) + drop <user>messagebus</user> (proot cannot capset/drop caps)
SC=/usr/share/dbus-1/system.conf
if ! dpkg-divert --list "$SC" | grep -q lodos; then
  dpkg-divert --local --rename --divert "$SC.lodos-orig" --add "$SC"
fi
sed -e "s|<listen>unix:path=/run/dbus/system_bus_socket</listen>|<listen>unix:abstract=/run/dbus/system_bus_socket</listen>|" \
    -e "s|<user>messagebus</user>|<!-- user removed for proot -->|" \
    "$SC.lodos-orig" > "$SC"
grep -q "unix:abstract=/run/dbus/system_bus_socket" "$SC"

# session.conf: keep abstract socket diversion from T016 if needed
SESC=/usr/share/dbus-1/session.conf
if ! dpkg-divert --list "$SESC" | grep -q lodos; then
  dpkg-divert --local --rename --divert "$SESC.lodos-orig" --add "$SESC"
fi
sed "s|<listen>unix:tmpdir=/tmp</listen>|<listen>unix:abstract=/tmp/lodos-dbus-session</listen>|" "$SESC.lodos-orig" > "$SESC"

echo "init-fixups-ok"
' 2>&1 | tee -a "$LOG"

# Start system bus + elogind, run login1 checks, then run 10s headless phoc+phosh probe
$R '
set -u
DPID=""
EPID=""
cleanup() {
  [ -n "$EPID" ] && kill "$EPID" 2>/dev/null || true
  [ -n "$DPID" ] && kill "$DPID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mkdir -p /run/dbus /run/systemd /tmp/xdg
mkdir -p /run/systemd/inhibit
# A prior proot run can leave runtime records after its daemon is killed; the
# referenced PIDs no longer exist and elogind aborts while restoring the stale
# session. This init owns the isolated guest daemon, so discard those records.
mkdir -p /run/systemd/sessions /run/systemd/seats
rm -f /run/systemd/sessions/* /run/systemd/seats/* /run/elogind.pid
chmod 700 /tmp/xdg
rm -f /run/dbus/pid

export DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=/run/dbus/system_bus_socket

# 1. System dbus
dbus-daemon --system --nofork --print-address > /tmp/lodos-sysbus.log 2>&1 &
DPID=$!
sys_ok=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if busctl --system list >/tmp/lodos-busctl-pre.log 2>&1; then
    sys_ok=1
    break
  fi
  sleep 0.2
done

if [ "$sys_ok" -eq 1 ]; then
  echo "INIT system-bus ok"
else
  err=$(tr "\n" " " < /tmp/lodos-sysbus.log | head -c 160)
  echo "INIT system-bus fail $err"
  exit 1
fi

# 2. elogind daemon (with netlink + unix2abstract preload shims)
LODOS_U2A_PREFIX=/tmp/xdg:/run/dbus:/run/systemd \
  LD_PRELOAD=/usr/local/lib/lodos/libelogind_shim.so:/usr/local/lib/lodos/libunix2abstract.so \
  /usr/libexec/elogind > /tmp/lodos-elogind.log 2>&1 &
EPID=$!

login1_ok=0
login1_err=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  if busctl --system list 2>/dev/null | grep -q "org.freedesktop.login1" \
     && gdbus call --system -d org.freedesktop.login1 -o /org/freedesktop/login1 -m org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1; then
    login1_ok=1
    break
  fi
  sleep 0.2
done

echo "== busctl --system list =="
busctl --system list 2>&1 || true
echo "== gdbus ping login1 =="
ping_out=$(gdbus call --system -d org.freedesktop.login1 -o /org/freedesktop/login1 -m org.freedesktop.DBus.Peer.Ping 2>&1) || true
echo "$ping_out"
echo "== loginctl list-sessions =="
loginctl list-sessions 2>&1 || true
echo "== loginctl list-seats =="
loginctl list-seats 2>&1 || true
echo "== elogind stderr =="
cat /tmp/lodos-elogind.log

if [ "$login1_ok" -eq 1 ]; then
  echo "INIT login1 ok"
else
  login1_err=$(tr "\n" " " < /tmp/lodos-elogind.log | head -c 160)
  [ -z "$login1_err" ] && login1_err="$ping_out"
  echo "INIT login1 fail $login1_err"
  exit 1
fi

# Owning the bus name precedes seat initialization. Wait for the manager
# seat query to succeed before PAM asks it to create a session.
seats_ok=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  if loginctl list-seats >/dev/null 2>&1; then
    seats_ok=1
    break
  fi
  sleep 0.2
done
if [ "$seats_ok" -ne 1 ]; then
  echo "INIT seat-list fail $(cat /tmp/lodos-elogind.log | tr "\n" " " | head -c 160)"
  exit 1
fi

# PAM opens the elogind session; the cgroup shim creates the writable scope
# directory and its minimal controller files under the fake cgroup bind.
su -l root -c "sleep 2" >/tmp/lodos-su-session.log 2>&1 || true
sessions=$(loginctl list-sessions --no-legend 2>/dev/null || true)
if [ -n "$sessions" ]; then
  sid=${sessions%% *}
  # Keep the synthetic /proc membership aligned with the scope elogind
  # actually created; the daemon shim reads this file on each proc cgroup open.
  printf 'session-%s.scope\n' "$sid" > /run/systemd/lodos-session-scope
  echo "INIT session ok $sid"
else
  echo "INIT session fail $(tr "\n" " " < /tmp/lodos-su-session.log | head -c 120)"
fi

# elogind creates inhibitor state here; ensure the runtime directory exists,
# then request a short-lived idle inhibitor while this shell command runs.
su -l root -c "DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=/run/dbus/system_bus_socket busctl --system call org.freedesktop.login1 /org/freedesktop/login1 \
  org.freedesktop.login1.Manager Inhibit ssss idle lodos verify block" \
  >/tmp/lodos-inhibit.log 2>&1 && inhibit_ok=1 || inhibit_ok=0
if [ "$inhibit_ok" -ne 1 ] && command -v elogind-inhibit >/dev/null 2>&1; then
  su -l root -c "elogind-inhibit --what=idle true" >>/tmp/lodos-inhibit.log 2>&1 && inhibit_ok=1 || true
elif [ "$inhibit_ok" -ne 1 ] && command -v systemd-inhibit >/dev/null 2>&1; then
  su -l root -c "systemd-inhibit --what=idle true" >>/tmp/lodos-inhibit.log 2>&1 && inhibit_ok=1 || true
fi
if [ "${inhibit_ok:-0}" -eq 1 ]; then
  echo "INIT inhibit ok"
else
  echo "INIT inhibit fail $(tr "\n" " " < /tmp/lodos-inhibit.log | head -c 120)"
fi

# 3. Re-run phosh 10s inside headless phoc with system bus + elogind active
export XDG_RUNTIME_DIR=/tmp/xdg WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export LODOS_U2A_PREFIX=/tmp/xdg:/run/dbus
echo "== phosh 10s in headless phoc (with system bus + elogind) =="
LD_PRELOAD=/usr/local/lib/lodos/libunix2abstract.so \
  dbus-run-session -- timeout 15 phoc -E "sh -c \"phosh & sleep 10; kill -0 \\\$! && echo phosh-alive\"" \
  > /tmp/lodos-phosh-elogind.log 2>&1 || true
grep -vE "^$" /tmp/lodos-phosh-elogind.log | tail -20 || true
warn_n=$(grep -ci "login1" /tmp/lodos-phosh-elogind.log || true)
echo "INIT phosh-login1-warn=$warn_n"
' 2>&1 | tee -a "$LOG"

echo "guest-init done (log $LOG)"
