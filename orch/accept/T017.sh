set -e
cd /mnt/SSD/LodOS
out=$(timeout 15m bash scripts/phone-app-probe.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -30
echo "$out" | grep -q 'LODOS_PROBE.*uid=1[0-9][0-9][0-9][0-9]'
echo "$out" | grep -qE 'LODOS_PROBE.*PATHSOCK (ok|fail)'
echo "$out" | grep -q 'LODOS_PROBE.*PROOT ok'
echo "$out" | grep -q 'LODOS_PROBE.*PROBE_DONE'
echo PASS
