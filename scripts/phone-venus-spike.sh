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
BASELINE_PIDFILE=/data/local/tmp/lodos/T055-bridge.pid
BRIDGE_FPS=0
mkdir -p build docs/research
: >"$LOG"
cleanup() {
  if [ -n "${SERIAL:-}" ]; then
    adb -s "$SERIAL" shell "for f in $PIDFILE $BASELINE_PIDFILE; do if [ -f \$f ]; then while read p; do kill \$p 2>/dev/null || true; done < \$f; rm -f \$f; fi; done" >/dev/null 2>&1 || true
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

if timeout 600 env NOBUILD=1 bash scripts/guest-vkwork-test.sh >>"$LOG" 2>&1; then
  # vkcube in Debian vulkan-tools has WSI only; the guest has no WSI socket.
  # Record a clearly identified offscreen triangle throughput proxy instead.
  adb -s "$SERIAL" shell "cd /data/local/tmp/lodos/vkb-icd && (LODOS_VK_SOCK=@lodos-vk-g ./vkb_server > /data/local/tmp/lodos/T055-bridge.log 2>&1 & echo \$! > $BASELINE_PIDFILE)"
  sleep 0.5
  if timeout 120 bash scripts/phone-run.sh "LODOS_VK_SOCK='@lodos-vk-g' VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lodos_icd.json' python3 -c 'import os,subprocess,time; n=12; t=time.perf_counter(); env=dict(os.environ); [subprocess.run(['/root/vkb-icd/triangle','/tmp/T055-triangle.ppm'],env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL) for _ in range(n)]; dt=time.perf_counter()-t; print(f'BRIDGE_TRIANGLE_FPS={n/dt:.6f}')'" >>"$LOG" 2>&1; then
    BRIDGE_FPS=$(sed -n 's/^BRIDGE_TRIANGLE_FPS=//p' "$LOG" | tail -1)
    [[ "$BRIDGE_FPS" =~ ^[0-9]+([.][0-9]+)?$ ]] || BRIDGE_FPS=0
  else
    blockers+=("Bridge offscreen triangle throughput proxy failed; see build/T055-venus.log")
  fi
  adb -s "$SERIAL" shell "kill \$(cat $BASELINE_PIDFILE) 2>/dev/null || true; rm -f $BASELINE_PIDFILE"
  blockers+=("Bridge vkcube FPS unavailable: guest vkcube requires a WSI socket; offscreen triangle throughput is a non-comparable proxy")
else
  blockers+=("Bridge baseline unavailable: guest-vkwork-test.sh failed; see build/T055-venus.log")
  tail -12 "$LOG"
fi

BLOCKERS=$(printf '%s\n' "${blockers[@]}")
python3 - "$ROOT/docs/research/S1-venus.json.json" "${missing[*]}" "$BLOCKERS" "$BRIDGE_FPS" <<'PY'
import json,sys
j={'works':False,'vkcube_fps':None,'bridge_vkcube_fps':float(sys.argv[4]),
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
