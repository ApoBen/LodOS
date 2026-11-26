set -e
cd /mnt/SSD/LodOS
out=$(timeout 30m bash scripts/phone-guest-display-test.sh 2>&1) || { echo "$out" | tail -40; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -qE 'GUEST DISPLAY ok client=[^ ]+ frames=([2-9]|[1-9][0-9]+)'
bash orch/accept/T022.sh >/dev/null 2>&1 || { echo "T022 regression"; exit 1; }
echo PASS
