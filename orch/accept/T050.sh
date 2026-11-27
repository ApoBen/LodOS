set -e
cd /mnt/SSD/LodOS
source orch/env.sh
rm -f docs/screens/T050-*.png
(cd native/lodos-wl && cargo test --features headless 2>&1 | tail -5) | grep -q "test result: ok" || { echo "cargo test fail"; exit 1; }
timeout 15m bash scripts/pc-desktop.sh apps >/tmp/T050.run.log 2>&1 || { tail -30 /tmp/T050.run.log; exit 1; }
for a in foot gnome-text-editor gnome-calculator; do grep -q "APP $a ok" /tmp/T050.run.log || { echo "no APP $a ok"; exit 1; }; done
python3 -I - docs/screens/T050-*.png <<'PY'
import sys,zlib,struct
def px(p):
  d=open(p,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n',p
  w,h=struct.unpack('>II',d[16:24]); i=8; idat=b''
  while i<len(d):
    n=struct.unpack('>I',d[i:i+4])[0]
    if d[i+4:i+8]==b'IDAT': idat+=d[i+8:i+8+n]
    i+=12+n
  assert w>=360 and h>=640; return zlib.decompress(idat)
ims=[px(p) for p in sys.argv[1:]]; assert len(ims)==3, len(ims)
def diff(a,b):
  n=min(len(a),len(b)); st=max(1,n//20000)
  return sum(a[k]!=b[k] for k in range(0,n,st))/len(range(0,n,st))
for i in range(3):
  for j in range(i+1,3):
    d=diff(ims[i],ims[j]); print(i,j,d); assert d>0.02,'identical screens'
PY
! pgrep -x 'lodos-init|phosh|phoc|gnome-calculator|gnome-text-edit' >/dev/null
echo PASS
