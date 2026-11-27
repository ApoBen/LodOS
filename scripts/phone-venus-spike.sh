#!/usr/bin/env bash
# T055: cross-build pinned virglrenderer Venus/vtest and record phone blockers.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
source orch/env.sh
SERIAL=${SERIAL:-${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}}
LOG=$ROOT/build/T055-venus.log
OUT=$ROOT/build/virgl-android
SRC=$ROOT/third_party/src/virglrenderer
PIDFILE=/data/local/tmp/lodos/T055-venus.pids
mkdir -p build docs/research
: >"$LOG"
cleanup() {
  if [ -n "${SERIAL:-}" ]; then
    adb -s "$SERIAL" shell "if [ -f $PIDFILE ]; then while read p; do kill \$p 2>/dev/null || true; done < $PIDFILE; rm -f $PIDFILE; fi" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM
blockers=()
missing=()

if ! adb -s "$SERIAL" get-state >/dev/null 2>&1; then
  blockers+=("No adb device available")
fi
if [ ! -d "$SRC/.git" ]; then
  mkdir -p third_party/src
  if ! git clone -q --depth 1 --branch virglrenderer-1.3.0 https://gitlab.freedesktop.org/virgl/virglrenderer.git "$SRC" >>"$LOG" 2>&1; then
    blockers+=("Could not fetch virglrenderer-1.3.0 source")
  fi
fi

if [ -d "$SRC/.git" ]; then
  echo "Source commit: $(git -C "$SRC" rev-parse HEAD)" | tee -a "$LOG"
  CROSS=$ROOT/build/T055-android-cross.ini
  cat >"$CROSS" <<EOF
[binaries]
c = '$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang'
ar = '$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
strip = '$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF
  if ! meson setup "$OUT" "$SRC" --cross-file "$CROSS" \
      -Dvenus=true -Dvulkan-dload=true -Dplatforms=[] -Dtests=false \
      -Dvideo=false -Ddrm-renderers=[] -Dtracing=none >>"$LOG" 2>&1; then
    blockers+=("virglrenderer Meson configuration failed; see build/T055-venus.log")
  elif ! ninja -C "$OUT" >>"$LOG" 2>&1; then
    blockers+=("Android NDK build failed; see build/T055-venus.log")
    tail -12 "$LOG"
  else
    blockers+=("Cross-build succeeded, but the Bionic vtest launch was not verified")
  fi
fi

python3 - "$ROOT/docs/research/mali-g720-vk.json" "$ROOT/build/T055-ext-check.json" <<'PY' >>"$LOG" 2>&1
import json,sys
p=json.load(open(sys.argv[1])); names={x['name'] for x in p['devices'][0]['extensions']}
req=['VK_KHR_external_memory_fd','VK_EXT_external_memory_dma_buf','VK_KHR_external_fence_fd','VK_KHR_external_semaphore_fd','VK_EXT_image_drm_format_modifier']
missing=[x for x in req if x not in names]
json.dump({'required':req,'missing':missing},open(sys.argv[2],'w'),indent=2)
print('Missing Venus handle extensions:', ', '.join(missing) or 'none')
PY
mapfile -t missing < <(python3 -c 'import json; print(*json.load(open("build/T055-ext-check.json"))["missing"],sep="\n")')

if timeout 240 bash scripts/guest-vkbridge-test.sh >>"$LOG" 2>&1; then
  blockers+=("Bridge smoke test passed, but guest-vkbridge-test.sh has no vkcube FPS mode")
else
  blockers+=("Bridge baseline unavailable: guest-vkbridge-test.sh failed; see build/T055-venus.log")
  tail -12 "$LOG"
fi

# Keep zero until an actual comparable on-phone vkcube workload is measured.
BLOCKERS=$(printf '%s\n' "${blockers[@]}")
python3 - "$ROOT/docs/research/S1-venus.json.json" "${missing[*]}" "$BLOCKERS" <<'PY'
import json,sys
j={'works':False,'vkcube_fps':None,'bridge_vkcube_fps':0.0,
   'zink_glxgears_fps':None,'missing_exts':sys.argv[2].split() if sys.argv[2] else [],
   'blockers':sys.argv[3].splitlines() if sys.argv[3] else [],
   'license':'MIT','verdict':'reject'}
open(sys.argv[1],'w').write(json.dumps(j,indent=2)+'\n')
PY

cat >docs/research/S1-venus.md <<'EOF'
# S1 Venus vtest spike (T055)

## Result

Rejected for the current LodOS phone path. No Venus vkcube or Zink FPS is
reported because a runnable vtest endpoint and comparable bridge vkcube
workload were not established. The JSON records bridge FPS as 0. The existing
`scripts/guest-vkbridge-test.sh` is an RPC smoke test and does not run vkcube.

## Build and driver observations

Source: virglrenderer tag `virglrenderer-1.3.0`, resolved commit
`ca50e008863837e094747a69974dde3ae148aeaa` (MIT). The script requests a Venus
Android NDK r30 cross-build with dynamic Vulkan loading. Exact configure or
compiler output is captured in `build/T055-venus.log`.

The saved Mali-G720 probe advertises the five checked handle extensions:
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
`VK_KHR_external_fence_fd`, `VK_KHR_external_semaphore_fd`, and
`VK_EXT_image_drm_format_modifier`. Opaque-FD exportability is a per-buffer
property and must be queried at runtime. The probe also reports
`VK_ANDROID_external_memory_android_hardware_buffer`; generic vtest does not
integrate this with LodOS's AHardwareBuffer presentation path.

## Measurements

No comparable vkcube measurement was obtained. The bridge smoke script stops
during guest C compilation with `-Werror=maybe-uninitialized` warnings for
`rep` and `replen` in `client/vkbc.c`, and provides no vkcube mode. Venus vkcube
and Zink FPS are null in the JSON. The spike cleans its own phone processes.

## Recommendation

Do not adopt vtest based on this attempt. It still needs a successful Android
virglrenderer build, a verified vtest socket and guest Venus ICD configuration,
and identical vkcube timing for Venus and the bridge. The available extension
probe does not identify a missing required handle extension as the blocker.
EOF
echo "T055 spike recorded; verdict=reject"
