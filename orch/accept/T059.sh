set -e
cd /mnt/SSD/LodOS
source orch/env.sh
(cd native/lodos-audio-relay && timeout 10m cargo test 2>&1 | tail -5) | grep -q "test result: ok"
f=$(find build native/lodos-audio-relay/target -path '*x86_64*musl*' -name lodos-audio-relay -type f | head -1)
file "$f" | grep -q 'statically linked'
grep -q 'lodos-audio-relay' scripts/pc-audio-test.sh
bash orch/accept/T049.sh | tail -3 | grep -q PASS
grep -qi 'relay' docs/research/S5-audio.md
echo PASS
