set -e
cd /mnt/SSD/LodOS
test -x scripts/pc-guest.sh
out=$(timeout 5m bash scripts/pc-guest.sh run sh -c 'uname -m; command -v phoc phosh foot wayland-info; dpkg-query -W -f="\${Version}\n" phoc phosh' 2>&1) || { echo "$out" | tail -20; exit 1; }
echo "$out"
echo "$out" | grep -qx x86_64
for b in phoc phosh foot wayland-info; do echo "$out" | grep -q "/$b\$" || { echo "missing $b"; exit 1; }; done
grep -q 'build/pc-rootfs' .gitignore || grep -q '^build' .gitignore
test -s docs/research/S2-pc-desktop.md
echo PASS
