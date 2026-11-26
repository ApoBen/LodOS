set -e
cd /mnt/SSD/LodOS
source orch/env.sh
out=$(bash scripts/guest-vkicd-test.sh 2>&1); echo "$out" | tail -25
echo "$out" | grep -Eq 'deviceName *= *Mali-G720'
echo "$out" | grep -Eq 'driverName|driverID'
! echo "$out" | grep -Eqi 'segmentation fault|ERROR_INCOMPATIBLE_DRIVER'
echo PASS
