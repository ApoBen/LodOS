#!/usr/bin/env bash
# PC side: while lodos.service is active and phone is attached, keep keeper.sh running on phone and refresh the work flag.
D="adb -s <phone-serial>"; K=/mnt/SSD/LodOS/orch/phone/keeper.sh
quota_notify(){ # $1=name $2=state file $3=OFF file
  local f=/mnt/SSD/LodOS/orch/state/$2 n=/mnt/SSD/LodOS/orch/state/$2.notified
  if [ -f "$f" ] && [ ! -f "/mnt/SSD/LodOS/orch/$3" ] && [ "$(cat "$f")" != "$(cat "$n" 2>/dev/null)" ] && [ "$(date +%s)" -lt "$(cat "$f")" ]; then
    cat "$f" > "$n"
    notify-send -u critical -a LodOS "LodOS: $1 kotası bitti" "İş sıradaki uygulayıcıya geçti. Başka hesapla $1'e giriş yapınca: rm /mnt/SSD/LodOS/orch/state/$2" 2>/dev/null
    echo "$(date '+%F %T') NOTIFY $1 quota -> user" >> /mnt/SSD/LodOS/orch/logs/runner.log
  fi
}
while true; do
  quota_notify agy agy_limited_until AGY_OFF
  quota_notify codex codex_limited_until CODEX_OFF
  if $D get-state 2>/dev/null | grep -q device; then
    lvl=$($D shell dumpsys battery 2>/dev/null | sed -n 's/^ *level: *\([0-9]*\).*/\1/p' | head -1)
    bst=$($D shell dumpsys battery 2>/dev/null | sed -n 's/^ *status: *\([0-9]*\).*/\1/p' | head -1)
    # status 2=charging 5=full: on charger only act if critically low (still draining under load)
    lim=${LOW_BATT:-15}; { [ "$bst" = 2 ] || [ "$bst" = 5 ]; } && lim=${LOW_BATT_CHG:-5}
    if [ -n "$lvl" ] && [ "$lvl" -le "$lim" ] && systemctl --user is-active -q lodos.service; then
      R=/mnt/SSD/LodOS; L=$R/orch/logs/runner.log
      echo "$(date '+%F %T') PHONE BATTERY ${lvl}% -> lock phone, save, poweroff" >> $L
      $D shell 'rm -f /data/local/tmp/lodos/work; for p in $(pgrep -x keeper.sh); do kill $p; done' >/dev/null 2>&1
      sleep 12   # keeper restores brightness + closes black screen
      $D shell 'dumpsys power | grep -q mWakefulness=Awake && input keyevent 26' >/dev/null 2>&1
      systemctl --user stop lodos.service
      cd $R && bash orch/checklist.sh >/dev/null 2>&1
      git -C $R add -A >/dev/null 2>&1; git -C $R commit -qm "phone battery ${lvl}%: state saved before poweroff" >/dev/null 2>&1
      if [ ! -f $R/orch/NO_SHUTDOWN ]; then
        a=$(timeout 120 notify-send -u critical -w -A cancel=Iptal "LodOS" "Telefon pili %${lvl} (status ${bst}). 2 dk sonra PC kapanacak." 2>/dev/null)
        [ "$a" = cancel ] && { echo "$(date '+%F %T') poweroff cancelled by user" >> $L; exit 0; }
        systemctl poweroff
      fi
      exit 0
    fi
    if systemctl --user is-active -q lodos.service; then
      $D shell 'mkdir -p /data/local/tmp/lodos && touch /data/local/tmp/lodos/work' >/dev/null 2>&1
      if ! $D shell 'p=$(cat /data/local/tmp/lodos/keeper.pid 2>/dev/null); [ -n "$p" ] && kill -0 $p 2>/dev/null' ; then
        $D push "$K" /data/local/tmp/lodos/keeper.sh >/dev/null 2>&1
        $D shell 'chmod 755 /data/local/tmp/lodos/keeper.sh; setsid nohup /data/local/tmp/lodos/keeper.sh >/dev/null 2>&1 < /dev/null &' >/dev/null 2>&1
      fi
    else
      $D shell 'rm -f /data/local/tmp/lodos/work' >/dev/null 2>&1   # keeper restores brightness itself
    fi
  fi
  sleep 60
done
