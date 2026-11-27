set -e
cd /mnt/SSD/LodOS
source orch/env.sh
cargo ndk --version
bash scripts/build-rs.sh native/hello-rs
B=build/rs/arm64/lodos-hello
file $B | grep -q 'ARM aarch64'
D="adb"
$D shell mkdir -p /data/local/tmp/lodos/bin
$D push $B /data/local/tmp/lodos/bin/ >/dev/null
$D shell chmod 755 /data/local/tmp/lodos/bin/lodos-hello
out=$($D shell /data/local/tmp/lodos/bin/lodos-hello)
echo "$out"; echo "$out" | grep -q 'lodos-rs ok aarch64'
echo PASS
