set -e
set -o pipefail
cd /mnt/SSD/LodOS
source orch/env.sh
cmake -S native/lodos-fg -B build/lodos-fg -DCMAKE_BUILD_TYPE=Release >/tmp/T069.cmake.log 2>&1 || { tail -20 /tmp/T069.cmake.log; exit 1; }
cmake --build build/lodos-fg -j8 >/tmp/T069.build.log 2>&1 || { tail -20 /tmp/T069.build.log; exit 1; }
L="VK_LAYER_PATH=$PWD/build/lodos-fg VK_INSTANCE_LAYERS=VK_LAYER_LODOS_fg"
# 1 no layer
timeout 2m build/lodos-fg/present_headless 2>&1 | tee /tmp/T069.a.log
grep -q 'app extent 1280x720' /tmp/T069.a.log; grep -q 'presented 30' /tmp/T069.a.log
# 2 layer loaded but LODOS_FG unset -> passthrough
env $L timeout 2m build/lodos-fg/present_headless 2>&1 | tee /tmp/T069.b.log
grep -q 'app extent 1280x720' /tmp/T069.b.log; grep -q 'presented 30' /tmp/T069.b.log
# 3 layer enabled balanced -> ~753x424, mesa device_select layer active (default)
env $L LODOS_FG=1 LODOS_FG_PRESET=balanced timeout 2m build/lodos-fg/present_headless 2>&1 | tee /tmp/T069.c.log
grep -q 'LODOS_FG layer ok' /tmp/T069.c.log; grep -q 'presented 30' /tmp/T069.c.log
w=$(grep -Eo 'app extent [0-9]+x' /tmp/T069.c.log | tail -1 | grep -Eo '[0-9]+')
[ "$w" -ge 740 ] && [ "$w" -le 770 ]
grep -qi 'layer usage' native/lodos-fg/README.md
echo PASS
