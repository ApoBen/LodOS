set -e
cd /mnt/SSD/LodOS/native/vk-bridge
source ../../orch/env.sh
out=$(timeout 20m bash test/host_syncfd.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -5
echo "$out" | grep -q 'VKB syncfd ok'
z=$(timeout 20m bash test/host_zink.sh 2>&1) || { echo "$z" | tail -30; exit 1; }
echo "$z" | grep -q 'VKB zink triangle ok'
m=$(echo "$z" | grep -Eo 'gdpa_misses=[0-9]+' | tail -1 | cut -d= -f2)
[ -n "$m" ] && [ "$m" -le 370 ]
grep -q 'vkGetSemaphoreFdKHR' gen/cmds_icd.txt
echo PASS
