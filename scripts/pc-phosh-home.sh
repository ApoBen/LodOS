#!/usr/bin/env bash
# T042 gesture driver, called by `pc-desktop.sh phosh-home` once phosh is up. args: ctl W H outdir
# All input goes through the lodos-wl control socket (tools/wlctl.py) -> wl_touch/wl_keyboard -> phoc -> phosh.
# Coordinates are fractions of W/H (phosh 0.46 layout, scale 1, measured at 720x1600).
set -euo pipefail
CTL=$1; W=$2; H=$3; OUT=$4
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
log() { echo "[phosh-home] $*" >&2; }
if [ "${PC_DESKTOP_HOLD:-0}" = 1 ]; then log "HOLD: ctl=$CTL (kill pc-desktop.sh to stop)"; while :; do sleep 5; done; fi
wl() { python3 -I tools/wlctl.py "$CTL" "$@"; }
px() { python3 -I -c "import sys; print(round(float(sys.argv[1]) * float(sys.argv[2])))" "$1" "$2"; }
X() { px "$1" "$W"; }; Y() { px "$1" "$H"; }
shot() { # name -> docs/screens/T042-phosh-<name>.png
  wl dump "$OUT/$1.ppm"; python3 -I scripts/ppm2png.py "$OUT/$1.ppm" "docs/screens/T042-phosh-$1.png" >&2
}
mean_diff() { # ppm a, ppm b -> fraction of sampled bytes that differ
  python3 -I - "$1" "$2" <<'PY'
import sys
a, b = (open(p, 'rb').read().split(b'\n', 3)[3] for p in sys.argv[1:3])
n = min(len(a), len(b)); st = max(1, n // 50000)
print(sum(a[i] != b[i] for i in range(0, n, st)) / len(range(0, n, st)))
PY
}

wl dump "$OUT/lock.ppm"
# 1. unlock: swipe up from the bottom -> keypad; guest PAM 'phosh' = pam_permit, so any 1-digit code + Unlock works.
#    (keypad auto-hides after a few idle seconds -> do it in one go)
for try in 1 2 3; do
  log "unlock attempt $try"
  wl swipe "$(X 0.5)" "$(Y 0.94)" "$(X 0.5)" "$(Y 0.19)" 400; sleep 1
  wl tap "$(X 0.38)" "$(Y 0.464)"; sleep 0.3          # digit '1'
  wl tap "$(X 0.5)" "$(Y 0.682)"; sleep 2.5           # Unlock button
  wl dump "$OUT/unlocked.ppm"
  d=$(mean_diff "$OUT/lock.ppm" "$OUT/unlocked.ppm"); log "diff vs lockscreen: $d"
  python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0.02 else 1)" "$d" && break
  [ "$try" = 3 ] && { log "unlock failed"; exit 1; }
  sleep 4
done
shot unlocked                                          # home overview (favorites app grid, empty)
# 2. full app grid: "Show All Apps" toggle under the search bar
wl tap "$(X 0.5)" "$(Y 0.0975)"; sleep 1.5
shot grid
# 3. tap the Foot icon (first grid cell)
pre=$(pgrep -x foot | wc -l || true)
wl tap "$(X 0.07)" "$(Y 0.091)"
for _ in $(seq 40); do [ "$(pgrep -x foot | wc -l)" -gt "$pre" ] && break; sleep 0.25; done
[ "$(pgrep -x foot | wc -l)" -gt "$pre" ] || { log "foot did not start after tap"; exit 1; }
log "foot started by tap"; sleep 2.5
# keyboard path proof: neutral prompt (hides host hostname), kernel line
wl type "PS1='lodos\$ '; clear; uname -srm; echo foot launched by injected tap, typed via lodos-wl ctl\n"; sleep 1
shot foot
log "screens: docs/screens/T042-phosh-{unlocked,grid,foot}.png"
