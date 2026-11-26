#!/bin/sh
# Build vkprobe for aarch64 Android (API 29+) with NDK clang.
set -e
cd "$(dirname "$0")"
. ../../orch/env.sh
aarch64-linux-android29-clang -O2 -Wall -Wextra -Wno-unused-parameter -o vkprobe vkprobe.c -ldl
echo built: $(pwd)/vkprobe
