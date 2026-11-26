set -e
cd /mnt/SSD/LodOS
out=$(timeout 40m bash scripts/guest-box64.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -q 'BOX64 hello ok'
echo "$out" | grep -q 'BOX64 bash ok 42'
echo "$out" | grep -qE 'BOX64 bench native=[0-9]+ box64=[0-9]+'
test -s docs/research/S4-box64.md
echo PASS
