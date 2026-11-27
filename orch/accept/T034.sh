set -e
cd /mnt/SSD/LodOS
source orch/env.sh
cd native/lodos-wl
out=$(timeout 15m cargo test -q --features headless 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -10
n=$(echo "$out" | grep -Eo '[0-9]+ passed' | awk '{s+=$1} END{print s+0}')
[ "$n" -ge 2 ]
grep -rqiE 'touch' tests src
cd ../..
timeout 15m bash scripts/build-rs.sh native/lodos-wl >/dev/null 2>&1
echo PASS
