set -e
cd /mnt/SSD/LodOS
bash scripts/guest-phosh.sh
out=$(bash scripts/phone-run.sh 'phoc --version 2>&1 | head -1; command -v phosh >/dev/null && echo phosh-bin; dbus-daemon --session --fork --print-address >/dev/null && echo dbus-sess')
echo "$out"
echo "$out" | grep -qi phoc
echo "$out" | grep -qx phosh-bin
echo "$out" | grep -qx dbus-sess
test -s docs/research/S3-phosh-guest.md
grep -qi headless docs/research/S3-phosh-guest.md
echo PASS
