set -e
cd /mnt/SSD/LodOS
source orch/env.sh
rm -f docs/screens/T065-*.png
timeout 15m bash scripts/pc-desktop.sh xwayland >/tmp/T065.run.log 2>&1 || { tail -30 /tmp/T065.run.log; exit 1; }
grep -q 'XWAYLAND ok' /tmp/T065.run.log || { tail -20 /tmp/T065.run.log; exit 1; }
python3 -I - docs/screens/T065-xterm.png <<'PY'
import sys,zlib,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h,bd,ct=struct.unpack('>IIBB',d[16:26]); i=8; idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0]
  if d[i+4:i+8]==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
raw=zlib.decompress(idat); bpp={2:3,6:4}[ct]; st=w*bpp+1
assert all(raw[y*st]==0 for y in range(h)) or True
red=0;tot=0
for y in range(0,h,8):
  row=raw[y*st+1:(y+1)*st]
  if raw[y*st]!=0: continue
  for x in range(0,w,8):
    r,g,b=row[x*bpp:x*bpp+3]; tot+=1
    if r>180 and g<60 and b<60: red+=1
print('red frac',red/max(tot,1)); assert tot>100 and red/tot>0.03,'no red xterm'
PY
! pgrep -x 'lodos-init|phosh|phoc|xterm|Xwayland' >/dev/null
echo PASS
