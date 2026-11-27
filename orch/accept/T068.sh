set -e
cd /mnt/SSD/LodOS
source orch/env.sh
ls native/lodos-fg/third_party/*/LICENSE* >/dev/null
cmake -S native/lodos-fg -B build/lodos-fg -DCMAKE_BUILD_TYPE=Release >/tmp/T068.cmake.log 2>&1 || { tail -20 /tmp/T068.cmake.log; exit 1; }
cmake --build build/lodos-fg -j8 >/tmp/T068.build.log 2>&1 || { tail -20 /tmp/T068.build.log; exit 1; }
rm -f docs/screens/T068-*.png
timeout 5m build/lodos-fg/fsr1_offscreen | tee /tmp/T068.log
grep -q 'FSR1 ok' /tmp/T068.log
python3 -I -c 'import sys;d=open(sys.argv[1],"rb").read(8);assert d==b"\x89PNG\r\n\x1a\n"' docs/screens/T068-fsr1.png
echo PASS
