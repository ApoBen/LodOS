# ai.md — LodOS state (AI-oriented, terse; read FIRST, update after every step)

## META
created:2026-11-26 | lang(user):tr | status:PRE-START (awaiting user go; orchestration not built)
roles: claude=coordinator/reviewer ONLY | agy=implementer
agy:~/.gemini/bin/agy -p "<task>" --model M --output-format json --dangerously-skip-permissions --add-dir /mnt/SSD/LodOS
models: default=gemini-4-neon-high | hard=gemini-4-carbon-high | trivial=gemini-4-neon-low
rules: minimize claude tokens; never re-read chat history; this file = source of truth; daily checklist -> ~/Masaüstü/LodOS-YYYY-MM-DD.md
user_req: autostart on boot; auto-shutdown when credits exhausted (details TBD, see OPEN)

## GOAL
single APK, rootless, NO termux, NO termux-x11. Full Linux on Android: GPU accel, wayland, GNOME-family UI (touch like ubuntu-touch), apt, x86_64 linux apps (box64/fex), wine, game mode. Order: phone -> tablet -> generalize -> github (GPL-3).

## ENV
pc: arch-based linux, 133G free on /mnt/SSD | sdk:~/development/android (platforms 33-36, build-tools 30/34/35, ndk 23.1 ONLY -> need newer ndk r27+) | jdk17
phone(P1): POCO X7 Pro "rodin" 2412DPC0AG, adb serial <phone-serial> usb, Dimensity8400 (8x A725, no LITTLE), Mali-G720, 12G, HyperOS, 4K pages, no root
tablet(P2): Tab S9 Ultra, SD8G2/Adreno740, no root

## DECISIONS (D#)
D1 base distro=Debian 13 (elogind avail, GNOME48 still non-systemd builtin mode, Mobian proves phosh). fedora/arch later as alt rootfs.
D2 UI: phone=Phosh(phoc/wlroots nested) | tablet=GNOME (risk R4) | fallback=Plasma Mobile
D3 no systemd: mini-init + dbus + elogind + systemctl-shim
D4 exec: proot shipped as lib*.so in APK, guest exec via proot loader mmap -> modern targetSdk OK (UserLAnd precedent)
D5 display: own Android-side minimal Wayland host (libwayland-server, xdg-shell, linux-dmabuf, input) -> SurfaceView via AHardwareBuffer; compositor (phoc/mutter) nested inside
D6 GPU mali: Vortek-like bridge (guest glibc vulkan ICD -> IPC -> host bionic Mali vulkan); GL via zink. Same process as wayland host => zero-copy present. adreno: turnip kgsl build.
D7 compat: box64 primary, FEX experimental; wine ARM64EC + fex/box64 wow64
D8 game mode: per-game fps cap, thermal-headroom auto fps (Android thermal API), PerformanceHint, FSR scale, mangohud, touch controls, profiles (Hades first)
D9 dist: github releases + f-droid + obtainium; google dev-verification handled at release time
D10 own repo of custom builds (mesa/zink, bridge, box64, fex, wine, dxvk)

## HARD LIMITS (unsolvable rootless)
real systemd; flatpak/snap/docker/podman (no userns); kernel anticheat; VA-API hw decode; raw sockets/<1024 ports/vpn/kmods; direct usb/bt; 16K-page devices problematic (future)

## RISKS
R1 mali vulkan bridge glibc<->bionic (biggest) | R2 mali lacks BCn etc -> transcode | R3 own wayland host | R4 mutter can't nest in wayland -> headless+pipewire or patch | R5 fex under proot | R6 phantom process killer -> onboarding guide

## PHASES
P0 spikes: S1 mali bridge vkcube+zink glxgears | S2 wayland host+nested phoc+zero-copy | S3 phosh/gnome48 w/ elogind+mini-init (+test systemd --user standalone) | S4 box64+wine arm64ec in proot, Hades
P1 phone MVP apk (wizard, rootfs dl, start, audio, input, /sdcard)
P2 compat (box64/fex/wine, smart launcher, own pkg repo)
P3 game mode
P4 tablet (turnip, gnome, dex)
P5 generalize + github + CI

## TASKS (queue) [ ]=todo [~]=doing [x]=done
[ ] T0 orchestration: git init, CLAUDE.md/GEMINI.md pointers, runner script, systemd user units (autostart), credit/limit detection, shutdown policy, daily checklist gen
[ ] T1 toolchain: install NDK r27+, cmake, verify adb, create android project skeleton
[ ] T2 S1 research: inspect winlator vortek + termux vulkan wrapper sources/licenses
...

## OPEN (need user)
- shutdown trigger: which quota (claude/agy/both)? grace period?
- confirm autonomy scope for agents on phone via adb

## LOG (newest first, 1 line each)
2026-11-26 seeded ai.md; plan agreed; awaiting go
