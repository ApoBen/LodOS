# P1 setup rootfs

`SetupActivity` uses `HttpRootfsRepo` for first-run rootfs installation. It downloads into the app cache, resumes partial transfers with HTTP Range, verifies SHA-256, and calls the existing `lodos_rootfs` JNI extractor into app-private files. The debug-only `fake_repo=1` intent extra retains the deterministic UI fake.

`android/app/src/main/assets/rootfs-manifest.json` currently contains placeholder URLs and zero SHA-256 digests. Replace both with a project-hosted immutable archive URL and its independently verified digest before enabling first-run downloads for users.

## Background reliability onboarding

After rootfs setup, the Ready screen links to an optional guide that detects Android 12+ child-process limits, battery optimization, and Xiaomi-family background controls. It opens the relevant system settings where available, falls back to LodOS app details when an intent is unavailable, and shows an optional advanced ADB tip. Android 14+ users are pointed to the Developer options child-process restriction control.

The session launcher offers `/sdcard` access through Android's all-files settings on API 30+ and legacy storage permissions on API 29; the guest bind is enabled only while access is granted.

The settings screen persists `lite_mode` (`auto|on|off`), `fg_preset` (`off|quality|balanced|performance`), and `fg_sharpness` (0..1, default 0.2) in the `lodos_settings` preferences. Enabled upscale presets pass `LODOS_FG`, `LODOS_FG_PRESET`, and `LODOS_FG_SHARPNESS` to the guest.
