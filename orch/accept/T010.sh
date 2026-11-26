set -e
cd /mnt/SSD/LodOS
source orch/env.sh
bash scripts/build-vk-layer.sh
file build/vk-layer/arm64/libvulkan_lodos_mali.so | grep -q 'ARM aarch64'
grep -qi bionic-vulkan-wrapper THIRD_PARTY.md
out=$(bash tools/vklayer-smoke/run.sh 2>&1)
echo "$out" | tail -20
echo "$out" | grep -q 'deviceName=.*Mali'
echo "$out" | grep -q 'textureCompressionBC=1'
echo "$out" | grep -q 'BC1_DECODE=ok'
echo PASS
