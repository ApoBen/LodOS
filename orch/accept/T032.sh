set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash native/vk-bridge/test/host_triangle.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -15
echo "$out" | grep -Eq 'VKB host triangle ok center=[0-9a-fx,()]+ corner=[0-9a-fx,()]+ calls=[0-9]+'
test -s build/vk-bridge/host-triangle/out.ppm
out2=$(timeout 20m bash native/vk-bridge/test/host_compute.sh 2>&1) || { echo "$out2" | tail -20; exit 1; }
echo "$out2" | grep -Eq 'VKB host compute ok'
echo PASS
