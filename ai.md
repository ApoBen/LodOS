# ai.md — LodOS state (AI-oriented, terse; read FIRST, update after every step)

## META
created:2026-11-26 | lang(user):tr | status:RUNNING via systemd --user lodos.service (orch/run.sh)
roles: claude=coordinator/reviewer ONLY | agy=implementer
agy:~/.gemini/bin/agy -p "<task>" --model M --output-format json --dangerously-skip-permissions --add-dir /mnt/SSD/LodOS
models: default=gemini-4-neon-high | hard=gemini-4-carbon-high | trivial=gemini-4-neon-low
rules: minimize claude tokens; never re-read chat history; this file = source of truth; daily checklist -> ~/Masaüstü/LodOS-YYYY-MM-DD.md
orch: orch/run.sh loop(next todo task->agy->accept script->git commit; last attempt=carbon; max fail->blocked) | review.sh=claude -p checkpoint every 2 done/blocked or when queue empty (cooldown 1h on claude limit) | checklist.sh->desktop | tasks: orch/tasks/T###.md (FORMAT.md) accept: orch/accept/
shutdown policy: agy quota -> claude implements (3h agy cooldown), then agy resumes; alternates forever | claude limit while agy out -> shutdown (waits if agy back <=90min) | no runnable work -> notify 5min w/ cancel -> systemctl poweroff | touch orch/NO_SHUTDOWN disables
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
D15 app sockets: path AF_UNIX under <filesDir>/run (T017); adb spikes keep abstract + native/unix2abstract shim
D16 keep >=2 needs_phone:no tasks queued so runner doesn't idle/poweroff when phone drops
D17 desktop bring-up on PC first (x86_64 guest + lodos-wl headless, pixman) then port to phone; PNG screenshots in docs/screens/
D10 own repo of custom builds (mesa/zink, bridge, box64, fex, wine, dxvk)

## HARD LIMITS (unsolvable rootless)
real systemd; flatpak/snap/docker/podman (no userns); kernel anticheat; VA-API hw decode; raw sockets/<1024 ports/vpn/kmods; direct usb/bt; 16K-page devices problematic (future)

## RISKS
R1 mali vulkan bridge glibc<->bionic (boundary proven T019; remaining: full API surface/pNext/memory) | R2 mali lacks BCn etc -> transcode | R3 own wayland host (shm CPU path ok T022; zero-copy AHB/dmabuf pending) | R4 mutter can't nest in wayland -> headless+pipewire or patch | R5 fex under proot | R6 phantom process killer -> onboarding guide | R7 HyperOS blocks 1st adb install (guardprovider missing; manual 1st install worked) -> end users may hit MIUI restrictions | R8 app-uid: nativeLibDir exec + dma_heap ok (T017/T020) CLOSED | R10 proot rootfs+apt at app uid OK (T020) CLOSED; first-run extract slow 199s (T026) | R11 Mali DMA_BUF export broken (getMemoryFd fd=-1) -> export only via AHB; dma_heap ok at app uid | R12 vk-layer ICD handles (loader magic) -> bridge must use vk_icdGetInstanceProcAddr; libcutils non-NDK (T014) | R13 shell uid can't bind path AF_UNIX (SELinux); app uid OK (T017) -> abstract/unix2abstract only for adb spikes | R17 bridge mapped mem = memfd shadow + memcpy sync on flush/submit/wait (T029) -> perf cost for big buffers; later AHB-backed zero-copy | R14 ring spin vs futex: short spin -> 100x latency; need adaptive spin/per-thread tuning (power) | R15 proot cgroup2 ro -> elogind no sessions/polkit (T025) | R18 lodos-init guest musl static built (T039 CLOSED-ish; aarch64 run untested) | R19 phoc nests w/o new protocols (T037 CLOSED-ish); phoc ignores request_state resize -> mode pinned via phoc.ini | R16 phone adb drops on PC suspend/resume -> phone tasks stall | R20 zink over ICD: 354 GDPA misses (T041); no WSI/swapchain in bridge -> present must go via lodos-wl AHB/shm path (T041 sync fds) | R21 rootless => no zram for lite mode; use smaller rootfs/mem limits | R9 lxc rootfs URLs rotate ~3d -> fetch script resolves latest; pin own mirror for release

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
[x] T016 S3: phosh/phoc in guest; headless phoc+phosh alive via native/unix2abstract LD_PRELOAD; elogind conflicts systemd (dbus-user-session)
[x] T017 app-uid: jniLibs exec ok, proot -0 ok, path AF_UNIX bind in filesDir ok (R13 shell-only)
[x] T018 vk-bridge e2e v0: rpcgen, handle table, Mali e2e; p50 3-5us
[x] T019 glibc client in proot -> bionic server OK (R1 boundary proven; no proot overhead on spin path)
[x] T020 app-uid rootfs ok (same proot flags, +INTERNET perm); dma_heap open+alloc ok at app uid; cold extract 199s (slow)
[x] T021 S3 init: sysvinit+elogind, system bus+login1 ok, phosh login1-warn=0; no sessions (cgroup ro), Inhibit FileNotFound
[x] T022 S2 v0: lodos-wl JNI->SurfaceView, wl_shm green ok; CPU copy 512² ~1.5-2ms, 60fps vsync
[x] T023 S1 guest ICD v0: vulkaninfo --summary shows Mali-G720 (35 cmds, 119 calls); GDPA cmds missing
[x] T024 S4 box64 0.3.4 in proot: hello/bash ok, bench native 216ms vs box64 801ms (BOX64_LOG>=1 breaks wide stdout)
[ ] T025 S3 sessions: fake cgroup2 (statfs shim) -> loginctl session + Inhibit (neon) a1: phone dropped; shim extended, untested
[ ] T026 P1 native rootfs extract in APK <60s cold (Rust, neon)
[ ] T027 S1 v1: guest compute+offscreen triangle via bridge, shared mapped mem, batched cmdbufs (carbon)
[ ] T028 S2 v1: app-uid proot guest wl_shm client (weston-simple-shm) -> DisplayActivity (neon)
[x] T029 S1 ICD 92 cmds, memfd-shadow mapped mem, PC-host compute e2e via ICD on Intel Arc OK
[x] T030 P1 wizard skeleton: SetupStateMachine+RootfsInstaller(fake repo)+SetupActivity, 11 JVM tests
[x] T031 D3 native/lodos-init (init+systemctl shim, 14 cargo tests); bionic build only -> guest needs musl/gnu target
[x] T032 S1 ICD 135 cmds, PC-host offscreen triangle via ICD on Intel OK (gaps: void* ptrs, WSI/AHB fd cmds, batching)
[x] T033 native/lodos-rootfs: gz/xz/zst, traversal guards, JNI .so 799K, 9 tests; PC gz 1.1s/xz 2.9s
[x] T034 lodos-wl headless feature: RGBA fb + PPM dump (SIGUSR1/DUMP_AFTER), seat ptr/touch/kbd inject, 4 tests
[x] T035 ICD 264 cmds (VK1.0-1.3+zink exts, ext_memory_fd), zink GL4.6 triangle on Intel via ICD; 370 GDPA misses (WSI, sync fd, VK1.4)
[x] T036 PC x86_64 trixie guest: static x86_64 proot, scripts/pc-guest.sh, phoc/phosh/foot installed (claude-impl)
[x] T037 phoc 0.46 pixman nested on lodos-wl headless 720x1600 + foot, docs/screens/T037-phoc-foot.png; +xdg-decoration stub, phoc.ini mode pin (claude-impl)
[x] T038 phosh lockscreen nested on PC (dbus+stock elogind), docs/screens/T038-phosh.png = FIRST DESKTOP (claude-impl)
[x] T039 lodos-init static musl x86_64/aarch64 (rust-lld, crt-static), 16 tests; aarch64 not exec-tested (claude-impl)
[x] T040 bugreport core+collector+own FileProvider, redaction, 7 JVM tests; ISSUE_REPO_URL placeholder; menu only via SetupActivity button (claude-impl)
[x] T041 sync fd sem/fence via SCM_RIGHTS, map_memory2, *2 cmds; GDPA misses 370->354; host_syncfd OK on Arc (claude-impl)
[ ] T042 phosh unlock+grid+foot via lodos-wl ctl socket inject (wlctl.py), 3 PNGs (carbon, no phone)
[ ] T043 phosh on phone adb spike (aarch64 guest, lodos-wl headless bionic), PNG (neon, phone)
[ ] T044 lite mode core: tier classify+profile+guest env/gsettings, >=6 JVM tests (neon, no phone)
[ ] T045 PC guest via lodos-init PID1 -> dbus/elogind/phoc/phosh PNG (neon, no phone)
(live status: task files frontmatter)

## OPEN (need user)
- 2026-11-27 USER: before GitHub release add (a) in-app bug-report/log collector (one button: logs+device info -> shareable file/issue template), (b) lite mode for 400-600k AnTuTu / 4-6GB devices (no animations, zram, smaller rootfs). Later Claude manages community (issues, contributors).
- 2026-11-27 USER DIRECTIVE: priority = first visible desktop (phosh). phone away ~09:30-evening -> plan PC-only desktop path now (lodos-wl headless/xdg-shell/seat on PC, phoc nested via wayland backend WLR_RENDERER=pixman, then phosh, screenshot proof); phone tasks after replug. GPU accel (zink over bridge) later.
(none)

## LOG (newest first, 1 line each)
2026-11-27 review19: T040,T041 ok (accept PASS: 7 JVM tests; syncfd SYNC_FD+OPAQUE on Arc); phone off adb; +T044 lite mode core, T045 lodos-init PID1 PC phosh (PC-only)
2026-11-27 review18: T038,T039 ok (accept PASS; PNG verified phosh lockscreen = first desktop); +T042 unlock/grid via inject, T043 phosh on phone
2026-11-27 review17: T036,T037 ok (claude-impl during agy quota; accept PASS, PNG verified: foot in nested phoc); next T038 phosh first desktop; queue T038-T041 PC-only
2026-11-27 review16: T034,T035 ok (accept PASS: 4 wl tests+PPM; zink triangle via ICD); phone off adb; only T036 runnable PC -> +T039 init musl, T040 bugreport, T041 syncfd
2026-11-27 review15: T024,T033 ok (accept PASS: box64 hello/bash/bench; 9 cargo tests); T025 a1 phone drop (todo); phone away -> +T036-T038 PC desktop path (directive)
2026-11-27 review14: T031,T032 ok (accept PASS: 14 cargo tests; Intel triangle via ICD 110 calls); phone back on adb -> T024-T028 runnable; +T035 zink over ICD
2026-11-27 review13: T029,T030 ok (accept PASS: Intel compute via ICD, 11 JVM tests); phone still off adb; +T032 host triangle, T033 rootfs extractor, T034 wl headless (PC-only)
2026-11-27 review12: T024 a1 fail (phone disconnected, not quality) -> REVIEW hint, max4; all phone tasks stalled -> +T029 vk host compute, T030 wizard, T031 lodos-init (PC-only)
2026-11-27 review11: T022,T023 ok (carbon, accept PASS: green pixels, guest vulkaninfo Mali); untracked __pycache__; +T027 vk work, T028 guest display
2026-11-27 review10: T020,T021 ok (agy neon, accept PASS); +T024 box64, T025 elogind sessions, T026 fast extract
2026-11-27 review9: T018,T019 ok (accept PASS, Mali via proot glibc client); +T023 guest ICD
2026-11-27 review8: T016,T017 ok (accept PASS); +T020 app-uid rootfs, T021 elogind init, T022 first pixels
2026-11-27 review7: T014,T015 ok (accept PASS on phone); +T018 vk e2e, T019 glibc client; R14
2026-11-27 review6: T012 unblocked (locale bug, fixed run.sh:105 LC_ALL=C) -> done; T013 ok; +T017 app-uid probe
2026-11-26 review5: T010,T011 ok (claude-impl, accept PASS); +T014 vk-layer cleanup, T015 transport, T016 phosh probe
2026-11-26 review4: T008,T009 ok (claude-impl); +T012 vk codegen, T013 wayland srv
2026-11-26 review3: T006,T007 ok (claude-implemented, agy quota); +T011 guest apt
2026-11-26 review2: T002-T005 ok; +T008 rust, T009 ahb probe, T010 vk-layer BC1; D13
2026-11-26 review1: T001 ok; T002->user (HyperOS adb install); +T005-T007 (rootfs/proot via adb shell)
2026-11-26 T000 orchestration built, service enabled, T001-T004 queued
2026-11-26 seeded ai.md; plan agreed
