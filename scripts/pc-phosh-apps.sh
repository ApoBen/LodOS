#!/usr/bin/env bash
# T050 app smoke driver, called by `pc-desktop.sh apps` once phosh (init-phosh boot) is up. args: ctl W H outdir run
# Input via lodos-wl ctl (tools/wlctl.py). App window state via guest `wlrctl toplevel` (wlr-foreign-toplevel on
# phoc's wayland-0): lodos-wl only sees phoc's single nested-output toplevel (ctl `toplevels` == 1), not app windows.
# Per app: open grid ("Show All Apps"), type the app name into the grid search, Enter (launches 1st match),
# poll app_id mapped (<=15s), dump PNG, `wlrctl toplevel close app_id:X`, poll process gone, print `APP <name> ok`.
set -euo pipefail
CTL=$1; W=$2; H=$3; OUT=$4; RUN=$5
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
log() { echo "[phosh-apps] $*" >&2; }
wl() { python3 -I tools/wlctl.py "$CTL" "$@"; }
px() { python3 -I -c "import sys; print(round(float(sys.argv[1]) * float(sys.argv[2])))" "$1" "$2"; }
X() { px "$1" "$W"; }; Y() { px "$1" "$H"; }
# guest wlrctl against phoc (wayland-0 lives in $RUN = guest /run/host-xdg)
gwlr() { XDG_RUNTIME_DIR=$RUN bash scripts/pc-guest.sh run /usr/bin/env WAYLAND_DISPLAY=wayland-0 wlrctl "$@" 2>/dev/null </dev/null; }
app_ids() { gwlr toplevel list | sed -n 's/^\([^:]*\): .*/\1/p' | sort -u | tr '\n' ' '; }
mean_diff() { # ppm a, ppm b -> fraction of sampled bytes that differ
  python3 -I - "$1" "$2" <<'PY'
import sys
a, b = (open(p, 'rb').read().split(b'\n', 3)[3] for p in sys.argv[1:3])
n = min(len(a), len(b)); st = max(1, n // 50000)
print(sum(a[i] != b[i] for i in range(0, n, st)) / len(range(0, n, st)))
PY
}

log "lodos-wl mapped toplevels: $(wl toplevels) (phoc)"
wl dump "$OUT/lock.ppm"
# unlock (T042): swipe up -> keypad, guest PAM phosh = pam_permit -> any digit + Unlock
for try in 1 2 3; do
  log "unlock attempt $try"
  wl swipe "$(X 0.5)" "$(Y 0.94)" "$(X 0.5)" "$(Y 0.19)" 400; sleep 1
  wl tap "$(X 0.38)" "$(Y 0.464)"; sleep 0.3
  wl tap "$(X 0.5)" "$(Y 0.682)"; sleep 2.5
  wl dump "$OUT/unlocked.ppm"
  d=$(mean_diff "$OUT/lock.ppm" "$OUT/unlocked.ppm"); log "diff vs lockscreen: $d"
  python3 -I -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0.02 else 1)" "$d" && break
  [ "$try" = 3 ] && { log "unlock failed"; exit 1; }
  sleep 4
done
if [ "${PC_DESKTOP_HOLD:-0}" = 1 ]; then log "HOLD: ctl=$CTL run=$RUN"; while :; do sleep 5; done; fi

# overview (scale 1, 720x1600 measured): search entry y=0.045, toggle under grid y=0.154, 1st grid cell (0.07,0.091).
# Fresh boot = "mobile friendly only" filter (hides foot) -> switch to all apps once; search clears after each launch.
wl tap "$(X 0.5)" "$(Y 0.154)"; sleep 1.5
# name | search term | app_id | comm (15 chars) | text typed into the app (makes the 3 screens distinct)
APPS="foot|foot|foot|foot|PS1='lodos\$ '; clear; uname -srm; echo T050: foot launched from phosh grid\n
gnome-text-editor|text editor|org.gnome.TextEditor|gnome-text-edit|LodOS T050: gnome-text-editor on phoc nested in lodos-wl\n
gnome-calculator|calculator|org.gnome.Calculator|gnome-calculato|6*7\n"
mapped() { case " $(app_ids) " in *" $1 "*) return 0;; *) return 1;; esac; }
while IFS='|' read -r name term aid comm text; do
  mapped "$aid" && { log "$name already open"; exit 1; }
  pre=$(pgrep -x "$comm" | tr '\n' ' ' || true)
  wl tap "$(X 0.5)" "$(Y 0.045)"; sleep 1
  wl type "$term"; sleep 1.5
  wl tap "$(X 0.07)" "$(Y 0.091)"; t0=$SECONDS
  until mapped "$aid"; do
    [ $((SECONDS - t0)) -lt 15 ] || { log "$name: $aid not mapped after 15s"; exit 1; }
    sleep 0.5
  done
  pids=; for p in $(pgrep -x "$comm" || true); do case " $pre " in *" $p "*) ;; *) pids+="$p ";; esac; done
  log "$name mapped after $((SECONDS - t0))s (pids: ${pids:-?}; lodos-wl toplevels: $(wl toplevels))"; sleep 2.5
  wl type "$text"; sleep 1.5
  wl dump "$OUT/$name.ppm"; python3 -I scripts/ppm2png.py "$OUT/$name.ppm" "docs/screens/T050-$name.png" >&2
  gwlr toplevel close "app_id:$aid" || true; t0=$SECONDS; how=xdg-close
  while mapped "$aid" || { [ -n "$pids" ] && kill -0 $pids 2>/dev/null; }; do
    if [ $((SECONDS - t0)) -ge 6 ] && [ "$how" = xdg-close ]; then
      how=pkill; log "$name: still open after xdg close -> SIGTERM ${pids:-<none>}"; [ -n "$pids" ] && kill -TERM $pids 2>/dev/null || true
    fi
    [ $((SECONDS - t0)) -lt 12 ] || { log "$name: still open 12s after close"; exit 1; }
    sleep 0.5
  done
  log "$name closed ($how) after $((SECONDS - t0))s"
  echo "APP $name ok"
  sleep 1.5
done <<<"$APPS"
log "lodos-wl mapped toplevels: $(wl toplevels)"
