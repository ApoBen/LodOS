set -e
cd /mnt/SSD/LodOS
source orch/env.sh
out=$(bash scripts/guest-vkbridge-test.sh 2>&1); echo "$out" | tail -15
echo "$out" | grep -Eq 'E2E device=.*Mali'
echo "$out" | grep -q 'E2E ok'
echo PASS
