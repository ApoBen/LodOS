set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash scripts/guest-init.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -q 'INIT login1 ok'
echo "$out" | grep -qE 'INIT session ok [A-Za-z0-9]+'
echo "$out" | grep -q 'INIT inhibit ok'
grep -qi cgroup docs/research/S3-phosh-guest.md
echo PASS
