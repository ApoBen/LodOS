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

## T042: phosh unlock, app grid, foot via injected touch (lodos-wl control socket)
- `scripts/pc-desktop.sh phosh-home` runs the T038 session with `LODOS_WL_CTL=$RUN/ctl` and the guest env `LODOS_PHOSH_NOAUTH=1`, then hands off to `scripts/pc-phosh-home.sh` (gestures). Output: docs/screens/T042-phosh-{unlocked,grid,foot}.png and build/T042-phosh.log. Wall time is about 25 s. `PC_DESKTOP_HOLD=1` keeps the session up for manual `wlctl.py` poking.
- Flow: swipe up from 94% to 19% of H opens the keypad. Tap '1', then tap Unlock, and the home overview appears (search bar plus "Show All Apps"; the favorites grid is empty because GSETTINGS_BACKEND=memory). Tap "Show All Apps" to get the full grid (Foot, Foot Client, Foot Server, Vim). Tap Foot and foot maps fullscreen in phoc. Then `wlctl type` sets a neutral PS1 and runs `uname -srm`, which proves the key path. The script retries the unlock up to 3 times (diff vs the lockscreen dump > 2%).
- No PIN/auth: phosh 0.46 authenticates with PAM service `phosh`, which the rootfs lacks, so it falls back to `other`/pam_unix, and root's password is `*`. The guest session script writes `/etc/pam.d/phosh` = `auth/account sufficient pam_permit.so` when `LODOS_PHOSH_NOAUTH=1`. This is a dev-only setting inside build/pc-rootfs. Not used: `phosh -U/--unlocked` would skip the lockscreen entirely.
- Gotcha: the phosh keypad hides after a few idle seconds, so swipe, digit, and Unlock must follow each other quickly.
- Coordinates (720x1600, scale 1) are stored as fractions of W/H in pc-phosh-home.sh: digit '1' (0.38,0.464), Unlock (0.5,0.682), Show All Apps (0.5,0.0975), Foot (0.07,0.091).

### lodos-wl control protocol (`native/lodos-wl/src/ctl.rs`, headless feature)
- `LODOS_WL_CTL=<path>` starts an AF_UNIX stream listener in the server event loop. Multiple clients are allowed. Each client sends one command per `\n` line, and the server answers each line with `ok` or `err <reason>` after it has applied the command (a `dump` reply comes after the file is written). Blank lines and `#` lines are ignored.
- Commands: `touch down <slot> <x> <y>`, `touch motion <slot> <x> <y>`, `touch up <slot> [x y]`, `touch cancel`, `ptr move <x> <y>`, `btn <evdev-btn> 0|1` (BTN_LEFT=272), `key <evdev-key> 0|1`, `dump <path.ppm>`, `ping`. x/y are host-output logical px.
- Each command maps 1:1 to the existing `ControlMsg` / `App::handle_control` (same path as the `HeadlessServer::inject_*` API). Touch/pointer focus is the topmost toplevel under the point (the nested phoc window), and keyboard focus follows touch down / button press.
- Client: `tools/wlctl.py <sock> tap x y [ms] | swipe x0 y0 x1 y1 ms | click x y | key code.. | type 'text' | dump out.ppm | raw 'line'..` (python3 -I, stdlib socket only). `type` uses a US layout with shift (KEY_LEFTSHIFT=42), and `\n` is Enter.
- Test: `ctl::tests::ctl_protocol_parser` (cargo test --features headless).
- The wlroots wayland backend in phoc takes wl_touch from the host seat as a touch device. phosh gestures (lockscreen swipe, taps) work with touch alone, and no pointer emulation was needed.

## T045: PC guest boots via lodos-init (musl x86_64) as the guest "PID1"
- `scripts/pc-desktop.sh init-phosh`: lodos-wl headless 720x1600 runs on the host. `scripts/pc-guest-init.sh` (idempotent) builds lodos-init (scripts/build-init.sh x86_64, static musl) and installs it as guest `/sbin/lodos-init`. `/sbin/init` is untouched. Then `proot ... /sbin/lodos-init --target=lodos-phosh.target --log=/tmp/lodos-desk/init.log --stop-timeout-ms=3000` runs, the screen is dumped to docs/screens/T045-init-phosh.png (phosh lockscreen), and lodos-init gets SIGTERM. The script then checks that every guest pid recorded before the SIGTERM is gone and that the log contains `lodos-init exited cleanly`. Log: build/T045-init-phosh.log. Wall time is about 12 s.
- Under proot, lodos-init is not a real PID1 because there is no pid namespace. It is the guest's root process and a subreaper, so orphans would reparent to it.
- Units are standard systemd .service files in guest /etc/systemd/system, enabled by symlinks in `lodos-phosh.target.wants/`:
  - `dbus.service` overrides the stock unit by name (etc/ beats usr/lib). The stock one uses `--address=systemd:`, `User=messagebus` and Type=notify. ExecStartPre `sysbus-prep` sets up the machine-id and writes /run/lodos/system.conf (the T038 edits). ExecStart is `dbus-daemon --nofork`.
  - `elogind.service` waits for the system bus socket in ExecStartPre, then runs `/usr/libexec/elogind`. The stock rootfs has no elogind unit, and the lodos-init builtin candidate paths miss `/usr/libexec/elogind`. That is harmless here because the unit file exists.
  - `lodos-sessbus.service` is a session dbus-daemon on unix:path=/run/lodos/session_bus. It replaces dbus-run-session, so every process is a unit main pid.
  - `lodos-phoc.service` waits in ExecStartPre for login1, the session bus, and the host `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`, and removes a stale wayland-0. It then runs `phoc -C /tmp/lodos-desk-phoc.ini` (no -E). WAYLAND_DISPLAY, XDG_RUNTIME_DIR and WLR_* are inherited from lodos-init's env.
  - `lodos-phosh.service` waits for phoc's `wayland-0`, then runs `/usr/libexec/phosh` as a plain client. phoc exposes the phosh protocols to it without -E.
- Readiness: lodos-init treats simple units as active once spawned, so dependents poll in ExecStartPre with `/usr/local/lib/lodos/wait-for sock|bus`. ExecStartPre runs synchronously, so the start order is effectively serialized.
- Boot log excerpt (ms from lodos-init start):
  ```
  +0    lodos-init starting (root=/, target=lodos-phosh.target); loaded 18 unit(s)
  +3    starting dbus.service            -> started +13
  +13   starting elogind.service         -> started +127 (waits for system_bus_socket)
  +127  starting lodos-sessbus.service   -> started +128
  +128  starting lodos-phoc.service      -> started +161 (login1 already up)
  +161  starting lodos-phosh.service     -> started +274; "Phosh ready after 0.15s"
  SIGTERM: shutting down 5 unit(s) in reverse dependency order: lodos-phosh, lodos-phoc, lodos-sessbus, elogind, dbus
  phosh exits rc=0 after ~1.9-2.3 s (slowest; under the 3 s stop timeout), phoc/sessbus/elogind/dbus ~15 ms each
  lodos-init exited cleanly; proot exits; 0 orphans
  ```
- Timings: lodos-init start to phosh spawned is about 0.27 s, and the lockscreen is mapped in lodos-wl under 1 s after guest start. Shutdown takes about 2 s, almost all of it phosh. No lodos-init code change was needed, and all 16 tests still pass.
- Gotcha: the D3 core order (dbus, then elogind first) only applies in BootstrapMode::Auto when units named dbus.service/elogind.service exist. Overriding by name keeps that path in use. All other stock units in the rootfs (getty, networkd, ...) are ignored because only `lodos-phosh.target.wants` is read.
- Phone port next: the same units with an aarch64 build. The phone also needs the T021 shims (unix2abstract, elogind-shim) as Environment=LD_PRELOAD in the dbus/elogind units.
- T050 app smoke (`pc-desktop.sh apps`, init-phosh boot): foot, gnome-text-editor 48, gnome-calculator 48 launch from phosh grid, map, render, close; PNGs docs/screens/T050-*.png.
- lodos-wl ctl `toplevels` -> `ok <n>` (mapped xdg_toplevels); under nested phoc always 1 (phoc's output window) -> app windows are invisible to lodos-wl; app state comes from guest `wlrctl toplevel list|close` (wlr-foreign-toplevel on phoc wayland-0, separate proot sharing rootfs + /run/host-xdg).
- grid launch: fresh session shows "mobile friendly" only (foot hidden) -> tap "Show All Apps" once; then tap search (y 0.045), type name, tap 1st cell (0.07,0.091); search clears after launch. GTK4 apps get keyboard via lodos-wl ctl `key`.
- close: foot/calculator exit on foreign-toplevel close (<1s); text-editor with unsaved draft shows a save dialog -> driver falls back to SIGTERM after 6s.
- comm names truncate to 15 chars (`gnome-calculato`): `pgrep -x gnome-calculator` never matches; use the 15-char comm or tracked pids.

## T043: phone phosh nested on lodos-wl headless
- `scripts/phone-desktop.sh` builds the aarch64 Android `lodos-wl --features headless`, installs the phosh guest package set in the phone trixie rootfs, runs pixman phoc/phosh, then pulls the framebuffer into `docs/screens/T043-phone-phosh.png` (720x1600). The result is the phosh lockscreen.
- Android shell uid cannot create pathname sockets under `/data/local/tmp/lodos/run`: `Permission denied`. lodos-wl continues on abstract `@lodos-lodos-wl`; no root workaround was used. `libunix2abstract.so` maps the guest's `/run/host-xdg/lodos-wl` connection to that abstract name. Phoc's own `wayland-0` uses a separate abstract name, while the u2a and elogind shims handle guest socket/netlink restrictions.
- xkbcommon is built with `xkb-config-root=/data/local/tmp/lodos/rootfs/debian/usr/share/X11/xkb` so the Android host binary reads the guest's installed `xkb-data` files. Its regular Android `/usr/share/X11/xkb` default does not exist.
- Timing from the run: phosh logged `Phosh ready after 1.13s`; lodos-wl had a mapped toplevel and PPM dump. Total runner wall time was about 11s. Cleanup stopped lodos-wl and the phone guest daemons; `pgrep -x phoc`, `phosh`, `elogind`, and `dbus-daemon` returned no processes.

## T065: X11 xterm through phoc Xwayland
- `scripts/pc-desktop.sh xwayland` enables `xwayland=true` in the phoc config for an init-managed phosh session, unlocks phosh, reads Xwayland's selected display and `-auth` file from host `/proc` with host Python, then launches `xterm -bg red -geometry 60x20` with those values. The process scan is limited to descendants of this guest session; the guest image does not need Python. Do not assume `DISPLAY=:0`: phoc can select another display when earlier X server locks/sockets exist.
- It dumps `docs/screens/T065-xterm.png` through lodos-wl's headless control socket, prints `XWAYLAND ok`, and shuts down xterm, phoc, phosh, and lodos-init with the managed-session cleanup path.
- `scripts/pc-guest.sh setup` includes Debian's `xwayland`, `xterm`, and `x11-apps` packages in its idempotent package set. The `xwayland` desktop mode checks those package statuses before boot and runs setup if the existing guest rootfs predates the package list.
