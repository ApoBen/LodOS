# S2 PC desktop guest (T036) — x86_64 Debian trixie, rootless, no phone

## Pieces
- `scripts/fetch-rootfs.sh [arm64|amd64]` (or `ARCH=`): arm64 is still the default and its outputs are unchanged (`SOURCE.txt`). amd64 -> `build/rootfs/debian-trixie-amd64.tar.xz` + `SOURCE-amd64.txt`. Source: lxc images (`debian/trixie/amd64/default/20261118_05:24`), docker fallback picks the `linux/amd64` manifest.
- `scripts/build-proot-pc.sh`: termux/proot c0b2d098 (same commit as T006) + talloc 2.4.2, built with host gcc as **one static binary with the loader bundled in** (no PROOT_LOADER, no 32-bit loader). Output: `build/proot/x86_64/proot`. The phone's talloc config (`scripts/proot/talloc-config.h`) works on glibc unchanged. Needs `-D__STDC_WANT_LIB_EXT1__=1`, same as the NDK build.
- `scripts/pc-guest.sh setup|run <cmd...>|shell`. Rootfs is `build/pc-rootfs` (gitignored via `build/`). It is extracted rootless (`tar --no-same-owner --exclude=./dev/*`) automatically on first `run`. Takes about 1 s on SSD.

## Runner flags
- proot (default): `-0 -r build/pc-rootfs --link2symlink --sysvipc --kill-on-exit -b /dev -b build/pc-guest/shm:/dev/shm -b /proc -b /sys -b $XDG_RUNTIME_DIR:/run/host-xdg -w /root`. These are the phone flags. `/tmp` is the guest's own (rootfs `/tmp`). `PROOT_TMP_DIR=build/pc-guest/tmp`.
- bwrap fallback (`PC_BACKEND=bwrap`, also used automatically if the proot build fails): `--unshare-user --uid 0 --gid 0 --bind rootfs / --dev-bind /dev --proc /proc --ro-bind /sys --bind xdg /run/host-xdg`. Checked: uname=x86_64, uid 0, phoc found. apt is not tested under bwrap: it has no fake chown, so prefer proot.
- Guest env: `env -i HOME=/root PATH=... LANG=C.UTF-8 XDG_RUNTIME_DIR=/run/host-xdg`, plus the host's `WAYLAND_DISPLAY`/`DISPLAY` when set. If the host has no XDG_RUNTIME_DIR, an empty dir is bound instead.
- `EXTRA_BINDS='-b host:guest'` works the same as phone-run.sh.

## Packages (setup)
- Uses the same config as guest-setup.sh: `APT::Sandbox::User root`, no recommends, dpkg force-unsafe-io, resolv.conf 8.8.8.8/1.1.1.1, policy-rc.d exit 101. The Android aid_* groups are left out.
- `elogind` Conflicts `systemd`, and the lxc image ships systemd-sysv, so a plain install fails. Fix: the same D3 swap as phone guest-init.sh: `+sysvinit-core libpam-elogind dbus-x11`, `-systemd systemd-sysv dbus-user-session libpam-systemd` (`--allow-remove-essential`). Everything installs in about 65 s with a warm network.
- Versions: phoc 0.46.0-1, phosh 0.46.0-3+deb13u1, foot 1.21.0-2, wayland-utils 1.2.0-2, weston 14.0.2-1, dbus 1.16.2-2, elogind 255.17-1debian2.
- Debian puts phosh at `/usr/libexec/phosh` (not in PATH), so setup symlinks `/usr/local/bin/phosh`.
- Guest `/bin/sh` is dash. dash's `command -v a b c` only resolves the first name. setup adds `/usr/local/bin/sh -> /bin/bash` so `sh` found via PATH handles multi-name lookups. `/bin/sh` stays dash. This is a workaround for orch/accept/T036.sh's multi-arg `command -v`. Fixing the accept script (one `command -v` per name) would make the symlink unnecessary.

## Smoke results (host KDE Wayland, WAYLAND_DISPLAY=wayland-1)
- `wayland-info` in the guest lists the host globals (wl_compositor v6, ...), so the /run/host-xdg bind works.
- `weston-simple-shm` ran 3 s with no error (timeout rc 124).
- `phoc -E 'sleep 3'` nested on the host (wlroots wayland backend): output WL-1 is added and it exits 0 after the child finishes. Xwayland is missing, which is a harmless CRITICAL (`xwayland` not installed).

## Problems / notes
- Harmless: `udevadm trigger` in postinst prints `Permission denied` on /sys uevent writes. It is fake root, and host /sys stays writable only for the real user.
- Harmless: dpkg "protected package" warnings come from the systemd removal.
- Nested phoc creates its socket (`wayland-0`) in the host XDG_RUNTIME_DIR because of the bind. Use `WAYLAND_DISPLAY`/socket names that don't collide with the host.
- Not done yet: running phosh against nested phoc, dbus/elogind startup on PC (same as phone T021/T025 path).
