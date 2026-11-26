set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash scripts/phone-app-rootfs.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -q 'LODOS_PROBE.*uid=1[0-9][0-9][0-9][0-9]'
echo "$out" | grep -qE 'LODOS_PROBE.*RFS debian_version ok.*13'
echo "$out" | grep -q 'LODOS_PROBE.*RFS apt-install ok'
echo "$out" | grep -q 'LODOS_PROBE.*RFS hello ok.*Hello, world'
echo "$out" | grep -qE 'LODOS_PROBE.*DMAHEAP (ok|fail)'
echo "$out" | grep -q 'LODOS_PROBE.*RFS_DONE'
echo PASS
