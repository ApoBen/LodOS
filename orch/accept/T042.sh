set -e
cd /mnt/SSD/LodOS
source orch/env.sh
rm -f docs/screens/T042-*.png
(cd native/lodos-wl && cargo test --features headless 2>&1 | tail -5) | grep -q "test result: ok" || { echo "cargo test fail"; exit 1; }
timeout 12m bash scripts/pc-desktop.sh phosh-home >/tmp/T042.run.log 2>&1 || { tail -30 /tmp/T042.run.log; exit 1; }
python3 -I - docs/screens/T042-phosh-unlocked.png docs/screens/T042-phosh-grid.png docs/screens/T038-phosh.png <<'PY'
import sys,zlib,struct
def px(p):
  d=open(p,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n',p
  w,h=struct.unpack('>II',d[16:24]); i=8; idat=b''
  while i<len(d):
    n=struct.unpack('>I',d[i:i+4])[0]
    if d[i+4:i+8]==b'IDAT': idat+=d[i+8:i+8+n]
    i+=12+n
  return w,h,zlib.decompress(idat)
u=px(sys.argv[1]); g=px(sys.argv[2]); l=px(sys.argv[3])
for w,h,r in (u,g): assert w>=360 and h>=640
def diff(a,b):
  n=min(len(a[2]),len(b[2])); st=max(1,n//20000)
  return sum(a[2][k]!=b[2][k] for k in range(0,n,st))/len(range(0,n,st))
d1=diff(u,l); d2=diff(g,u); print('unlocked-vs-lock',d1,'grid-vs-unlocked',d2)
assert d1>0.02,'unlocked == lockscreen'; assert d2>0.02,'grid == unlocked'
PY
test -s docs/screens/T042-phosh-foot.png
pgrep -x phoc >/dev/null && { echo "phoc left running"; exit 1; }
pgrep -x phosh >/dev/null && { echo "phosh left running"; exit 1; }
echo PASS
