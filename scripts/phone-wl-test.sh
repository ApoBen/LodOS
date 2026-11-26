#!/usr/bin/env bash
# T013: run lodos-wl (headless smithay Wayland server) on phone as shell uid; guest wayland-info connects
# via socket dir bind-mounted into proot (/data/local/tmp/lodos/run -> /run/lodos).
# usage: scripts/phone-wl-test.sh   (builds if needed)
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-<phone-serial>}
L=/data/local/tmp/lodos
RUN=$L/run
A="adb -s $SERIAL"
BIN=build/rs/arm64/lodos-wl

if [ ! -x "$BIN" ] || [ -n "$(find native/lodos-wl/src native/lodos-wl/Cargo.toml -newer "$BIN" 2>/dev/null)" ]; then
  bash scripts/build-xkbcommon.sh
  bash scripts/build-rs.sh native/lodos-wl
fi

$A shell "mkdir -p $L/bin $RUN && chmod 700 $RUN"
$A push "$BIN" $L/bin/lodos-wl >/dev/null
$A shell "chmod 755 $L/bin/lodos-wl"

stop() { $A shell "pid=\$(cat $RUN/lodos-wl.pid 2>/dev/null); [ -n \"\$pid\" ] && kill \$pid 2>/dev/null; rm -f $RUN/lodos-wl.pid" || true; }
trap stop EXIT
stop
$A shell "cd $L && (nohup $L/bin/lodos-wl $RUN/wayland-0 > $RUN/lodos-wl.log 2>&1 & echo \$! > $RUN/lodos-wl.pid)"
# shell uid (HyperOS SELinux) can't create sock_file in /data/local/tmp -> path socket may be absent;
# server then only has abstract @lodos-wayland-0.
for i in $(seq 1 50); do $A shell "grep -q listening $RUN/lodos-wl.log" && break; sleep 0.1; done
$A shell "grep -q listening $RUN/lodos-wl.log" || { echo "server not listening"; $A shell cat $RUN/lodos-wl.log; exit 1; }
echo "== server up:"; $A shell cat $RUN/lodos-wl.log

export EXTRA_BINDS="-b $RUN:/run/lodos"
set +e
bash scripts/phone-run.sh 'command -v wayland-info >/dev/null || { apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq wayland-utils >/dev/null; }
command -v python3 >/dev/null || { apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-minimal >/dev/null; }
export XDG_RUNTIME_DIR=/run/lodos WAYLAND_DISPLAY=wayland-0
if [ -S /run/lodos/wayland-0 ]; then exec wayland-info; fi
# abstract-socket fallback: pre-connected fd via WAYLAND_SOCKET (libwayland-client)
exec python3 -c "
import os, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(\"\\0lodos-wayland-0\")
s.set_inheritable(True)
os.environ[\"WAYLAND_SOCKET\"] = str(s.fileno())
os.execvp(sys.argv[1], sys.argv[1:])
" wayland-info'
rc=$?
echo "== server log:"
$A shell cat $RUN/lodos-wl.log
exit $rc
