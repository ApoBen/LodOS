set -e
cd /mnt/SSD/LodOS
out=$(bash native/vk-bridge/build-host.sh 2>&1)
echo "$out" | tail -15
n=$(echo "$out" | grep -c '^ROUNDTRIP vk[A-Za-z0-9]* ok$')
test "$n" -ge 10
echo "$out" | grep -q '^ROUNDTRIP_ALL ok n='
grep -q 'vkCreateInstance' build/vk-bridge/gen/vkb_cmds.h
echo PASS
