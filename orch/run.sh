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
commit(){ ( flock 8; git add -A >/dev/null 2>&1; git commit -qm "$1" >/dev/null 2>&1 || true ) 8>"$S/git.lock"; }

deps_done(){ # $1 task file
  local d; for d in $(field "$1" deps | tr ',' ' '); do
    [ "$(field "$T/$d.md" status)" = done ] || return 1; done; return 0; }

LANES=${LANES:-3}   # parallel task lanes; phone/gradle/pc-display tasks never overlap
RUN_D=$S/running
# resources a task needs exclusively (from needs_phone + its accept script)
task_res(){ local f=$1 a; a=$(field "$f" accept)
  [ "$(field "$f" needs_phone)" = yes ] && echo phone
  grep -qs 'adb' "$R/$a" && echo phone
  grep -qs 'gradlew' "$R/$a" && echo gradle
  grep -qsE 'lodos-wl|phoc|phosh|pc-desktop|pc-guest|wayland' "$R/$a" "$f" && echo pcdisplay
  true; }
res_busy(){ local r; for r in $(task_res "$1" | sort -u); do grep -qsx "$r" "$RUN_D"/* && return 0; done; return 1; }

next_task(){
  local f st id
  for st in doing todo; do
    for f in $(ls "$T"/T*.md 2>/dev/null | sort); do
      [ "$(field "$f" status)" = "$st" ] || continue
      id=$(basename "$f" .md); [ -e "$RUN_D/$id" ] && continue
      deps_done "$f" || continue
      if [ "$(field "$f" needs_phone)" = yes ] && ! phone_ok; then continue; fi
      res_busy "$f" && continue
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

AGY_COOLDOWN=${AGY_COOLDOWN:-10800}
CODEX_COOLDOWN=${CODEX_COOLDOWN:-18000}
CODEX=$HOME/.local/bin/codex
# touch orch/AGY_OFF / orch/CODEX_OFF to disable an implementer manually
agy_limited(){ [ -f "$O/AGY_OFF" ] || { [ -f "$S/agy_limited_until" ] && [ "$(date +%s)" -lt "$(cat "$S/agy_limited_until")" ]; }; }
codex_limited(){ [ -f "$O/CODEX_OFF" ] || [ ! -x "$CODEX" ] || { [ -f "$S/codex_limited_until" ] && [ "$(date +%s)" -lt "$(cat "$S/codex_limited_until")" ]; }; }
GROK_COOLDOWN=${GROK_COOLDOWN:-18000}
GROK=$HOME/.local/bin/grok
grok_limited(){ [ -f "$O/GROK_OFF" ] || [ ! -x "$GROK" ] || { [ -f "$S/grok_limited_until" ] && [ "$(date +%s)" -lt "$(cat "$S/grok_limited_until")" ]; }; }
codex_quota(){ grep -qiE "usage limit|rate.?limit|quota|too many requests|\b429\b|purchase more credits|try again (at|in)" "$1"; }
claude_limited(){ grep -qiE 'usage limit|limit reached|rate.?limit|out of (extra )?usage|resets? (at|in)' "$1" \
  && grep -qE '"is_error": *true' "$1"; }

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
  if agy_limited && ! codex_limited; then
    log "RUN $id a$att implementer=codex (agy off/cooldown)"
    timeout 3h "$CODEX" exec -s workspace-write --skip-git-repo-check -C "$R" \
       -c sandbox_workspace_write.network_access=true \
       -c "sandbox_workspace_write.writable_roots=[\"$HOME/.cargo\",\"$HOME/.gradle\",\"$HOME/.cache\",\"/tmp\"]" \
       "IMPLEMENTER MODE (runner): you implement this task yourself. $prompt" </dev/null >"$out" 2>&1
    local crc=$?
    if [ $crc -ne 0 ] && codex_quota "$out"; then
      setf "$f" attempts "$((att-1))"; setf "$f" status todo
      echo $(( $(date +%s) + CODEX_COOLDOWN )) > "$S/codex_limited_until"
      log "codex quota (rc=$crc) -> next implementer for ${CODEX_COOLDOWN}s"; return 0
    fi
    log "CODEX $id a$att finished rc=$crc"
  elif agy_limited && ! grok_limited; then
    log "RUN $id a$att implementer=grok (agy+codex out)"
    GROK_SANDBOX_AUTO_ALLOW_BASH=1 timeout 3h "$GROK" -p "IMPLEMENTER MODE (runner): you implement this task yourself. $prompt" \
       --cwd "$R" --sandbox lodos --permission-mode acceptEdits --output-format json </dev/null >"$out" 2>&1
    local grc=$?
    if [ $grc -ne 0 ] && codex_quota "$out"; then
      setf "$f" attempts "$((att-1))"; setf "$f" status todo
      echo $(( $(date +%s) + GROK_COOLDOWN )) > "$S/grok_limited_until"
      log "grok quota (rc=$grc) -> next implementer for ${GROK_COOLDOWN}s"; return 0
    fi
    log "GROK $id a$att finished rc=$grc"
  elif agy_limited; then
    log "RUN $id a$att implementer=claude (agy+codex+grok out)"
    timeout 3h claude -p "IMPLEMENTER MODE (runner): agy quota is out, you implement this task yourself. $prompt" \
       --output-format json --dangerously-skip-permissions >"$out" 2>&1
    if claude_limited "$out"; then
      setf "$f" attempts "$((att-1))"; setf "$f" status todo
      local left=99999; [ -f "$O/AGY_OFF" ] || left=$(( $(cat "$S/agy_limited_until" 2>/dev/null || echo 0) - $(date +%s) ))
      [ -f "$S/codex_limited_until" ] && [ ! -f "$O/CODEX_OFF" ] && { local cl=$(( $(cat "$S/codex_limited_until") - $(date +%s) )); [ $cl -lt $left ] && left=$cl; }
      [ -f "$S/grok_limited_until" ] && [ ! -f "$O/GROK_OFF" ] && { local gl=$(( $(cat "$S/grok_limited_until") - $(date +%s) )); [ $gl -lt $left ] && left=$gl; }
      if [ "$left" -le 5400 ]; then log "claude limit; waiting ${left}s for agy"; sleep "$left"; return 0; fi
      maybe_shutdown "Claude limiti doldu (agy+codex+grok de bitti)"
    fi
    log "CLAUDE $id a$att finished"
  else
    log "RUN $id a$att model=$model"
    timeout 3h "$AGY" -p "$prompt" --model "$model" --output-format json \
       --dangerously-skip-permissions --add-dir "$R" >"$out" 2>&1
    st=$(agy_status "$out"); log "AGY $id a$att -> $st"
    if [ "$st" = QUOTA ]; then
      setf "$f" attempts "$((att-1))"; setf "$f" status todo
      echo $(( $(date +%s) + AGY_COOLDOWN )) > "$S/agy_limited_until"
      log "agy quota -> claude takes over for ${AGY_COOLDOWN}s"; return 0
    fi
  fi
  if LC_ALL=C timeout 1h bash "$acc" >"$L/$id.accept.log" 2>&1; then
    setf "$f" status done; log "DONE $id"; commit "$id: done (a$att)"
  elif [ "$att" -ge "$max" ]; then
    setf "$f" status blocked; log "BLOCKED $id"; commit "$id: blocked"
  else
    setf "$f" status todo; log "FAIL $id a$att"; commit "$id: wip a$att"; return 0
  fi
  ( flock 8; echo $(( $(cat "$S/since_review" 2>/dev/null || echo 0) + 1 )) > "$S/since_review" ) 8>"$S/count.lock"
}

log "runner start (lanes=$LANES)"
rm -rf "$RUN_D"; mkdir -p "$RUN_D"
declare -A PIDS
lane(){ # run one task in background; resource claim file removed on exit
  local f=$1 id; id=$(basename "$f" .md)
  task_res "$f" | sort -u > "$RUN_D/$id"
  ( run_task "$f"; rm -f "$RUN_D/$id" ) &
  PIDS[$id]=$!; log "LANE start $id pid=$! res=$(tr '\n' ',' < "$RUN_D/$id")"
}
reap(){ local id; for id in "${!PIDS[@]}"; do
  kill -0 "${PIDS[$id]}" 2>/dev/null || { wait "${PIDS[$id]}" 2>/dev/null; unset "PIDS[$id]"; rm -f "$RUN_D/$id"; }; done; }
while true; do
  reap; checklist
  started=0
  while [ "${#PIDS[@]}" -lt "$LANES" ] && f=$(next_task); do lane "$f"; started=1; sleep 5; done
  if [ "$(cat "$S/since_review" 2>/dev/null || echo 0)" -ge "$REVIEW_EVERY" ] && [ "$(date +%s)" -ge "${NEXT_REVIEW_TRY:-0}" ]; then
    NEXT_REVIEW_TRY=$(( $(date +%s) + 900 ))   # failed/limited review retried at most every 15 min
    review; [ $? -eq 2 ] && agy_limited && codex_limited && grok_limited && [ "${#PIDS[@]}" -eq 0 ] && maybe_shutdown "Claude limiti doldu (agy+codex+grok de bitti)"
    continue
  fi
  if [ "${#PIDS[@]}" -gt 0 ] || [ $started = 1 ]; then sleep 30; continue; fi
  # nothing running and nothing runnable: let Claude plan more work
  review; rc=$?
  next_task >/dev/null && continue
  if [ $rc -eq 2 ]; then maybe_shutdown "is yok, Claude limiti dolu"; fi
  maybe_shutdown "kuyrukta calisabilir gorev yok"
done
