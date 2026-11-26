set -e
cd /mnt/SSD/LodOS
source orch/env.sh
bash scripts/build-vk-layer.sh
SO=build/vk-layer/arm64/libvulkan_lodos_mali.so
RE=$(ls "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-readelf 2>/dev/null | head -1); RE=${RE:-readelf}
need=$($RE -d $SO | grep NEEDED); echo "$need"
! echo "$need" | grep -q libcutils
n=$($RE --dyn-syms -W $SO | awk '$5=="GLOBAL" && $7!="UND"' | wc -l); echo "exports=$n"
[ "$n" -le 16 ]
$RE --dyn-syms -W $SO | grep -q vk_icdGetInstanceProcAddr
out=$(bash tools/vklayer-smoke/run.sh 2>&1); echo "$out" | tail -5
echo "$out" | grep -q 'BC1_DECODE=ok'
echo PASS
