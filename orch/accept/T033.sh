set -e
cd /mnt/SSD/LodOS
source orch/env.sh
cd native/lodos-rootfs
out=$(timeout 15m cargo test -q 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -10
n=$(echo "$out" | grep -Eo '[0-9]+ passed' | awk '{s+=$1} END{print s+0}')
[ "$n" -ge 6 ]
grep -rqE 'traversal|\.\./' src tests 2>/dev/null
timeout 15m cargo ndk -t arm64-v8a build --release --features jni -q
so=$(find target -path '*aarch64*release*' -name 'liblodos_rootfs.so' | head -1)
test -n "$so"
nm -D --defined-only "$so" | grep -q Java_org_lodos_app_setup_NativeRootfs_extract
echo PASS
