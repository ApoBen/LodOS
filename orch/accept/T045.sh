set -e
cd /mnt/SSD/LodOS
source orch/env.sh
(cd native/lodos-init && timeout 10m cargo test -q 2>&1 | tail -5)
rm -f docs/screens/T045-init-phosh.png
timeout 6m bash scripts/pc-desktop.sh init-phosh
test -s docs/screens/T045-init-phosh.png
python3 -I -c "
import zlib,struct,sys
d=open('docs/screens/T045-init-phosh.png','rb').read()
assert d[:8]==b'\x89PNG\r\n\x1a\n'
w,h=struct.unpack('>II',d[16:24]); assert w>=360 and h>=640, (w,h)
idat=b''; i=8
while i<len(d):
    l=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]
    if t==b'IDAT': idat+=d[i+8:i+8+l]
    i+=12+l
raw=zlib.decompress(idat); assert len(set(raw[::97]))>8, 'flat image'
"
! pgrep -x 'lodos-init|phosh|phoc' >/dev/null
echo PASS
