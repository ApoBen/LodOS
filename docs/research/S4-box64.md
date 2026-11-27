# S4 v0: box64 in Debian trixie proot guest (shell uid) — T024

Device: POCO X7 Pro (`<phone-serial>`, Dimensity 8400, 8x Cortex-A725, 4K pages, Android 15 kernel 6.6.118).
Guest: Debian 13.7 (`trixie` arm64) via `scripts/phone-run.sh` (`proot -0`, shell uid).
Script: `scripts/guest-box64.sh`.

> **WIP (attempt 1 incomplete):** Phone USB disconnected mid-run before `hello` output debugging and benchmark execution could complete. All observations below are from live phone runs prior to disconnect; benchmark & `hello` stdout verification pending phone reconnect.

## Install path & version
- **Apt package** (directly in Debian 13 `trixie/main` arm64, no backports or source build needed):
  - `box64` `0.3.4+dfsg-1` (3.08 MB `.deb`; 26.8 MB total installed with deps).
  - Explicitly installed with `libgcc-s1-amd64-cross` and `libstdc++6-amd64-cross` (pulls `gcc-14-cross-base`, `libc6-amd64-cross`) so `apt` never installs multiarch `libc6:amd64` into the arm64 rootfs.
  - `box64 -v`: `[BOX64] Box64 with Dynarec v0.3.4 nogit built on Apr 24 2025 09:54:47`.
- **x86_64 test binaries** (`/opt/x64root`):
  - `dpkg --add-architecture amd64` + `apt-get download hello:amd64 bash:amd64` (`hello 2.10-5`, `bash 5.2.37-2+b10`) + `dpkg-deb -x` into `/opt/x64root`.
  - No `libc6:amd64` extracted into `/opt/x64root`: box64 wraps `libc.so.6`, `ld-linux-x86-64.so.2`, `libpthread.so.0`, `libdl.so.2`, `libutil.so.1`, `libresolv.so.2`, `librt.so.1`, `libbsd.so.0` natively.
  - **Usrmerge quirk**: `bash:amd64` in Debian 13 extracts to `./usr/bin/bash` (no `./bin`); `scripts/guest-box64.sh` creates `/opt/x64root/bin -> usr/bin`.

## Runtime flags & detected hardware
- Dynarec enabled by default on ARM64:
  `Dynarec for ARM64, with extension: ASIMD AES CRC32 PMULL ATOMICS SHA1 SHA2 USCAT FLAGM FLAGM2 FRINT AFP`
  `Running on Cortex-A725 with 8 cores, pagesize: 4096`
  `Will use hardware counter measured at 13.0 MHz emulating 3.3 GHz`
- Env vars set in `scripts/guest-box64.sh`:
  - `BOX64_NOBANNER=1`
  - `BOX64_LD_LIBRARY_PATH=/opt/x64root/usr/lib/x86_64-linux-gnu:/opt/x64root/lib/x86_64-linux-gnu:/usr/x86_64-linux-gnu/lib`
  - `BOX64_PATH=/opt/x64root/usr/bin:/opt/x64root/bin`

## Verified results (before disconnect)
- `box64 /opt/x64root/bin/bash -c 'echo $((6*7))'` -> printed `42`, exit 0.
- `box64 /opt/x64root/usr/bin/hello` -> exited 0 after wrapping `libc.so.6` etc., **but produced no stdout line in initial `BOX64_LOG=1` probe** (needs investigation on reconnect: possible `wprintf`/locale/glibc 2.41 wrapping issue in box64 0.3.4).
- Benchmark (`for ((i=1;i<=100000;i++))` native vs box64): **not yet run** (phone disconnected).

## Proot / Android-specific observations
1. **39-bit virtual address space**: `[BOX64] Didn't detect 48bits of address space, considering it's 39bits`. Android 15 ARM64 kernel exposes 39-bit VA (`CONFIG_ARM64_VA_BITS=39`); box64 detects this automatically and adapts its mmap/dynarec allocator.
2. **Missing program break (`[heap]`)**: `[BOX64] Warning, program break not found`. Under `proot` (`libproot-loader.so`), the main executable is mapped by the loader rather than kernel `execve`, so `/proc/self/maps` has no standard `[heap]` VMA at ELF `brk`. Non-fatal for `bash`; monitor for apps relying on `brk`/`sbrk` or fixed low mmap hints.
3. **binfmt_misc unavailable rootless**: Debian's `box64` installs `/usr/lib/binfmt.d/box64.conf`, which has no effect in rootless proot; x86_64 binaries must be invoked via `box64 <bin>` (or box64's internal execve interception for child x86_64 binaries via `BOX64_PATH`).
4. **Pending probes** (on reconnect): `hello` stdout root cause, `/proc/self/exe` readlink behavior inside box64+proot, `personality()` / `mmap` hints, child x86_64 `execve` from x86_64 `bash`, and native vs box64 dynarec vs interpreter timing.
