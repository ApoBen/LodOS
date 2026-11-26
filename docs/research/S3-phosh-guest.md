# S3 probe: phosh/phoc in Debian trixie guest (proot, shell uid) — T016

Device: POCO X7 Pro (<phone-serial>), guest Debian 13.7 via scripts/phone-run.sh (proot -0, shell uid).
Script: `scripts/guest-phosh.sh` (idempotent; log /tmp/lodos-guest-phosh.log on host).

## Install
- pkgs: phosh phoc squeekboard gnome-session-bin dbus-user-session (+ gnome-settings-daemon-common, wayland-utils).
- versions: phoc 0.46.0, phosh 0.46.0-3+deb13u1, libwlroots-0.18 0.18.2-3, squeekboard 1.43.1.
- 180 new pkgs, 94.2 MB download, 459 MB declared; first run **193 s** (≈62 s download @1.5 MB/s, rest dpkg).
- rootfs du: **586 MB -> 957 MB (+361 MB)**; +5 MB for gnome-settings-daemon-common. Re-run (all installed): ~25 s (probes only).
- dpkg noise only: `invoke-rc.d: policy-rc.d denied execution of start` (expected), `Failed to scan devices: Permission denied` (udev trigger). No failed postinst.

## elogind status
NOT installable. Exact apt error (simulated, script detects and skips):
```
dbus-user-session : Depends: systemd but it is not installable
libpam-systemd : Depends: systemd (= 257.13-1~deb13u1) but it is not installable
systemd-sysv : PreDepends: systemd but it is not installable
   2. elogind:arm64=255.17-1debian2 is selected for install
   2. elogind:arm64 Conflicts systemd
```
Rootfs ships systemd + systemd-sysv + libpam-systemd, and dbus-user-session needs systemd. libpam-elogind requires elogind, so it is blocked too.
D3 (mini-init + dbus + elogind) therefore needs: swap systemd-sysv -> sysvinit-core/elogind (apt remove of systemd-sysv, libpam-systemd, dbus-user-session -> use dbus-x11 or plain dbus-daemon session) — follow-up task. Phosh runs today without logind (warnings only, see below).

## Binary probes
| probe | result |
|---|---|
| `phoc --version` | `Phoc 0.46.0 - Phone compositor` rc=0 |
| `phosh --help` | phosh is `/usr/libexec/phosh` (not on PATH by design); symlinked to `/usr/local/bin/phosh`, prints `Usage:` |
| squeekboard / gnome-session | present in /usr/bin |
| `dbus-daemon --session --fork --print-address` (stock config) | **FAIL**: `Failed to bind socket "/tmp/dbus-XXXX": Permission denied` (R13: shell uid can't create path AF_UNIX in /data/local/tmp) |
| same, after fixup | `unix:abstract=/tmp/lodos-dbus-session,guid=...` rc=0 |

## headless phoc (key signal)
`XDG_RUNTIME_DIR=/tmp/xdg WLR_BACKENDS=headless WLR_RENDERER=pixman timeout 8 phoc -E 'sleep 3'`
- plain: **rc=139 (segfault)** after `Unable to open wayland socket: No such file or directory` (wl_display_add_socket_auto fails on path bind, phoc 0.46 crashes on the NULL socket instead of exiting cleanly). Before that: `Linux dmabuf support unavailable` (no DRM node), Xwayland missing (not installed; harmless).
- with `LD_PRELOAD=/usr/local/lib/lodos/libunix2abstract.so` (new shim, see below): **works**. `Running compositor on wayland display 'wayland-0'`, `Output 'HEADLESS-1' added`, `wayland-info` inside `-E` sees 44 globals (wl_shm, zwlr_layer_shell_v1, phosh_private, zwlr_screencopy, ...). rc=0.
- => wlroots/phoc + pixman renderer run fine under proot; the only blocker is the path socket (R13).

## phosh inside headless phoc (bonus)
`dbus-run-session -- phoc -E 'phosh & ...'` with shim: phosh connects and binds `phosh_private`, **still running after 10 s** (`phosh-alive`). Warnings only:
no system bus (NM, hostname1, login1, geoclue), no org.gnome.SessionManager, a11y bus, no backgrounds pkg, sensor-proxy, polkit `No session for pid` (no logind).
Without `gnome-settings-daemon-common`, phosh aborts: `Settings schema 'org.gnome.settings-daemon.peripherals.touchscreen' is not installed` -> added to pkgs.
Teardown segfault (rc=139) when timeout kills phoc: Gdk `Connection reset by peer`, not relevant.
dbus-daemon warning: `Failed to set fd limit to 65536: Operation not permitted` (harmless).

## Guest fixups done by the script (guest only)
1. `/etc/machine-id` was `uninitialized` (13 chars) -> `dbus-uuidgen`; `/var/lib/dbus/machine-id` symlink (gio refused autolaunch/session).
2. `/usr/local/bin/phosh -> /usr/libexec/phosh`; scripts/phone-run.sh PATH now includes /usr/local/{sbin,bin} (Debian default).
3. `dpkg-divert --local` of `/usr/share/dbus-1/session.conf`: `<listen>unix:tmpdir=/tmp</listen>` -> `unix:abstract=/tmp/lodos-dbus-session`. Fixed name means only one session bus per netns (all shell-uid procs). Second instance fails with "address in use". OK for probes. Real fix: app-uid path sockets (T017) or the shim (dbus works with `unix:path=` under the shim too).
4. `native/unix2abstract/unix2abstract.c` -> `build/u2a/libunix2abstract.so` (NDK clang `--target=aarch64-linux-gnu -nostdlib`, header-free, no DT_NEEDED; resolves dlsym/getenv from guest glibc). Wraps bind()/connect(): AF_UNIX path matching `$LODOS_U2A_PREFIX` (':'-sep, default `/tmp/xdg`) -> abstract `"\0<path>"`. Both server and client must preload it. Limits: `test -S` / stat of the socket path fails; getsockname returns the abstract name.

## Takeaways for S3
- phosh stack installs cleanly (+361 MB, ~3 min). phoc/phosh binaries work under proot.
- R13 is the only hard blocker for headless phoc. The LD_PRELOAD shim is enough for in-guest compositor+clients. Candidate for global use (/etc/ld.so.preload with prefix /tmp/xdg:/run/user) if app uid also denies path sockets (T017 decides).
- elogind conflicts with the rootfs systemd. D3 needs a systemd-sysv -> elogind swap task. A system bus (dbus system daemon) is needed for phosh managers.
- Next: phoc with a real output (wayland backend nested into lodos-wl via abstract socket, or headless + screencopy) and a system dbus.
