set -e
cd /mnt/SSD/LodOS
source orch/env.sh
bash scripts/build-proot.sh
P=build/proot/arm64
file $P/libproot.so | grep -q 'ARM aarch64'
file $P/libproot-loader.so | grep -q 'ARM aarch64'
grep -qi proot THIRD_PARTY.md
D="adb"; R=/data/local/tmp/lodos/proot
$D shell mkdir -p $R /data/local/tmp/lodos/tmp
$D push $P/libproot.so $P/libproot-loader.so $R/ >/dev/null
$D shell chmod 755 $R/libproot.so $R/libproot-loader.so
out=$($D shell "PROOT_LOADER=$R/libproot-loader.so PROOT_TMP_DIR=/data/local/tmp/lodos/tmp $R/libproot.so -0 /system/bin/id")
echo "$out"; echo "$out" | grep -q 'uid=0'
echo PASS
