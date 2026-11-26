#!/usr/bin/env bash
# Run a shell command inside the Debian guest on phone via proot (adb shell, shell uid).
# usage: scripts/phone-run.sh '<cmd>'   env EXTRA_BINDS='-b host:guest ...' adds proot binds
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh
SERIAL=${SERIAL:-<phone-serial>}
L=/data/local/tmp/lodos
CMD=${1:?usage: phone-run.sh '<cmd>'}
# single-quote-escape cmd for the device shell
q=$(printf '%s' "$CMD" | sed "s/'/'\\\\''/g")
# /dev/shm: android has none; POSIX sem_open/shm_open (python multiprocessing) need it
adb -s "$SERIAL" shell "mkdir -p $L/shm && chmod 1777 $L/shm && cd $L && PROOT_LOADER=$L/proot/libproot-loader.so PROOT_TMP_DIR=$L/tmp \
  $L/proot/libproot.so -0 -r $L/rootfs/debian --link2symlink --sysvipc --kill-on-exit \
  -b /dev -b $L/shm:/dev/shm -b /proc -b /sys ${EXTRA_BINDS:-} -w /root \
  /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin TERM=xterm /bin/sh -c '$q'"
