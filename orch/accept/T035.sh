set -e
cd /mnt/SSD/LodOS
out=$(timeout 25m bash native/vk-bridge/test/host_zink.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -15
echo "$out" | grep -Eq 'VKB zink triangle ok renderer=.*zink.* center=0x[0-9a-f]+ corner=0x[0-9a-f]+'
echo "$out" | grep -Eq 'calls=[1-9][0-9]*'
for s in host_triangle host_compute; do
  o=$(timeout 20m bash native/vk-bridge/test/$s.sh 2>&1) || { echo "$o" | tail -20; exit 1; }
  echo "$o" | grep -Eq 'VKB host (triangle|compute) ok'
done
echo PASS
