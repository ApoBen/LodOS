set -e
cd /mnt/SSD/LodOS/native/vk-bridge
source ../../orch/env.sh
s=$(timeout 20m bash test/host_syncfd.sh 2>&1) || { echo "$s" | tail -30; exit 1; }
echo "$s" | grep -q 'VKB syncfd ok'
z=$(timeout 20m bash test/host_zink.sh 2>&1) || { echo "$z" | tail -30; exit 1; }
echo "$z" | grep -q 'VKB zink triangle ok'
m=$(echo "$z" | grep -Eo 'gdpa_misses=[0-9]+' | tail -1 | cut -d= -f2)
echo "gdpa_misses=$m"
[ -n "$m" ] && [ "$m" -le 250 ]
grep -q 'GDPA gaps after T063' ../../docs/research/S1-gpu-bridge.md
echo PASS
