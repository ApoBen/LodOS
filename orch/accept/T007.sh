set -e
cd /mnt/SSD/LodOS
bash scripts/phone-rootfs.sh
out=$(bash scripts/phone-run.sh 'cat /etc/debian_version; uname -m; id -u; /usr/bin/apt --version | head -1')
echo "$out"
echo "$out" | grep -qE '^(13|trixie)'
echo "$out" | grep -q aarch64
echo "$out" | grep -qx 0
echo "$out" | grep -q '^apt '
echo PASS
