set -e
cd /mnt/SSD/LodOS/native/vk-bridge
source ../../orch/env.sh
rm -f ../../docs/screens/T047-wsi.png
w=$(timeout 20m bash test/host_wsi.sh 2>&1) || { echo "$w" | tail -30; exit 1; }
echo "$w" | tail -5
echo "$w" | grep -Eq 'VKB wsi ok frames=([3-9][0-9]|[0-9]{3,})'
n=$(echo "$w" | grep -Eo 'nonblack=[0-9]+' | tail -1 | cut -d= -f2); [ -n "$n" ] && [ "$n" -ge 10000 ]
test -s ../../docs/screens/T047-wsi.png
grep -q 'vkQueuePresentKHR' icd/*.c
z=$(timeout 20m bash test/host_zink.sh 2>&1) || { echo "$z" | tail -20; exit 1; }
echo "$z" | grep -q 'VKB zink triangle ok'
echo PASS
