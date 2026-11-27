set -e
cd /mnt/SSD/LodOS
source orch/env.sh
adb get-state >/dev/null 2>&1 || { echo "no phone"; exit 1; }
rm -f docs/screens/T043-phone-phosh.png
timeout 15m bash scripts/phone-desktop.sh >/tmp/T043.run.log 2>&1 || { tail -30 /tmp/T043.run.log; exit 1; }
python3 -I - docs/screens/T043-phone-phosh.png <<'PY'
import sys,zlib,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h=struct.unpack('>II',d[16:24]); assert w>=360 and h>=640,(w,h)
i=8;idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0]
  if d[i+4:i+8]==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
raw=zlib.decompress(idat); st=max(1,len(raw)//20000)
s={raw[k:k+3] for k in range(0,len(raw)-4,st)}; print('distinct~',len(s)); assert len(s)>=16
PY
adb shell 'pgrep -f phoc' >/dev/null 2>&1 && { echo "phoc left on phone"; exit 1; }
echo PASS
