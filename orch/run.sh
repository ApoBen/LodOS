#!/usr/bin/env bash
# LodOS autonomous runner. Zero-AI-cost loop: dispatch tasks to agy, verify, commit, ask Claude to review at checkpoints.
set -u
R=/mnt/SSD/LodOS; O=$R/orch; T=$O/tasks; S=$O/state; L=$O/logs
AGY=$HOME/.gemini/bin/agy
PHONE=<phone-serial>
REVIEW_EVERY=${REVIEW_EVERY:-2}
mkdir -p "$S" "$L"
exec 9>"$S/lock"; flock -n 9 || { echo "runner already running"; exit 0; }
cd "$R" || exit 1
[ -f "$O/env.sh" ] && source "$O/env.sh"

log(){ echo "$(date '+%F %T') $*" | tee -a "$L/runner.log"; }
field(){ sed -n "s/^$2: *//p" "$1" | head -1; }
setf(){ sed -i "0,/^$2:.*/s//$2: $3/" "$1"; }
phone_ok(){ adb -s "$PHONE" get-state 2>/dev/null | grep -q device; }
checklist(){ bash "$O/checklist.sh" >/dev/null 2>&1; }
commit(){ git add -A >/dev/null 2>&1; git commit -qm "$1" >/dev/null 2>&1 || true; }

deps_done(){ # $1 task file
  local d; for d in $(field "$1" deps | tr ',' ' '); do
    [ "$(field "$T/$d.md" status)" = done ] || return 1; done; return 0; }

next_task(){
  local f st
  for st in doing todo; do
    for f in $(ls "$T"/T*.md 2>/dev/null | sort); do
      [ "$(field "$f" status)" = "$st" ] || continue
      deps_done "$f" || continue
      if [ "$(field "$f" needs_phone)" = yes ] && ! phone_ok; then continue; fi
      echo "$f"; return 0
    done
  done
  return 1
}

# agy output -> SUCCESS | QUOTA | FAIL
agy_status(){ python3 -I - "$1" <<'PY'
import json,sys,re
t=open(sys.argv[1],errors='ignore').read()
try:
    j=json.loads(t[t.find('{'):]); st=j.get('status','')
except Exception: st=''
if st=='SUCCESS': print('SUCCESS'); sys.exit()
print('QUOTA' if re.search(r'quota|rate.?limit|resource.?exhausted|\b429\b|exceeded|out of credits',t,re.I) else 'FAIL')
PY
}

maybe_shutdown(){ # $1 reason
  log "SHUTDOWN-CHECK reason=$1"; checklist; commit "runner: $1"
  if [ -f "$O/NO_SHUTDOWN" ]; then log "NO_SHUTDOWN present -> exit"; exit 0; fi
  local a
  a=$(timeout 300 notify-send -u critical -a LodOS -A cancel="İptal et" --wait \
      "LodOS" "Sebep: $1. Bilgisayar 5 dakika içinde kapanacak." 2>/dev/null)
  if [ "$a" = cancel ]; then log "shutdown cancelled by user -> exit"; exit 0; fi
  log "POWEROFF"; systemctl poweroff; exit 0
}

review(){
  bash "$O/review.sh"; local rc=$?
  [ $rc -eq 0 ] && echo 0 > "$S/since_review"
  return $rc   # 0 ok, 2 claude limit, other fail
}

run_task(){
  local f=$1 id att max model acc out prompt st prev
  id=$(field "$f" id); att=$(field "$f" attempts); max=$(field "$f" max)
  model=$(field "$f" model); acc=$(field "$f" accept)
  att=$((att+1)); setf "$f" attempts "$att"; setf "$f" status doing; checklist
  [ "$att" -ge "$max" ] && model=gemini-4-carbon-high   # last attempt escalates
  prev=""; [ -f "$L/$id.accept.log" ] && prev="Previous attempt's accept-script output (tail):
$(tail -40 "$L/$id.accept.log")"
  prompt="You are a LodOS implementation agent. First read /mnt/SSD/LodOS/GEMINI.md (rules). Your task file: $f (attempt $att/$max). Do the task described in it. Acceptance is checked afterwards by: $acc
$prev"
  out="$L/$id.a$att.json"
  log "RUN $id a$att model=$model"
  timeout 3h "$AGY" -p "$prompt" --model "$model" --output-format json \
     --dangerously-skip-permissions --add-dir "$R" >"$out" 2>&1
  st=$(agy_status "$out"); log "AGY $id a$att -> $st"
  if [ "$st" = QUOTA ]; then setf "$f" attempts "$((att-1))"; maybe_shutdown "agy kotasi bitti"; fi
  if timeout 1h bash "$acc" >"$L/$id.accept.log" 2>&1; then
    setf "$f" status done; log "DONE $id"; commit "$id: done (a$att)"
  elif [ "$att" -ge "$max" ]; then
    setf "$f" status blocked; log "BLOCKED $id"; commit "$id: blocked"
  else
    setf "$f" status todo; log "FAIL $id a$att"; commit "$id: wip a$att"; return 0
  fi
  echo $(( $(cat "$S/since_review" 2>/dev/null || echo 0) + 1 )) > "$S/since_review"
}

log "runner start"
while true; do
  checklist
  if f=$(next_task); then
    run_task "$f"
    [ "$(cat "$S/since_review" 2>/dev/null || echo 0)" -ge "$REVIEW_EVERY" ] && review
    continue
  fi
  # nothing runnable: let Claude plan more work
  review; rc=$?
  next_task >/dev/null && continue
  if [ $rc -eq 2 ]; then maybe_shutdown "is yok, Claude limiti dolu"; fi
  maybe_shutdown "kuyrukta calisabilir gorev yok"
done
