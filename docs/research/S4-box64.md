# S4 v0: box64 in Debian trixie proot guest (shell uid) — T024

Device: POCO X7 Pro (`<phone-serial>`, Dimensity 8400, 8x Cortex-A725, 4K pages, Android 15 kernel 6.6.118).
Guest: Debian 13.7 (`trixie` arm64) via `scripts/phone-run.sh` (`proot -0`, shell uid).
Script: `scripts/guest-box64.sh` (idempotent; verified end-to-end).

## Install path & version
- **Apt package** (directly in Debian 13 `trixie/main` arm64, no backports or source build needed):
  - `box64` `0.3.4+dfsg-1` (3.08 MB `.deb`; 26.8 MB total installed with deps).
  - Installed with `libgcc-s1-amd64-cross` and `libstdc++6-amd64-cross` (pulls `gcc-14-cross-base`, `libc6-amd64-cross`) so `apt` never installs multiarch `libc6:amd64` into the arm64 rootfs.
  - `box64 -v`: `[BOX64] Box64 with Dynarec v0.3.4 nogit built on Apr 24 2025 09:54:47`.
  - Fallback source-build path (`ptitSeb/box64`, `cmake -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo`, `make -j8`) is kept in `scripts/guest-box64.sh` if `box64` is absent from apt.
- **x86_64 test binaries** (`/opt/x64root`):
  - `dpkg --add-architecture amd64` + `apt-get download hello:amd64 bash:amd64` (`hello 2.10-5`, `bash 5.2.37-2+b10`) + `dpkg-deb -x` into `/opt/x64root`.
  - No `libc6:amd64` extracted into `/opt/x64root`: box64 wraps `libc.so.6`, `ld-linux-x86-64.so.2`, `libpthread.so.0`, `libdl.so.2`, `libutil.so.1`, `libresolv.so.2`, `librt.so.1`, `libbsd.so.0` natively.
  - **Usrmerge quirk**: `bash:amd64` in Debian 13 extracts to `./usr/bin/bash` (no `./bin`); `scripts/guest-box64.sh` creates `/opt/x64root/bin -> usr/bin`.

## Runtime flags & detected hardware
- ARM64 Dynarec enabled by default:
  - `Dynarec for ARM64, with extension: ASIMD AES CRC32 PMULL ATOMICS SHA1 SHA2 USCAT FLAGM FLAGM2 FRINT AFP`
  - `Running on Cortex-A725 with 8 cores, pagesize: 4096`
  - `Will use hardware counter measured at 13.0 MHz emulating 3.3 GHz`
- Env vars set in `scripts/guest-box64.sh`:
  - `BOX64_LOG=0`, `BOX64_NOBANNER=1` (important: see stdout orientation issue below)
  - `BOX64_LD_LIBRARY_PATH=/opt/x64root/usr/lib/x86_64-linux-gnu:/opt/x64root/lib/x86_64-linux-gnu:/usr/x86_64-linux-gnu/lib`
  - `BOX64_PATH=/opt/x64root/usr/bin:/opt/x64root/bin`

## Verified results & performance
- `box64 /opt/x64root/usr/bin/hello` -> `Hello, world!` (exit 0).
- `box64 /opt/x64root/bin/bash -c 'echo $((6*7))'` -> `42` (exit 0).
- Child x86_64 `execve` from x86_64 bash: `box64 /opt/x64root/bin/bash -c '/opt/x64root/usr/bin/hello'` -> `Hello, world!` (box64 intercepts child x86_64 ELF `execve` without `binfmt_misc`).
- Loop benchmark (`for ((i=1;i<=100000;i++)); do :; done`):
  - Native arm64 `/bin/bash`: **218–237 ms**
  - `box64` dynarec `/opt/x64root/bin/bash`: **851–854 ms** (~3.6–3.9× native)
  - `box64` interpreter (`BOX64_DYNAREC=0`): **18598 ms** (~78× native; dynarec gives ~21.8× speedup over interpreter)

## Proot / Android / Box64 issues observed
1. **39-bit virtual address space**: `[BOX64] Didn't detect 48bits of address space, considering it's 39bits`. Android 15 ARM64 kernel exposes 39-bit VA (`CONFIG_ARM64_VA_BITS=39`); box64 detects this automatically and adapts its allocator.
2. **Missing program break (`[heap]`) under proot**: `[BOX64] Warning, program break not found`. Under `proot` (`libproot-loader.so`), the main binary is mapped by the loader (`00200000` loader, `30000000+` box64 rwxp dynarec blocks, `34800000` box64 ELF), so `/proc/self/maps` has no kernel `[heap]` VMA. Non-fatal for `hello`/`bash`, but monitor for x86_64 apps (e.g. wine/games) with fixed low `mmap` hints or `brk`/`sbrk` assumptions.
3. **`BOX64_LOG>=1` breaks wide-oriented stdout (`wprintf`)**: box64 writes its `[BOX64]` log lines to `stdout` (byte-oriented stdio, setting `fwide(stdout) < 0`). Debian's GNU `hello` prints via `__wprintf_chk` (wide-oriented stdio on `stdout`), which glibc rejects with `-1` when `stdout` is already byte-oriented — producing empty stdout with exit 0. Keeping `BOX64_LOG=0` + `BOX64_NOBANNER=1` (or redirecting box64 logs away from `stdout`) avoids this completely.
4. **`/proc/<pid>/exe` & `personality`**: External processes inspecting `/proc/<pid>/exe` of a box64 process see `/usr/bin/box64` (translated by proot); `/proc/<pid>/personality` is `00000000` (no `ADDR_LIMIT_3GB` or `PER_LINUX32` set). Inside box64 bash, `/usr/bin/uname -m` returns `x86_64` (box64's builtin `uname -m` interception), whereas `uname -a` invokes native arm64 `/usr/bin/uname` and prints `aarch64`.
5. **No `binfmt_misc` rootless**: Debian's `/usr/lib/binfmt.d/box64.conf` has no effect in rootless proot; top-level x86_64 launches must invoke `box64 <bin>`, while nested x86_64 `execve` is handled internally by box64.
