# Lite mode (T044)
Target: 400-600k AnTuTu, 4-6GB RAM. Core: `android/app/src/main/java/org/lodos/app/lite/LiteMode.kt` (pure JVM).

## Tiers (`LiteMode.classify`)
- LOW: RAM <= 6144MB OR cores <= 4 OR weak GPU (Mali-G5x/G6x, Adreno 5xx, Adreno 60x/61x)
- MID: RAM <= 8192MB
- HIGH: else (e.g. D8400 / Mali-G720 / 12GB)

## Profiles (`profileFor`)
| tier | animations | phoshAnim | rootfs | maxFps | compScale | zramMb |
|------|-----------|-----------|--------|--------|-----------|--------|
| LOW  | off | off | minimal | 30  | 0.75 | 0 |
| MID  | on  | off | full    | 60  | 1.0  | 0 |
| HIGH | on  | on  | full    | 120 | 1.0  | 0 |

User override (`LiteModeStore.userForce`): true -> LOW profile, false -> HIGH profile, null -> auto.

## Guest outputs
- `toGuestEnv`: LODOS_LITE, LODOS_MAX_FPS, LODOS_ROOTFS_FLAVOR, LODOS_COMPOSITOR_SCALE, GTK_ENABLE_ANIMATIONS=0 (lite only).
- `toGsettings`: `org.gnome.desktop.interface enable-animations <bool>`; phosh has no known dedicated animation key in `sm.puri.phosh` -> emitted as comment (phosh honours enable-animations).

## TODO
- zram: needs root (swapon/zram sysfs). Rootless (proot) => no zram; zramMb stays 0. Use guest-side memory limits instead: smaller "minimal" rootfs, fewer autostart services, lower fps, no animations.
- wire: AndroidDeviceSpec.read (GPU renderer from GL/Vulkan probe) -> resolve -> pass env to guest launch, apply gsettings via lodos-init.
- build/ship a "minimal" rootfs flavor.
- compositorScale/maxFps consumption in lodos_wl compositor.
- UI toggle for userForce.
