set -e
cd /mnt/SSD/LodOS
out=$(bash scripts/phone-wl-test.sh 2>&1)
echo "$out" | tail -25
echo "$out" | grep -q "'wl_compositor'"
echo "$out" | grep -q "'xdg_wm_base'"
echo "$out" | grep -q "'wl_shm'"
echo PASS
