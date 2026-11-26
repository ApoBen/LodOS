# ai.md — LodOS state (AI-oriented, terse; read FIRST, update after every step)

## META
created:2026-11-26 | lang(user):tr | status:RUNNING via systemd --user lodos.service (orch/run.sh)
roles: claude=coordinator/reviewer ONLY | agy=implementer
agy:~/.gemini/bin/agy -p "<task>" --model M --output-format json --dangerously-skip-permissions --add-dir /mnt/SSD/LodOS
models: default=gemini-4-neon-high | hard=gemini-4-carbon-high | trivial=gemini-4-neon-low
rules: minimize claude tokens; never re-read chat history; this file = source of truth; daily checklist -> ~/Masaüstü/LodOS-YYYY-MM-DD.md
orch: orch/run.sh loop(next todo task->agy->accept script->git commit; last attempt=carbon; max fail->blocked) | review.sh=claude -p checkpoint every 2 done/blocked or when queue empty (cooldown 1h on claude limit) | checklist.sh->desktop | tasks: orch/tasks/T###.md (FORMAT.md) accept: orch/accept/
shutdown policy: agy quota -> claude implements (3h agy cooldown) | claude limit while agy out -> shutdown | no runnable work -> notify 5min w/ cancel -> systemctl poweroff | touch orch/NO_SHUTDOWN disables
phone keeper: lodos-phone.service (orch/phone/watch.sh) runs keeper.sh on phone: brightness 0 after 1min no input while work active; restores on input/unplug/screen-off; phone batt<=15% -> lock phone, stop runner, commit, poweroff PC
sudo: none given; agents write NEEDS_ROOT in NOTES -> status user -> user runs cmd

## GOAL
single APK, rootless, NO termux, NO termux-x11. Full Linux on Android: GPU accel, wayland, GNOME-family UI (touch like ubuntu-touch), apt, x86_64 linux apps (box64/fex), wine, game mode. Order: phone -> tablet -> generalize -> github (GPL-3).

## ENV
pc: arch-based linux, 133G free on /mnt/SSD | sdk:~/development/android (platforms 33-36, build-tools 30/34/35, ndk 23.1 ONLY -> need newer ndk r27+) | jdk17
phone(P1): POCO X7 Pro "rodin" 2412DPC0AG, adb serial <phone-serial> usb, Dimensity8400 (8x A725, no LITTLE), Mali-G720, 12G, HyperOS, 4K pages, no root
tablet(P2): Tab S9 Ultra, SD8G2/Adreno740, no root

## DECISIONS (D#)
D13 S1 bridge per T003: IPC (no in-process libhybris); host=bionic-vulkan-wrapper (BCn compute, Mali fixes) + own vk.xml-generated C bridge w/ per-thread shm rings + SCM_RIGHTS
D1 base distro=Debian 13 (elogind avail, GNOME48 still non-systemd builtin mode, Mobian proves phosh). fedora/arch later as alt rootfs.
D2 UI: phone=Phosh(phoc/wlroots nested) | tablet=GNOME (risk R4) | fallback=Plasma Mobile
D3 no systemd: mini-init + dbus + elogind + systemctl-shim
D4 exec: proot shipped as lib*.so in APK, guest exec via proot loader mmap -> modern targetSdk OK (UserLAnd precedent)
D5 display: own Android-side minimal Wayland host (libwayland-server, xdg-shell, linux-dmabuf, input) -> SurfaceView via AHardwareBuffer; compositor (phoc/mutter) nested inside
D6 GPU mali: Vortek-like bridge (guest glibc vulkan ICD -> IPC -> host bionic Mali vulkan); GL via zink. Same process as wayland host => zero-copy present. adreno: turnip kgsl build.
D7 compat: box64 primary, FEX experimental; wine ARM64EC + fex/box64 wow64
D8 game mode: per-game fps cap, thermal-headroom auto fps (Android thermal API), PerformanceHint, FSR scale, mangohud, touch controls, profiles (Hades first)
D9 dist: github releases + f-droid + obtainium; google dev-verification handled at release time
D11 langs: Kotlin=android app/UI/lifecycle | Rust=new native daemons (wayland host via smithay, audio bridge, mini-init/logind shim, launcher, IPC) via cargo-ndk | C=vulkan ICD/bridge + patches to existing C projects (proot, mesa, box64)
D12 until apk install works: spike native bits via adb push /data/local/tmp (shell uid) — proot/vkprobe/rootfs
D14 runner runs accept scripts with LC_ALL=C (tr_TR breaks [A-Za-z]/case ops)
D10 own repo of custom builds (mesa/zink, bridge, box64, fex, wine, dxvk)

## HARD LIMITS (unsolvable rootless)
real systemd; flatpak/snap/docker/podman (no userns); kernel anticheat; VA-API hw decode; raw sockets/<1024 ports/vpn/kmods; direct usb/bt; 16K-page devices problematic (future)

## RISKS
R1 mali vulkan bridge glibc<->bionic (biggest) | R2 mali lacks BCn etc -> transcode | R3 own wayland host | R4 mutter can't nest in wayland -> headless+pipewire or patch | R5 fex under proot | R6 phantom process killer -> onboarding guide | R7 HyperOS blocks 1st adb install (guardprovider missing; manual 1st install worked) -> end users may hit MIUI restrictions | R8 shell-uid probes (dma_heap, /data/local/tmp exec) may differ from app uid -> re-verify in APK | R10 proot -r rootfs (link2symlink, sysvipc) only shell-uid tested; apt/dpkg in guest unproven (T011) | R11 Mali DMA_BUF export broken (getMemoryFd fd=-1) -> export only via AHB; app-uid dma_heap untested | R12 vk-layer ICD handles (loader magic) -> bridge must use vk_icdGetInstanceProcAddr; libcutils non-NDK (T014) | R13 shell uid can't bind path AF_UNIX in /data/local/tmp (SELinux) -> abstract sockets; multi-client nested compositor needs path at app uid or proot path->abstract translation (T017) | R14 ring spin vs futex: short spin -> 100x latency; need adaptive spin/per-thread tuning (power) | R9 lxc rootfs URLs rotate ~3d -> fetch script resolves latest; pin own mirror for release

## PHASES
P0 spikes: S1 mali bridge vkcube+zink glxgears | S2 wayland host+nested phoc+zero-copy | S3 phosh/gnome48 w/ elogind+mini-init (+test systemd --user standalone) | S4 box64+wine arm64ec in proot, Hades
P1 phone MVP apk (wizard, rootfs dl, start, audio, input, /sdcard)
P2 compat (box64/fex/wine, smart launcher, own pkg repo)
P3 game mode
P4 tablet (turnip, gnome, dex)
P5 generalize + github + CI

## TASKS (queue) [ ]=todo [~]=doing [x]=done
[x] T000 orchestration (claude, 2026-11-26)
[x] T001 toolchain: ndk r30 (30.0.16248370), cmake 4.1.2, orch/env.sh
[x] T002 app skeleton org.lodos.app (AGP8.9.3/Gradle8.11.1/Kt2.1.21); user did 1st install manually, adb updates OK
[x] T003 S1 research -> docs/research/S1-gpu-bridge.md (IPC bridge, vk.xml codegen, reuse bionic-vulkan-wrapper host layer)
[x] T004 vkprobe mali -> docs/research/mali-g720-vk.{json,md}: no BCn/robustness2/clip-cull/multiViewport/fp64
[x] T005 scripts/fetch-rootfs.sh lxc trixie arm64 (13.7) + docker fallback
[x] T006 proot (termux c0b2d098+talloc static, NDK r30) runs -0 on phone
[x] T007 proot+debian trixie on phone: scripts/phone-rootfs.sh, phone-run.sh (xz->gz recompress)
[x] T008 rust 1.97.1+cargo-ndk 4.1.2, native/hello-rs, scripts/build-rs.sh
[x] T009 tools/ahbprobe: AHB alloc ok, Vk AHB import zero-copy ok, dma_heap/system ok, DMA_BUF import-only, sync_fd ok -> docs/research/ahb-dmabuf.md
[x] T010 bionic-vulkan-wrapper NDK r30 build (patch 0001) + BC1 GPU decode ok on Mali; deps libcutils(non-NDK), exports all
[x] T011 scripts/guest-setup.sh: apt/dpkg/dns ok, /dev/shm bind, sysvipc emulated ok
[x] T012 vk-bridge codegen v0: gen.py 10 cmds roundtrip ok (gaps: pNext, handle map, reply/out dir, bounds)
[x] T013 native/lodos-wl smithay 0.7 on phone, guest wayland-info ok; shell uid path-socket EACCES -> abstract @lodos-*
[x] T014 vk-layer: libcutils dropped (NDK prop shim), exports=3 vk_icd*, 9.5MB
[x] T015 transport v0: memfd SPSC ring+futex, SEQPACKET SCM_RIGHTS; phone 1.8GB/s, p50 0.39us (spin-dependent; futex path slow ~113us)
[ ] T016 S3 probe: phosh/phoc in guest, headless phoc under proot (neon)
[ ] T017 app-uid probe: proot+lodos-wl as jniLibs, path socket + proot -0 at app uid (deps T006,T013; neon)
[ ] T018 vk-bridge e2e v0: 6 cmds client->server @lodos-vk on Mali, handle map/replies (carbon)
[ ] T019 guest glibc client in proot -> bionic server (R1 proof; deps T018)
(live status: task files frontmatter)

## OPEN (need user)

## LOG (newest first, 1 line each)
2026-11-27 review7: T014,T015 ok (accept PASS on phone); +T018 vk e2e, T019 glibc client; R14
2026-11-27 review6: T012 unblocked (locale bug, fixed run.sh:105 LC_ALL=C) -> done; T013 ok; +T017 app-uid probe
2026-11-26 review5: T010,T011 ok (claude-impl, accept PASS); +T014 vk-layer cleanup, T015 transport, T016 phosh probe
2026-11-26 review4: T008,T009 ok (claude-impl); +T012 vk codegen, T013 wayland srv
2026-11-26 review3: T006,T007 ok (claude-implemented, agy quota); +T011 guest apt
2026-11-26 review2: T002-T005 ok; +T008 rust, T009 ahb probe, T010 vk-layer BC1; D13
2026-11-26 review1: T001 ok; T002->user (HyperOS adb install); +T005-T007 (rootfs/proot via adb shell)
2026-11-26 T000 orchestration built, service enabled, T001-T004 queued
2026-11-26 seeded ai.md; plan agreed
