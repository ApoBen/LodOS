set -e
cd /mnt/SSD/LodOS
test -x scripts/fetch-rootfs.sh
bash scripts/fetch-rootfs.sh
t=build/rootfs/debian-trixie-arm64.tar.xz
test -s $t
(cd build/rootfs && sha256sum -c debian-trixie-arm64.sha256)
v=$(tar -xJOf $t ./etc/debian_version 2>/dev/null || tar -xJOf $t etc/debian_version)
echo "debian_version=$v"; case "$v" in 13*|trixie*) ;; *) echo "not trixie"; exit 1;; esac
tar -tJf $t | grep -qE '(^|/)usr/bin/apt$'
echo PASS
