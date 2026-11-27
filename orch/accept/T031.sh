set -e
cd /mnt/SSD/LodOS
source orch/env.sh
cd native/lodos-init
timeout 15m cargo test -q 2>&1 | tail -15
n=$(cargo test -q 2>&1 | grep -Eo '[0-9]+ passed' | awk '{s+=$1} END{print s+0}')
[ "$n" -ge 8 ]
cd ../..
timeout 15m bash scripts/build-rs.sh native/lodos-init >/dev/null 2>&1 || timeout 15m bash -c 'cd native/lodos-init && cargo ndk -t arm64-v8a build --release -q'
{ test -f build/rs/arm64/lodos-init || find native/lodos-init/target -path '*aarch64*' -name lodos-init -type f | grep -q . ; }
echo PASS
