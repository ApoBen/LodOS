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

### elogind (T021: systemd-free swap + system dbus + login1)
- **Script**: `scripts/guest-init.sh` (idempotent host driver; log `/tmp/lodos-guest-init.log`).
- **Package swap (Debian 13 trixie arm64)**:
  - Install: `sysvinit-core elogind libpam-elogind dbus-x11 libglib2.0-bin` (pulls `initscripts insserv startpar sysv-rc systemd-standalone-sysusers`).
  - Remove (via `apt-get install --allow-remove-essential` with `<pkg>-`): `systemd systemd-sysv dbus-user-session libpam-systemd` (also removes `libnss-systemd systemd-resolved`).
  - Disk impact: **-10.3 MB** freed (1.66 MB download, 10 added, 6 removed). `libsystemd0` remains installed (Debian trixie `elogind` uses `libsystemd0` directly, no `libelogind0` needed). No failed postinst; `phosh`/`phoc`/`udev`/`polkitd`/`libseat1` all stay intact.
- **System D-Bus (`dbus-daemon --system`)**:
  - Diverted `/usr/share/dbus-1/system.conf` (`dpkg-divert --local`):
    1. `<listen>unix:path=/run/dbus/system_bus_socket</listen>` -> `unix:abstract=/run/dbus/system_bus_socket` (R13 shell-uid workaround; `DBUS_SYSTEM_BUS_ADDRESS` exported to match, and `LODOS_U2A_PREFIX=/tmp/xdg:/run/dbus` maps hardcoded path clients to the same abstract address).
    2. Removed `<user>messagebus</user>`: under `proot -0`, `dbus-daemon` otherwise aborts with `Failed to drop capabilities: Operation not permitted` (`capset` denied for unprivileged host uid).
  - `SO_PEERCRED` under `proot -0` reports `uid=0` (`root`), so `/usr/share/dbus-1/system.d/org.freedesktop.login1.conf` `<policy user="root">` matches cleanly and `RequestName(org.freedesktop.login1)` succeeds.
- **elogind daemon (`/usr/libexec/elogind`)**:
  - Stock `/usr/libexec/elogind` fails at startup with `sd-device-monitor: Failed to create socket: Permission denied` (`Failed to create udev watchers: Permission denied`) because Android SELinux denies `socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT)` (even when udev is inactive).
  - Workaround: `native/elogind-shim/elogind_shim.c` (`build/elogind-shim/libelogind_shim.so`, header-free NDK clang build, pushed to `/usr/local/lib/lodos/`). On `socket(AF_NETLINK, ..., NETLINK_KOBJECT_UEVENT)` failure, substitutes an idle `AF_UNIX` `SOCK_DGRAM` fd (pollable via `epoll`) and stubs `bind`/`setsockopt`/`getsockname` on that fd. Chains cleanly with `libunix2abstract.so` in `LD_PRELOAD`.
  - With the shim, `/usr/libexec/elogind` stays alive, acquires `org.freedesktop.login1`, responds `()` to `gdbus call --system -d org.freedesktop.login1 -o /org/freedesktop/login1 -m org.freedesktop.DBus.Peer.Ping`, and shows up in `busctl --system list` (`busctl` is shipped by `elogind`).
- **What works / doesn't**:
  - **Works**: system bus, `org.freedesktop.login1` ownership & `Peer.Ping`, `busctl --system list`, `loginctl list-sessions` (`No sessions.`), `loginctl list-seats` (`seat0` created automatically: `CanTTY=no`, `CanGraphical=no`, `IdleHint=yes`), `phosh` login1 proxy lookup (**`phosh-login1-warn=0`**, down from 1 `phosh-torch-manager-WARNING: Failed to get login1 session proxy` in T016; phosh reaches `Phosh ready` and stays `phosh-alive` 10s).
  - **Cgroups (T025)**: Android's cgroup2 mount cannot be written by shell uid. `scripts/guest-init.sh` instead binds `$L/fake-cgroup` (a writable regular directory under `/data/local/tmp/lodos`) onto guest `/sys/fs/cgroup`. `native/elogind-shim/elogind_shim.c` reports `CGROUP2_SUPER_MAGIC` for that path, creates/removes minimal `cgroup.*` files around elogind's scope-directory operations, and supplies `/proc/<pid>/cgroup` reads as membership in `user.slice/user-0.slice/session-<id>.scope`. After PAM creates a session, the driver records its scope basename in `/run/systemd/lodos-session-scope`; the shim reads it when synthesizing proc membership. This lets elogind resolve the synthetic session despite the regular files not changing kernel cgroup membership; the shim is appropriate only for the isolated guest probe, not a general cgroup implementation. Controller files are empty, so resource controllers remain unavailable.
  - **PAM (`libpam-elogind`)**: `su -l root -c true` triggers the configured `pam_elogind.so` session hook. The shim also treats unprivileged `/run/user/*` mount/unmount operations as successful no-ops; the resulting `loginctl list-sessions` entry is checked by the driver and reported as `INIT session ok <id>`.
  - **Inhibitors**: the driver ensures `/run/systemd/inhibit` exists and requests a short-lived idle inhibitor through the login1 D-Bus method while a PAM session is open. If elogind tries to open a missing numeric `/run/systemd/inhibit/<id>.ref`, the shim creates that individual reference file and retries the original open. The synthetic `/proc/<pid>/cgroup` membership is needed because the regular fake `cgroup.procs` file cannot update kernel membership. UPower activation can still fail (`dbus-daemon-launch-helper` setuid check under proot).

