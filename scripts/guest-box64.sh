#!/usr/bin/env bash
# Idempotent HOST-side driver: install box64 + x86_64 test binaries (/opt/x64root) in Debian trixie
# proot guest (shell uid), verify x86_64 hello & bash execution, and benchmark bash loop native vs box64.
# usage: scripts/guest-box64.sh   (needs scripts/guest-setup.sh done)
set -euo pipefail
cd "$(dirname "$0")/.."
source orch/env.sh

R="bash scripts/phone-run.sh"

# 1) Idempotent box64 install (trixie/main arm64; cross-lib deps avoid pulling libc6:amd64 into host rootfs)
$R '
set -e
export DEBIAN_FRONTEND=noninteractive
if ! command -v box64 >/dev/null 2>&1; then
  apt-get update -q
  if apt-cache show box64 >/dev/null 2>&1; then
    apt-get install -y -q --no-install-recommends box64 libgcc-s1-amd64-cross libstdc++6-amd64-cross
    apt-get clean
  else
    echo "box64 not in apt; building from source" >&2
    apt-get install -y -q --no-install-recommends git cmake make gcc g++ python3 ca-certificates
    rm -rf /tmp/box64-src
    git clone --depth 1 https://github.com/ptitSeb/box64.git /tmp/box64-src
    cmake -S /tmp/box64-src -B /tmp/box64-src/build -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
    make -C /tmp/box64-src/build -j8
    make -C /tmp/box64-src/build install
    rm -rf /tmp/box64-src
    apt-get clean
  fi
fi

# 2) Idempotent x86_64 test binaries extracted to /opt/x64root (no multiarch install into /)
if [ ! -x /opt/x64root/usr/bin/hello ] || [ ! -x /opt/x64root/bin/bash ]; then
  if ! dpkg --print-foreign-architectures | grep -qx amd64; then
    dpkg --add-architecture amd64
  fi
  apt-get update -q
  mkdir -p /tmp/x64debs /opt/x64root
  rm -f /tmp/x64debs/*.deb
  (cd /tmp/x64debs && apt-get download hello:amd64 bash:amd64)
  for d in /tmp/x64debs/*.deb; do
    dpkg-deb -x "$d" /opt/x64root
  done
  rm -rf /tmp/x64debs
  # Debian 13 usrmerge puts bash in ./usr/bin/bash; symlink /opt/x64root/bin -> usr/bin
  if [ ! -e /opt/x64root/bin ] && [ -d /opt/x64root/usr/bin ]; then
    ln -s usr/bin /opt/x64root/bin
  fi
fi

# 3) Version + functional checks + benchmark
export BOX64_NOBANNER=1
export BOX64_LD_LIBRARY_PATH=/opt/x64root/usr/lib/x86_64-linux-gnu:/opt/x64root/lib/x86_64-linux-gnu:/usr/x86_64-linux-gnu/lib
export BOX64_PATH=/opt/x64root/usr/bin:/opt/x64root/bin

ver=$(box64 -v 2>&1 | sed -n "s/.*v\([0-9][^ ]*\).*/\1/p" | head -n1)
echo "BOX64 version ${ver:-unknown}"

hello_out=$(box64 /opt/x64root/usr/bin/hello)
echo "hello_out: $hello_out"
case "$hello_out" in
  *Hello*|*hello*) echo "BOX64 hello ok" ;;
  *) echo "ERROR: unexpected hello output: [$hello_out]" >&2; exit 1 ;;
esac

bash_out=$(box64 /opt/x64root/bin/bash -c "echo \$((6*7))")
if [ "$bash_out" != "42" ]; then
  echo "ERROR: unexpected bash output: [$bash_out]" >&2
  exit 1
fi
echo "BOX64 bash ok 42"

loop_cmd="for ((i=1;i<=100000;i++)); do :; done"

t0=$(date +%s%3N)
/bin/bash -c "$loop_cmd"
t1=$(date +%s%3N)
native_ms=$((t1 - t0))

t0=$(date +%s%3N)
box64 /opt/x64root/bin/bash -c "$loop_cmd"
t1=$(date +%s%3N)
box64_ms=$((t1 - t0))

echo "BOX64 bench native=${native_ms} box64=${box64_ms}"
'
