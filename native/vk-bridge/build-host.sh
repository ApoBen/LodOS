#!/usr/bin/env bash
# host-only build+test of vk-bridge codegen: gen.py -> build/vk-bridge/gen, gcc, run roundtrip.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VKH="$ROOT/third_party/src/Vulkan-Headers"
TAG=v1.4.369
if [ ! -f "$VKH/registry/vk.xml" ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-Headers "$VKH"
fi
OUT="$ROOT/build/vk-bridge"
GEN="$OUT/gen"
rm -rf "$GEN"; mkdir -p "$GEN"
python3 "$ROOT/native/vk-bridge/gen/gen.py" "$VKH/registry/vk.xml" "$ROOT/native/vk-bridge/gen/cmds.txt" "$GEN"
${CC:-gcc} -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -I "$VKH/include" -I "$GEN" \
    "$GEN/vkb_marshal.c" "$ROOT/native/vk-bridge/test/roundtrip.c" -o "$OUT/roundtrip"
"$OUT/roundtrip"
