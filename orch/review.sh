#!/usr/bin/env bash
# Claude checkpoint review. exit 0=ok, 2=claude usage limit (cooldown), 1=fail
R=/mnt/SSD/LodOS; O=$R/orch; S=$O/state; L=$O/logs
cd "$R" || exit 1
now=$(date +%s)
if [ -f "$S/claude_limited_until" ] && [ "$now" -lt "$(cat "$S/claude_limited_until")" ]; then
  echo "$(date '+%F %T') REVIEW skipped (claude cooldown)" >> "$L/runner.log"; exit 2; fi
out="$L/review-$(date +%Y%m%d-%H%M%S).json"
echo "$(date '+%F %T') REVIEW start" >> "$L/runner.log"
timeout 45m claude -p "$(cat "$O/review_prompt.md")" --output-format json \
  --dangerously-skip-permissions >"$out" 2>&1
if grep -qiE 'usage limit|limit reached|rate.?limit|out of (extra )?usage|resets? (at|in)' "$out" \
   && grep -q '"is_error": *true\|"is_error":true' "$out"; then
  echo $((now+3600)) > "$S/claude_limited_until"
  echo "$(date '+%F %T') REVIEW claude limit -> cooldown 1h" >> "$L/runner.log"; exit 2
fi
if grep -q '"is_error": *false\|"is_error":false' "$out"; then
  git add -A >/dev/null 2>&1; git commit -qm "review: claude checkpoint" >/dev/null 2>&1
  echo "$(date '+%F %T') REVIEW ok" >> "$L/runner.log"; exit 0; fi
echo "$(date '+%F %T') REVIEW failed (see $out)" >> "$L/runner.log"; exit 1
