set -e
set -o pipefail
cd /mnt/SSD/LodOS
source orch/env.sh
rm -f docs/screens/T073-*.png
timeout 40m bash scripts/pc-phosh-gpu.sh >/tmp/T073.log 2>&1 || { tail -40 /tmp/T073.log; exit 1; }
r=$(grep -Eo 'GPU_RENDERER=.*' /tmp/T073.log | tail -1); echo "$r"
echo "$r" | grep -qiE 'zink|vulkan' || { echo "no GPU renderer"; exit 1; }
echo "$r" | grep -qiE 'llvmpipe|softpipe|pixman|swiftshader' && { echo "software renderer"; exit 1; }
python3 -I - docs/screens/T073-phosh-gpu.png <<'PY'
import sys,struct,zlib
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
i=8;idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0];t=d[i+4:i+8]
  if t==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
raw=zlib.decompress(idat); s=raw[::101]; nz=sum(1 for b in s if b); assert nz>len(s)*0.05,'black'
PY
pgrep -f '[v]kb_server|[p]hoc ' && { echo leftover; exit 1; }
test -s docs/research/S2-gpu-desktop.md
echo PASS
