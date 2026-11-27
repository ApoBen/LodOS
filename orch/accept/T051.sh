set -e
cd /mnt/SSD/LodOS/native/vk-bridge
source ../../orch/env.sh
rm -f ../../docs/screens/T051-zink-wsi.png
w=$(timeout 20m bash test/host_zinkwsi.sh 2>&1) || { echo "$w" | tail -30; exit 1; }
echo "$w" | tail -5
echo "$w" | grep -Eq 'VKB zinkwsi ok frames=([3-9][0-9]|[0-9]{3,}) renderer=.*zink'
test -s ../../docs/screens/T051-zink-wsi.png
python3 -I - ../../docs/screens/T051-zink-wsi.png <<'PY'
import sys,zlib,struct
d=open(sys.argv[1],'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h=struct.unpack('>II',d[16:24]); i=8; idat=b''
while i<len(d):
  n=struct.unpack('>I',d[i:i+4])[0]
  if d[i+4:i+8]==b'IDAT': idat+=d[i+8:i+8+n]
  i+=12+n
r=zlib.decompress(idat); st=max(1,len(r)//20000)
vals=set(r[k] for k in range(0,len(r),st)); print('distinct',len(vals))
assert len(vals)>=16,'flat image'
PY
w2=$(timeout 10m bash test/host_wsi.sh 2>&1) || { echo "$w2" | tail -20; exit 1; }
echo "$w2" | grep -q 'VKB wsi ok'
echo PASS
