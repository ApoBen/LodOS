set -e
cd /mnt/SSD/LodOS
out=$(timeout 30m bash scripts/guest-vkwork-test.sh 2>&1) || { echo "$out" | tail -40; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -qE 'VKB compute ok n=[0-9]+'
echo "$out" | grep -qE 'VKB triangle ok center=\(?255,0,0'
bash orch/accept/T023.sh >/dev/null 2>&1 || { echo "T023 regression"; exit 1; }
echo PASS
