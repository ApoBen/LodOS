# Guest launch (T046) — org.lodos.app.guest.GuestLaunch
Single builder for the app-uid proot command (was inline in ProbeActivity; mirrors scripts/phone-run.sh).
- `<nativeLibDir>/libproot.so` + env `PROOT_LOADER=<nativeLibDir>/libproot_loader.so`: app data is noexec (W^X, D4), so binaries ship as jniLibs.
- `PROOT_TMP_DIR=<tmpDir>`: proot needs a writable tmp; app has no /tmp.
- `-0` fake root (apt/dpkg need uid 0); `-r <rootfs>`; `--link2symlink` (no hardlinks on app storage); `--sysvipc` (SysV shm/sem emu); `--kill-on-exit` (no orphans).
- binds: /dev /proc /sys; `<shmDir|tmpDir>:/dev/shm` (Android has no /dev/shm; sem_open/shm_open need it); `<runDir>:/run/lodos` (= XDG_RUNTIME_DIR, wayland socket); optional `<sdcard>:/sdcard`; extraBinds.
- `-w /root /usr/bin/env -i K=V... <cmd>`: clean guest env (no Android env leak). Defaults HOME, PATH, TERM, LANG=C.UTF-8, XDG_RUNTIME_DIR=/run/lodos, WAYLAND_DISPLAY; then LiteMode.toGuestEnv(lite); extraEnv overrides all (but never PROOT_* in buildEnv).
- validation: every path absolute, rootfs dir exists, cmd non-empty -> else IllegalArgumentException.
- API: buildArgv(spec), hostEnv(spec) (for ProcessBuilder), guestEnv(spec), buildEnv(spec)=guest+host.
