# S4: Wine and Windows x86/x64 games on ARM64

Scope: Hades first, after the Box64 smoke test in [S4-box64.md](S4-box64.md). This is a survey and spike plan, not a claim that game graphics work on the Mali ICD.

## Options

### ARM64EC Wine + FEX (`libarm64ecfex` / WoW64 DLL)

- **Source/builds:** FEX's ARM64EC development guide documents its emulator DLL and Wine integration: https://wiki.fex-emu.com/index.php/Development:ARM64EC . Wine's ARM64EC/ARM64X support lets ARM64 Wine host native ARM64 DLLs while x64 PE code is dispatched to an ARM64EC emulator DLL. FEX upstream says its Wine/ARM64EC integration is still evolving; the FEX guide recommends its coordinated Wine fork for best results. Current GameNative/Proton-Wine release artifacts include ARM64EC builds: https://github.com/GameNative/proton-wine/releases .
- **ABI:** upstream Linux FEX and Wine builds target glibc distributions. GameNative/Winlator Android component packages are commonly **Bionic** builds and are not drop-in executables/libraries for our Debian glibc guest. Rebuild the paired Wine/FEX stack for glibc, or port the Android/Bionic integration deliberately; do not mix libc ABIs.
- **License:** FEX is MIT; Wine is LGPL-2.1-or-later; GameNative app/integration has its own GPL-3.0-or-later project license and bundled components retain their individual licenses. These are compatible with the GPL-3 APK, subject to preserving component notices/source obligations.
- **Maintenance:** active upstream FEX and Wine projects; ARM64EC is young and changes require matching Wine/FEX revisions. GameNative's release artifacts provide a useful moving reference but their Android packaging is not the target environment.
- **Fit:** best long-term ARM execution path if a compatible glibc package pair is built. Hades is x64, so it exercises ARM64EC dispatch. Validate title-specific middleware and graphics separately.

### x86_64 Wine/WoW64 under Box64

- **Source/builds:** Box64 documents Wine variants and setup at https://github.com/ptitSeb/box64/blob/main/docs/WINE.md . x86_64 Wine builds can come from Debian/Kron4ek or Proton builds; Kron4ek publishes x86_64 and x86_64-WoW64 archives at https://github.com/Kron4ek/Wine-Builds/releases . Debian trixie PC guest apt metadata currently offers Wine 10.0 (`wine`, `wine64`, `wine64-tools`); it offers no Box64 candidate. Phone guest already has Box64 0.3.4 from trixie arm64 (T024).
- **ABI:** Wine and its Linux ELF dependencies are **glibc x86_64**. Box64 translates the x86_64 Wine Unix processes and wraps host Linux/glibc calls; LodOS guest provides the ARM64 glibc environment. Use Wine's new WoW64 mode for 32-bit Windows programs without an i386 Linux runtime; older mixed Wine builds need Box32/Box86 and extra libraries. Box64's current docs say Arm64EC support is planned, not available, so this path runs the whole Wine Unix side through Box64.
- **License:** Box64 is MIT; Wine LGPL-2.1-or-later. Kron4ek build scripts are MIT, while Wine remains LGPL. Compatible with GPL-3 APK distribution when notices and corresponding sources are handled.
- **Maintenance:** Box64 is active but T024 pins an older Debian build; upstream 0.3.8+ documents experimental WOW64 Wine usage. Wine stable and community Proton builds are maintained, but this combination needs per-update regression checks and has more emulated Wine overhead.
- **Fit:** easiest immediate comparison and a practical fallback. Hades x64 avoids the 32-bit WoW64 path initially. Test x64 game launch before deciding if 32-bit support matters.

### Winlator / GameNative / Hangover builds

- **Where they come from:** Winlator source/releases: https://github.com/brunodev85/Winlator . GameNative source and component ecosystem: https://github.com/GameNative . Hangover project/releases: https://github.com/AndreRH/hangover . They package Wine/Proton, Box64 or FEX, DXVK/VKD3D, wrappers and Android-specific launch/configuration. GameNative currently publishes ARM64EC Proton builds; Hangover provides ARM64 Wine with x86/x64 execution using Box64/FEX components.
- **ABI:** Android Winlator/GameNative packages often use **Bionic** directly (and some variants include glibc containers). Hangover's ordinary Linux builds target **glibc**. Android Bionic package binaries cannot simply be placed in our glibc proot; reuse their source, patches and component configuration, or adapt/build explicitly for glibc.
- **License:** Winlator app is MIT; GameNative app is GPL-3.0-or-later; Hangover is LGPL-2.1. Embedded Wine, Box64/FEX, DXVK and drivers keep their own licenses. All main project licenses are GPL-3 compatible; audit each shipped artifact and preserve notices.
- **Maintenance:** active, device-facing projects can move quickly and are valuable integration references. Their Android lifecycle, Bionic loader, graphics wrappers and container assumptions differ from LodOS; adopting a complete APK is not the same as reusing a Linux guest runtime.
- **Reuse decision:** use these projects as reference implementations for patches/configuration and compare their Wine/FEX revisions, not as binary dependencies for the first glibc guest spike. Hangover is a worthwhile later comparison if FEX ARM64EC integration proves difficult.

## DXVK/VKD3D

- DXVK 2.x and current 3.x implement D3D8-11 over Vulkan. Current DXVK 3.x requires Vulkan 1.3 plus `VK_EXT_robustness2`, depth-clip-enable, transform feedback, maintenance5/6 and other features; it explicitly does not first-party support Android/mobile drivers: https://github.com/doitsujin/dxvk/wiki/Driver-support . Older DXVK 1.10.3 needs Vulkan 1.1 and is a reasonable legacy diagnostic baseline, not a promise of correctness.
- VKD3D-Proton implements D3D12 over Vulkan. Its current requirements include Vulkan 1.3, descriptor indexing, robustness2 and push descriptors: https://github.com/HansKristian-Work/vkd3d-proton . This is not needed to start Hades if its selected renderer is D3D11; identify the actual game API before adding components.
- Our Mali-G720 ICD reports Vulkan 1.3.278, but [mali-g720-vk.md](mali-g720-vk.md) records missing robustness2 and BCn texture compression, no shader clip/cull distance, one viewport, and missing pipeline-library extension. Therefore current DXVK/VKD3D requirements are not met. In particular, absent robustness2 blocks current DXVK; BCn needs the planned R2 transcode/workaround path. A Vulkan 1.3 version string alone is insufficient. First phone spike should use WineD3D/software or Vulkan capability diagnostics; DXVK on this ICD needs an explicit compatibility layer/driver fix before expected gameplay.

## Proot concerns

- **Wineserver and sockets:** wineserver uses per-user runtime state and Unix-domain sockets under the Wine server directory (normally `$WINEPREFIX`/user runtime paths). Keep `HOME`, `WINEPREFIX`, `TMPDIR` and `XDG_RUNTIME_DIR` stable across launcher and game; ensure the directory is writable and shared by all guest processes. Avoid host/guest Wine processes sharing the same prefix. Proot path translation and process teardown can leave stale endpoints; test `wineserver -k` and clean prefix ownership/state after interrupted runs.
- **Shared memory:** proot guest `/dev/shm` should be a writable, adequately sized shared tmpfs/backing directory. Existing `pc-guest.sh` binds a dedicated shared-memory directory and phone runner binds its guest shm. Check `df -h /dev/shm`, `ipcs -m`, and concurrent Wine process behavior; some apps use SysV IPC and require proot `--sysvipc` (PC runner already enables it).
- **esync/fsync/ntsync:** esync relies on file-descriptor/event mechanisms and can hit fd limits; test `ulimit -n` and fallback with `WINEESYNC=0`. fsync/ntsync generally need kernel support/device nodes or privileges that rootless proot cannot provide. Do not assume Android's host kernel exposes a usable ntsync device inside the guest. Start with both esync/fsync disabled, then test each independently; retain wineserver synchronization fallback. Proot intercepts syscalls and may affect futex/event behavior.
- **R5:** measure startup, prefix creation, loading, frame pacing and process cleanup under the actual phone kernel/proot. Confirm socket and shm paths survive game child-process launches. No existing PC x86_64 success proves ARM64 proot behavior.

## Recommendation

**Primary:** paired glibc ARM64EC Wine + FEX runtime, because it executes Wine's Unix side natively and matches the ARM64EC direction in project decision D7. Build from pinned FEX/Wine sources using the FEX ARM64EC guide; do not copy Bionic-only GameNative packages. **Fallback:** Box64 0.3.4+ with x86_64 Wine WoW64 (and upgrade Box64 if the 0.3.4 build fails Wine startup), since it directly extends the already verified T024 guest and requires no ARM64EC support in Box64.

**PC-first spike in the existing x86_64 T036 guest:**

```sh
scripts/pc-guest.sh run apt-get update
scripts/pc-guest.sh run apt-get install -y wine wine64 winetricks cabextract
scripts/pc-guest.sh run wine --version
scripts/pc-guest.sh run env WINEPREFIX=/root/.wine-hades WINEDEBUG=-all wineboot -u
```

Use the owner's legally obtained Hades Windows x64 files (Steam or GOG); do not download game files from a third party. With Steam/GOG files staged into `/root/games/Hades`, run `scripts/pc-guest.sh run env WINEPREFIX=/root/.wine-hades wine /root/games/Hades/x64/Hades.exe` (adjust executable path to the install). First establish WineD3D launch and log output. Query Vulkan availability with `scripts/pc-guest.sh run vulkaninfo --summary`; if the guest has no working Vulkan device, record that and keep the spike to Wine startup/API diagnostics. If Vulkan is available, test a pinned DXVK 1.10.3 release archive from https://github.com/doitsujin/dxvk/releases/tag/v1.10.3 using its `setup_dxvk.sh install` in that prefix, then compare with current DXVK only after checking required extensions. T036 is x86_64 native, so this validates Wine/prefix/game behavior, not emulation.

**Phone ARM64 spike:**

1. In a throwaway Debian arm64 proot prefix, record `uname -m`, `ldd --version`, Vulkan summary/extensions and `/dev/shm` capacity; check existing installed `box64 -v`.
2. Build/install a matched glibc FEX ARM64EC DLL set and the FEX-coordinated Wine fork in guest-private `/opt/lodos-wine-arm64ec`; use an isolated `/data` equivalent under guest home for `WINEPREFIX`. Follow FEX's published ARM64EC configure path (`./configure --enable-archs=arm64ec,aarch64,i386 --prefix=/usr --with-mingw=clang --disable-tests`) with its ARM64EC toolchain first on PATH, and install `libarm64ecfex.dll` / WoW64 DLLs per that Wine/FEX revision's guide. Pin both source commits and preserve licenses.
3. If paired build/runtime fails before creating a prefix, fallback spike: fetch Kron4ek's latest `amd64-wow64` Wine archive from https://github.com/Kron4ek/Wine-Builds/releases, extract into guest `/opt/wine-x86_64`, set `BOX64_LD_LIBRARY_PATH`/`BOX64_PATH` per T024, and run `box64 /opt/wine-x86_64/bin/wine64 wineboot -u` with a fresh prefix. Capture `BOX64_LOG=0` because logging to stdout can break wide-oriented output.
4. Run the same Hades executable with WineD3D first. Save Wine/Box64/FEX logs and compare loader, prefix, intro/menu, and 10-minute gameplay. Then probe DXVK separately: the phone ICD's missing robustness2 and BCn mean current DXVK is expected to reject or fail; test DXVK 1.10.3 only as a baseline and record the precise failure before planning R2/robustness work.

## Sources

- FEX ARM64EC development and Wine integration: https://wiki.fex-emu.com/index.php/Development:ARM64EC
- FEX upstream source/license/releases: https://github.com/FEX-Emu/FEX
- Box64 Wine guidance and status: https://github.com/ptitSeb/box64/blob/main/docs/WINE.md
- Box64 upstream source/license: https://github.com/ptitSeb/box64
- GameNative Proton-Wine ARM64EC release artifacts: https://github.com/GameNative/proton-wine/releases
- GameNative repositories: https://github.com/GameNative
- Winlator source/license: https://github.com/brunodev85/Winlator
- Hangover source/license/releases: https://github.com/AndreRH/hangover
- Kron4ek Wine builds and WoW64 archives: https://github.com/Kron4ek/Wine-Builds/releases
- DXVK Vulkan feature requirements and releases: https://github.com/doitsujin/dxvk/wiki/Driver-support ; https://github.com/doitsujin/dxvk/releases
- VKD3D-Proton driver requirements: https://github.com/HansKristian-Work/vkd3d-proton
- Wine source and license: https://gitlab.winehq.org/wine/wine
- Local Vulkan evidence: `docs/research/mali-g720-vk.md` and `docs/research/mali-g720-vk.json` (measured by LodOS `vkprobe`, 2026-11-26).
