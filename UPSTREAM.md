# Upstream contributions (tracker)

Fixes/features we made in third-party projects. Details per patch: patches/<project>/README.md.
Flow: agent writes the patch + README -> reviewer marks "upstreamable: yes" candidates here -> Claude prepares the branch/fork + PR text -> **the user approves each PR** -> `gh pr create` (account: user's gh login). Respect each project's CONTRIBUTING (DCO sign-off, CLA, issue-first, code style, tests).

| project | patch | summary | upstreamable | status | link |
|---|---|---|---|---|---|
| bionic-vulkan-wrapper | 0002-lodos-no-cutils-icd-exports | drop libcutils.so (non-NDK) dep + vk_icd-only export map | yes | issue opened 2026-11-27 | https://github.com/leegao/bionic-vulkan-wrapper/issues/231 |
| mesa | 0001-egl-wayland-zink-kopper-without-render-node | honor LIBGL_KOPPER_DRI2 in Wayland EGL without a DRM render node | yes | candidate | — |
