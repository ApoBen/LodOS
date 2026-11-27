#!/usr/bin/env bash
# T019: R1 proof. vk-bridge client built with guest glibc gcc INSIDE proot/Debian talks to the
# bionic vkb_server (T018, shell uid, outside proot) over abstract @lodos-vk + memfd SCM_RIGHTS.
# Steps: host gen.py + NDK server build -> push sources into guest -> guest gcc build -> run.
# usage: scripts/guest-vkbridge-test.sh   env: SOCK (default lodos-vk-g), NOBUILD=1 skip guest gcc install
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
cd "$(dirname "$0")/.."
ROOT=$PWD
source orch/env.sh
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
SRC=$ROOT/native/vk-bridge
VKH=$ROOT/third_party/src/Vulkan-Headers
[ -f "$VKH/registry/vk.xml" ] || git clone -q --depth 1 --branch v1.4.369 https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
OUT=$ROOT/build/vk-bridge/guest
GEN=$OUT/gen
L=/data/local/tmp/lodos
PDIR=$L/vkb-e2e
GDIR_HOST=$L/rootfs/debian/root/vkb   # same dir as seen from the device (shell uid owns rootfs)
GDIR=/root/vkb                         # inside guest
SOCK=${SOCK:-lodos-vk-g}
R="bash scripts/phone-run.sh"

echo "== host: gen + bionic server (NDK r30)"
rm -rf "$OUT"; mkdir -p "$GEN" "$OUT/src"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_e2e.txt" "$GEN" >/dev/null
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")
ACC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
"$ACC" "${CFLAGS[@]}" -fPIE -pie "$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" -ldl -o "$OUT/vkb_server"

# guest source tree (layout keeps relative includes like ../transport/ring.h working)
S=$OUT/src
mkdir -p "$S"/{client,transport,test,gen,genout,include}
cp "$SRC"/client/vkbc.[ch] "$S/client/"
cp "$SRC"/transport/*.[ch] "$S/transport/"
cp "$SRC/test/e2e_test.c" "$S/test/"
cp "$SRC/gen/vkb_wire.h" "$S/gen/"
cp "$GEN"/*.[ch] "$S/genout/"
cp -r "$VKH/include/vulkan" "$VKH/include/vk_video" "$S/include/"
cat > "$S/build.sh" <<'EOF'
set -e
cd "$(dirname "$0")"
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I include -I gen -I genout -I client \
  test/e2e_test.c client/vkbc.c genout/vkb_rpc_common.c genout/vkb_rpc_client.c transport/ring.c transport/sock.c \
  -o e2e_test-glibc
file e2e_test-glibc 2>/dev/null || true
ldd e2e_test-glibc | head -3
EOF

echo "== guest: gcc/libc6-dev (idempotent)"
if [ "${NOBUILD:-0}" != 1 ]; then
$R '
set -e
if dpkg -s gcc libc6-dev 2>/dev/null | grep -c "^Status: install ok installed" | grep -q 2; then echo "gcc already installed"; exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get update -q >/dev/null && apt-get install -y -q gcc libc6-dev >/dev/null && echo "gcc installed"
'
fi

echo "== push"
adb -s "$SERIAL" shell "rm -rf $GDIR_HOST && mkdir -p $GDIR_HOST $PDIR"
adb -s "$SERIAL" push -q "$S/." "$GDIR_HOST/" >/dev/null
adb -s "$SERIAL" push -q "$OUT/vkb_server" "$PDIR/vkb_server-g" >/dev/null

echo "== guest: build e2e_test with glibc gcc"
$R "sh $GDIR/build.sh && gcc --version | head -1 && ldd --version | head -1"

echo "== run: bionic server (shell uid, outside proot) + glibc client (inside proot)"
adb -s "$SERIAL" shell "cd $PDIR && chmod 755 vkb_server-g && { [ -f srv-g.pid ] && kill \$(cat srv-g.pid) 2>/dev/null; true; }; \
    (./vkb_server-g -s $SOCK >srv-g.log 2>&1 & echo \$! >srv-g.pid); sleep 0.3"
rc=0
$R "uname -m; $GDIR/e2e_test-glibc $SOCK" || rc=$?
adb -s "$SERIAL" shell "cd $PDIR; kill \$(cat srv-g.pid) 2>/dev/null; sleep 0.2; echo '-- server log'; cat srv-g.log; rm -f srv-g.pid"
exit $rc
