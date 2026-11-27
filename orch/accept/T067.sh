set -e
cd /mnt/SSD/LodOS
source orch/env.sh
test -s docs/research/S1-kopper-norender.md
if ls patches/mesa/*.patch >/dev/null 2>&1; then grep -qi 'upstreamable' patches/mesa/README.md; fi
rm -f docs/screens/T067-*.png
timeout 30m bash native/vk-bridge/test/host_zink_norender.sh >/tmp/T067.log 2>&1 || { tail -30 /tmp/T067.log; exit 1; }
grep -q 'NORENDER ok' /tmp/T067.log || { tail -20 /tmp/T067.log; exit 1; }
python3 -I -c 'import sys;d=open(sys.argv[1],"rb").read(8);assert d==b"\x89PNG\r\n\x1a\n"' docs/screens/T067-eglgears-norender.png
echo PASS
