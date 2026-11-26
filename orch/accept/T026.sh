set -e
cd /mnt/SSD/LodOS
out=$(ROOTFS=rootfs2 FRESH=1 timeout 20m bash scripts/phone-app-rootfs.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
ms=$(echo "$out" | grep -oE 'RFS extract ok native [0-9]+' | tail -1 | awk '{print $NF}')
test -n "$ms" && test "$ms" -lt 60000
echo "$out" | grep -q 'LODOS_PROBE.*RFS apt-install ok'
echo "$out" | grep -q 'LODOS_PROBE.*RFS hello ok.*Hello, world'
echo PASS
