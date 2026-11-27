set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash native/vk-bridge/test/host_compute.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -20
echo "$out" | grep -Eq 'VKB host compute ok n=[0-9]+ calls=[0-9]+'
bash native/vk-bridge/build-host.sh >/dev/null 2>&1
echo PASS
