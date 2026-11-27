set -e
cd /mnt/SSD/LodOS
source orch/env.sh
cmake -S native/lodos-fg -B build/lodos-fg -DCMAKE_BUILD_TYPE=Release >/tmp/T069.cmake.log 2>&1 || { tail -20 /tmp/T069.cmake.log; exit 1; }
cmake --build build/lodos-fg -j8 >/tmp/T069.build.log 2>&1 || { tail -20 /tmp/T069.build.log; exit 1; }
ls build/lodos-fg/liblodos_fg.so >/dev/null
rm -f docs/screens/T069-*.png /tmp/T069-dump.png
# without layer: full extent
timeout 3m build/lodos-fg/present_headless | tee /tmp/T069.a.log
grep -q 'app extent 1280x720' /tmp/T069.a.log
# with layer: reduced extent + dump
LODOS_FG=1 LODOS_FG_PRESET=balanced LODOS_FG_DUMP=/tmp/T069-dump.png LODOS_FG_DUMP_FRAME=10 \
  VK_LAYER_PATH="$PWD/build/lodos-fg" VK_INSTANCE_LAYERS=VK_LAYER_LODOS_fg \
  timeout 3m build/lodos-fg/present_headless | tee /tmp/T069.b.log
grep -q 'LODOS_FG layer ok' /tmp/T069.b.log
w=$(grep -Eo 'app extent [0-9]+x' /tmp/T069.b.log | tail -1 | grep -Eo '[0-9]+')
[ "$w" -ge 740 ] && [ "$w" -le 770 ]
python3 -I - /tmp/T069-dump.png <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h=struct.unpack('>II',d[16:24]); assert (w,h)==(1280,720),(w,h)
PY
cp /tmp/T069-dump.png docs/screens/T069-layer.png
grep -qi 'layer usage' native/lodos-fg/README.md
echo PASS
