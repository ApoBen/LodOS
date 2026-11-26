set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash scripts/phone-display-test.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -q 'DISPLAY green ok'
test -s /tmp/lodos-t022.png
echo PASS
