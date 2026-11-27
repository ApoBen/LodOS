# P1 setup rootfs

`SetupActivity` uses `HttpRootfsRepo` for first-run rootfs installation. It downloads into the app cache, resumes partial transfers with HTTP Range, verifies SHA-256, and calls the existing `lodos_rootfs` JNI extractor into app-private files. The debug-only `fake_repo=1` intent extra retains the deterministic UI fake.

`android/app/src/main/assets/rootfs-manifest.json` currently contains placeholder URLs and zero SHA-256 digests. Replace both with a project-hosted immutable archive URL and its independently verified digest before enabling first-run downloads for users.

The session launcher offers `/sdcard` access through Android's all-files settings on API 30+ and legacy storage permissions on API 29; the guest bind is enabled only while access is granted.
