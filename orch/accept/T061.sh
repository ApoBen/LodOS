set -e
cd /mnt/SSD/LodOS
source orch/env.sh
sh -n scripts/guest/audio-start.sh
grep -q 'lodos-audio-relay' scripts/guest/audio-start.sh
grep -q 'liblodos_audio_relay.so' scripts/stage-jnilibs.sh
bash scripts/build-audio-relay.sh >/dev/null 2>&1
f=$(find build native/lodos-audio-relay/target -path '*aarch64*musl*' -name lodos-audio-relay -type f | head -1)
file "$f" | grep -q 'statically linked'
bash orch/accept/T059.sh | tail -1 | grep -q PASS
echo PASS
