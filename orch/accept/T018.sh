set -e
cd /mnt/SSD/LodOS
source orch/env.sh
out=$(bash native/vk-bridge/build-e2e.sh 2>&1); echo "$out" | tail -15
echo "$out" | grep -Eq 'E2E device=.*Mali'
echo "$out" | grep -Eq 'qfam=[1-9]'
echo "$out" | grep -q 'E2E ok'
echo PASS
