set -e
set -o pipefail
cd /mnt/SSD/LodOS
source orch/env.sh
cmake -S native/lodos-fg -B build/lodos-fg -DCMAKE_BUILD_TYPE=Release >/tmp/T071.cmake.log 2>&1 || { tail -20 /tmp/T071.cmake.log; exit 1; }
cmake --build build/lodos-fg -j8 >/tmp/T071.build.log 2>&1 || { tail -20 /tmp/T071.build.log; exit 1; }
ls build/lodos-fg/liblodos_fg.so >/dev/null
rm -f docs/screens/T071-*.png /tmp/T071-dump.png
# without layer: full extent
timeout 3m build/lodos-fg/present_headless | tee /tmp/T071.a.log
grep -q 'app extent 1280x720' /tmp/T071.a.log
# with layer: reduced extent + dump
LODOS_FG=1 LODOS_FG_PRESET=balanced LODOS_FG_DUMP=/tmp/T071-dump.png LODOS_FG_DUMP_FRAME=10 \
  VK_LAYER_PATH="$PWD/build/lodos-fg" VK_INSTANCE_LAYERS=VK_LAYER_LODOS_fg \
  timeout 3m build/lodos-fg/present_headless | tee /tmp/T071.b.log
grep -q 'LODOS_FG layer ok' /tmp/T071.b.log
w=$(grep -Eo 'app extent [0-9]+x' /tmp/T071.b.log | tail -1 | grep -Eo '[0-9]+')
[ "$w" -ge 740 ] && [ "$w" -le 770 ]
python3 -I - /tmp/T071-dump.png <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h=struct.unpack('>II',d[16:24]); assert (w,h)==(1280,720),(w,h)
import zlib
i=8;idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0];t=d[i+4:i+8]
  if t==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
raw=zlib.decompress(idat); nz=sum(1 for b in raw[::97] if b); assert nz>len(raw[::97])*0.1,'dump looks black'
PY
cp /tmp/T071-dump.png docs/screens/T071-layer.png
grep -qi 'layer usage' native/lodos-fg/README.md
echo PASS
