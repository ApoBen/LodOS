set -e
cd /mnt/SSD/LodOS
source orch/env.sh
out=$(bash native/vk-bridge/build-transport.sh 2>&1); echo "$out" | tail -12
[ "$(echo "$out" | grep -c 'TRANSPORT ok')" -ge 2 ]
[ "$(echo "$out" | grep -c 'FDPASS ok')" -ge 2 ]
echo "$out" | grep -q 'LAT_US p50='
echo PASS
