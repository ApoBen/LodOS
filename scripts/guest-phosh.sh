#!/usr/bin/env bash
# Idempotent: install phosh stack in Debian guest (proot on phone) + smoke-probe binaries.
# usage: scripts/guest-phosh.sh   (needs scripts/guest-setup.sh done)
# elogind Conflicts: systemd (trixie; dbus-user-session needs systemd) -> tried via apt -s, dropped if unsolvable.
# R13 (shell uid: no path AF_UNIX in /data/local/tmp) workarounds, guest-only:
#  - session dbus: /usr/share/dbus-1/session.conf diverted, listen unix:tmpdir -> unix:abstract=/tmp/lodos-dbus-session
#  - wayland: LD_PRELOAD shim native/unix2abstract (path under $LODOS_U2A_PREFIX -> abstract) -> /usr/local/lib/lodos/
# Logs: /tmp/lodos-guest-phosh.log (host)
set -euo pipefail
cd "$(dirname "$0")/.."
R="bash scripts/phone-run.sh"
L=/data/local/tmp/lodos
SERIAL=${SERIAL:-<phone-serial>}
LOG=${LOG:-/tmp/lodos-guest-phosh.log}
# gnome-settings-daemon-common: schemas phosh aborts without (peripherals.touchscreen); wayland-utils: probe
PKGS="phosh phoc squeekboard gnome-session-bin dbus-user-session gnome-settings-daemon-common wayland-utils"
OPT="elogind"
source orch/env.sh
: > "$LOG"

du_rootfs() { adb -s "$SERIAL" shell "du -sk $L/rootfs/debian 2>/dev/null" | tail -1 | awk '{print $1}'; }

t0=$(date +%s); kb0=$(du_rootfs)
$R "
set -e
missing=''
for p in $PKGS; do dpkg -s \$p 2>/dev/null | grep -q '^Status: install ok installed' || missing=\"\$missing \$p\"; done
if [ -z \"\$missing\" ]; then echo 'phosh pkgs already installed'; exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
want=\"\$missing\"
for o in $OPT; do
  if apt-get install -s -y -q --no-install-recommends \$missing \$o >/tmp/opt-\$o.sim 2>&1; then want=\"\$want \$o\"
  else echo \"OPT-SKIP \$o: apt conflict:\"; grep -E 'Conflicts|Depends|^E:' /tmp/opt-\$o.sim | head -8; fi
done
echo \"installing:\$want\"
apt-get install -y -q --no-install-recommends \$want
apt-get clean
" 2>&1 | tee -a "$LOG"
t1=$(date +%s); kb1=$(du_rootfs)
echo "install: $((t1-t0))s, rootfs ${kb0}K -> ${kb1}K (+$(( (kb1-kb0)/1024 ))M)" | tee -a "$LOG"

# guest fixups (idempotent)
$R '
set -e
# machine-id: rootfs ships "uninitialized" -> phoc/gio refuse to autolaunch dbus
grep -qE "^[0-9a-f]{32}$" /etc/machine-id 2>/dev/null || dbus-uuidgen > /etc/machine-id
mkdir -p /var/lib/dbus; [ -e /var/lib/dbus/machine-id ] || ln -s /etc/machine-id /var/lib/dbus/machine-id
# phosh lives in /usr/libexec (started by phosh-session); expose on PATH for probes
[ -e /usr/local/bin/phosh ] || ln -s /usr/libexec/phosh /usr/local/bin/phosh
# session bus: abstract socket (path socket bind = EACCES at shell uid)
C=/usr/share/dbus-1/session.conf
if ! dpkg-divert --list "$C" | grep -q lodos; then
  dpkg-divert --local --rename --divert "$C.lodos-orig" --add "$C"
fi
sed "s|<listen>unix:tmpdir=/tmp</listen>|<listen>unix:abstract=/tmp/lodos-dbus-session</listen>|" "$C.lodos-orig" > "$C"
grep -q "unix:abstract=/tmp/lodos-dbus-session" "$C"
echo fixups-ok
' 2>&1 | tee -a "$LOG"

# unix2abstract shim (host NDK clang, header-free, guest glibc target)
SO=build/u2a/libunix2abstract.so
SRC=native/unix2abstract/unix2abstract.c
if [ ! -f "$SO" ] || [ "$SRC" -nt "$SO" ]; then
  mkdir -p build/u2a
  clang --target=aarch64-linux-gnu -O2 -Wall -fPIC -shared -nostdlib -fno-builtin -fuse-ld=lld -o "$SO" "$SRC"
fi
adb -s "$SERIAL" shell "mkdir -p $L/rootfs/debian/usr/local/lib/lodos"
adb -s "$SERIAL" push "$SO" $L/rootfs/debian/usr/local/lib/lodos/ >/dev/null

# probes (never fail the script)
$R '
echo "== phoc --version"; phoc --version 2>&1 | head -3; echo "rc=$?"
echo "== phosh --help"; ls -l /usr/libexec/phosh /usr/bin/phosh 2>&1; (phosh --help 2>&1 || /usr/libexec/phosh --help 2>&1) | head -1
echo "== squeekboard"; command -v squeekboard || ls /usr/bin/squeekboard 2>&1
echo "== gnome-session"; command -v gnome-session
echo "== dbus session"; dbus-daemon --session --fork --print-address; echo "rc=$?"; pkill -x dbus-daemon; sleep 0.3
mkdir -p /tmp/xdg && chmod 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
echo "== headless phoc (plain)"
timeout 8 phoc -E "sleep 3" > /tmp/phoc-headless.log 2>&1
echo "phoc-headless-plain rc=$?"; grep -vE "^$" /tmp/phoc-headless.log | tail -5
echo "== headless phoc + unix2abstract shim + wayland-info"
LD_PRELOAD=/usr/local/lib/lodos/libunix2abstract.so timeout 10 phoc -E "sh -c \"wayland-info | grep -c interface:; sleep 1\"" > /tmp/phoc-headless-shim.log 2>&1
echo "phoc-headless-shim rc=$?"; grep -vE "^$" /tmp/phoc-headless-shim.log | tail -6
echo "== phosh in headless phoc (shim)"
LD_PRELOAD=/usr/local/lib/lodos/libunix2abstract.so dbus-run-session -- timeout 15 phoc -E "sh -c \"phosh & sleep 10; wayland-info | grep -E phosh_private; kill -0 \\\$! && echo phosh-alive\"" > /tmp/phosh-headless.log 2>&1
echo "phosh-headless rc=$?"; grep -vE "^$" /tmp/phosh-headless.log | tail -15
true
' 2>&1 | tee -a "$LOG" || true
echo "guest-phosh done (log $LOG)"
