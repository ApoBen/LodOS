set -e
cd /mnt/SSD/LodOS
source orch/env.sh
timeout 15m bash scripts/build-init.sh x86_64 >/dev/null 2>&1
timeout 15m bash scripts/build-init.sh aarch64 >/dev/null 2>&1
for a in x86_64 aarch64; do
  f=$(ls build/init/$a/* | head -1); file -L build/init/$a/* | tee /dev/stderr | grep -q 'statically linked' || exit 1
done
file -L build/init/aarch64/* | grep -q 'aarch64'
cd native/lodos-init
out=$(timeout 15m cargo test -q 2>&1) || { echo "$out" | tail -30; exit 1; }
n=$(echo "$out" | grep -Eo '[0-9]+ passed' | awk '{s+=$1} END{print s+0}')
[ "$n" -ge 15 ]
cd ../..
timeout 15m bash scripts/build-rs.sh native/lodos-init >/dev/null 2>&1
echo PASS
