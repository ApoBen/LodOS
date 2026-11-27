set -e
cd /mnt/SSD/LodOS
source orch/env.sh
rm -f docs/screens/T038-phosh.png
timeout 10m bash scripts/pc-desktop.sh phosh >/tmp/T038.run.log 2>&1 || { tail -30 /tmp/T038.run.log; exit 1; }
f=docs/screens/T038-phosh.png
test -s $f
python3 -I - "$f" <<'PY'
import sys,zlib,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h,bd,ct=struct.unpack('>IIBB',d[16:26]); assert w>=360 and h>=640,(w,h)
i=8;idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0];t=d[i+4:i+8]
  if t==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
raw=zlib.decompress(idat); s=set()
step=max(1,len(raw)//20000)
for k in range(0,len(raw)-4,step): s.add(raw[k:k+3])
print('size',w,h,'distinct~',len(s)); assert len(s)>=16,'framebuffer too uniform'
PY
pgrep -x phoc >/dev/null && { echo "phoc left running"; exit 1; }
pgrep -x phosh >/dev/null && { echo "phosh left running"; exit 1; }
grep -qi phosh build/T038-phosh.log
echo PASS
