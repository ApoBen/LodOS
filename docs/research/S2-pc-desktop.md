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

## T037: phoc nested on lodos-wl headless
- `scripts/pc-desktop.sh foot` starts lodos-wl headless (720x1600), then guest `phoc -C phoc.ini -E foot <demo>`, then SIGUSR1 PPM dump -> `scripts/ppm2png.py` -> docs/screens/T037-phoc-foot.png, then stops everything.
- Runtime dir: `$XDG_RUNTIME_DIR/lodos-desk` (bound to guest /run/host-xdg). This keeps phoc's own `wayland-0` out of the host session dir.
- phoc 0.46 does not handle wlroots output `request_state`, so the host toplevel configure size is ignored (it stays at the wayland-backend default of 1280x720). The nested output size is pinned with `[output:WL-1] mode=WxH` in phoc.ini. `[core] xwayland=false` removes the Xwayland CRITICAL.
- lodos-wl protocols that phoc+pixman needs were already there (compositor, shm, xdg_wm_base, seat, output+xdg-output). Added: an initial toplevel configure with the output size, and an xdg-decoration stub (always server-side).
- Harmless phoc log noise: `Basic output test failed for WL-1` (x3, phoc probing modes on a disabled output), no machine-id (idle-inhibit dbus), foot "no server-side cursors / toplevel icon".

## T038: phosh nested on lodos-wl headless (first desktop)
- `scripts/pc-desktop.sh phosh`: lodos-wl headless 720x1600 -> guest `scripts/pc-phosh-session.sh` (copied to rootfs /tmp) -> system dbus -> elogind -> `dbus-run-session -- phoc -C phoc.ini -E /usr/libexec/phosh` -> SIGUSR1 dump -> docs/screens/T038-phosh.png. Guest stdout+stderr -> build/T038-phosh.log. Wall time ~10 s.
- Result: the phosh lockscreen renders (clock, date, "Slide up to unlock", status icon). `Phosh ready after 0.13s`.
- PC needs no shims: path AF_UNIX sockets work, and `NETLINK_KOBJECT_UEVENT` is allowed, so neither unix2abstract nor elogind-shim is needed. elogind runs unmodified.
- System bus: a copy of stock system.conf with `<user>messagebus</user>` removed (proot -0 can't capset), `<pidfile>` removed, and `includedir system.d` made absolute (relative includedir resolves next to the config file). `dbus-daemon --nofork`. login1 is acquired.
- Daemons stay children of the payload shell (no --fork). proot `--kill-on-exit` plus the pgid kill reap them, and nothing is left running.
- Guest packages added to pc-guest.sh PKGS: `gnome-settings-daemon-common` (phosh aborts on the missing touchscreen schema) and `librsvg2-common` (without the SVG pixbuf loader the status/arrow icons draw as broken-image placeholders).
- Env: `GSETTINGS_BACKEND=memory XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland`. `/etc/machine-id` is generated if it is "uninitialized".
- Remaining warnings (not blockers): `No session for pid` from polkit (no PAM/elogind session, same as R15/T025), no org.a11y.Bus, no org.gnome.SessionManager (gnome-session isn't installed), no sm.puri.OSK0 (squeekboard isn't installed), no NetworkManager agent, no gnome-backgrounds (black wallpaper), no org.gnome.Calls. Plus the 3 known `Basic output test failed for WL-1` CRITICALs.
- Next: unlock and home screen need input (lodos-wl seat pointer/touch injection), squeekboard for the OSK, and an elogind session (T025 cgroup shim) for polkit.
