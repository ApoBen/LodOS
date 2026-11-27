#!/usr/bin/env bash
# T023: Guest glibc Vulkan ICD v0 (libvulkan_lodos.so) -> vk-bridge -> bionic vkb_server.
# Builds vkb_server with NDK r30 (cmds_icd.txt), builds libvulkan_lodos.so inside Debian proot with
# guest gcc, installs /usr/share/vulkan/icd.d/lodos_icd.json, runs vulkaninfo --summary.
# env: LODOS_VK_SOCK (default @lodos-vk-g), NOBUILD=1 skip apt check
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
cd "$(dirname "$0")/.."
ROOT=$PWD
source orch/env.sh

SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
LODOS_VK_SOCK=${LODOS_VK_SOCK:-@lodos-vk-g}
SRC=$ROOT/native/vk-bridge
VKH=$ROOT/third_party/src/Vulkan-Headers
[ -f "$VKH/registry/vk.xml" ] || git clone -q --depth 1 --branch v1.4.369 https://github.com/KhronosGroup/Vulkan-Headers "$VKH"

OUT=$ROOT/build/vk-bridge/icd
GEN=$OUT/gen
L=/data/local/tmp/lodos
PDIR=$L/vkb-icd
GDIR_HOST=$L/rootfs/debian/root/vkb-icd
GDIR=/root/vkb-icd
ICD_JSON=/usr/share/vulkan/icd.d/lodos_icd.json
ICD_LIB=/usr/lib/aarch64-linux-gnu/libvulkan_lodos.so
R="bash scripts/phone-run.sh"

echo "== host: codegen (cmds_icd.txt) + bionic vkb_server (NDK r30)"
rm -rf "$OUT"
mkdir -p "$GEN" "$OUT/src"
python3 "$SRC/gen/gen.py" "$VKH/registry/vk.xml" "$SRC/gen/cmds_icd.txt" "$GEN"
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -I "$VKH/include" -I "$SRC/gen" -I "$GEN" -I "$SRC/client")
ACC="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
"$ACC" "${CFLAGS[@]}" -fPIE -pie \
    "$SRC/server/vkb_server.c" "$GEN/vkb_rpc_common.c" "$GEN/vkb_rpc_server.c" \
    "$SRC/transport/ring.c" "$SRC/transport/sock.c" -ldl -o "$OUT/vkb_server"

# Stage guest source tree for proot glibc gcc build
S=$OUT/src
mkdir -p "$S"/{icd,client,transport,gen,genout,include}
cp "$SRC"/icd/icd.c "$SRC"/icd/wsi_wayland.c "$SRC"/icd/icd.map "$SRC"/icd/lodos_icd.json "$S/icd/"
cp "$SRC"/client/vkbc.[ch] "$S/client/"
cp "$SRC"/transport/*.[ch] "$S/transport/"
cp "$SRC/gen/vkb_wire.h" "$S/gen/"
cp "$GEN"/*.[ch] "$S/genout/"
cp -r "$VKH/include/vulkan" "$VKH/include/vk_video" "$S/include/"
cat > "$S/build.sh" <<EOF
set -e
cd "\$(dirname "\$0")"
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -fPIC -shared \
    -Wl,--version-script=icd/icd.map \
    -I include -I gen -I genout -I client \
    icd/icd.c client/vkbc.c genout/vkb_rpc_common.c genout/vkb_rpc_client.c \
    transport/ring.c transport/sock.c -lpthread -ldl -o libvulkan_lodos.so
mkdir -p /usr/lib/aarch64-linux-gnu /usr/share/vulkan/icd.d
install -m 755 libvulkan_lodos.so "$ICD_LIB"
install -m 644 icd/lodos_icd.json "$ICD_JSON"
nm -D --defined-only "$ICD_LIB"
EOF

echo "== guest: ensure gcc, libc6-dev, libvulkan1, vulkan-tools"
if [ "${NOBUILD:-0}" != 1 ]; then
    $R '
    set -e
    if [ "$(dpkg -s gcc libc6-dev libvulkan1 vulkan-tools 2>/dev/null | grep -c "^Status: install ok installed")" -eq 4 ]; then
        echo "guest packages already installed"
        exit 0
    fi
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q >/dev/null
    apt-get install -y -q --no-install-recommends gcc libc6-dev libvulkan1 vulkan-tools >/dev/null
    echo "guest packages installed"
    '
fi

echo "== push sources + server"
adb -s "$SERIAL" shell "rm -rf $GDIR_HOST && mkdir -p $GDIR_HOST $PDIR"
adb -s "$SERIAL" push -q "$S/." "$GDIR_HOST/" >/dev/null
adb -s "$SERIAL" push -q "$OUT/vkb_server" "$PDIR/vkb_server-icd" >/dev/null

echo "== guest: build + install libvulkan_lodos.so and $ICD_JSON"
$R "sh $GDIR/build.sh"

echo "== run: bionic server (shell uid, LODOS_VK_SOCK=$LODOS_VK_SOCK) + guest vulkaninfo --summary"
adb -s "$SERIAL" shell "cd $PDIR && chmod 755 vkb_server-icd && \
    { [ -f srv-icd.pid ] && kill \$(cat srv-icd.pid) 2>/dev/null; rm -f srv-icd.pid; true; } && \
    (LODOS_VK_SOCK='$LODOS_VK_SOCK' ./vkb_server-icd >srv-icd.log 2>&1 & echo \$! >srv-icd.pid) && \
    for i in \$(seq 1 30); do grep -q 'listening' srv-icd.log 2>/dev/null && break; sleep 0.1; done"

rc=0
$R "LODOS_VK_SOCK='$LODOS_VK_SOCK' VK_ICD_FILENAMES='$ICD_JSON' vulkaninfo --summary" || rc=$?

adb -s "$SERIAL" shell "cd $PDIR; [ -f srv-icd.pid ] && kill \$(cat srv-icd.pid) 2>/dev/null; sleep 0.1; echo '-- server log'; cat srv-icd.log; rm -f srv-icd.pid"
exit $rc
