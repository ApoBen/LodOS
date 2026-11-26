#!/usr/bin/env bash
# PC side: while lodos.service is active and phone is attached, keep keeper.sh running on phone and refresh the work flag.
D="adb -s <phone-serial>"; K=/mnt/SSD/LodOS/orch/phone/keeper.sh
while true; do
  if $D get-state 2>/dev/null | grep -q device; then
    lvl=$($D shell dumpsys battery 2>/dev/null | sed -n 's/^ *level: *\([0-9]*\).*/\1/p' | head -1)
    if [ -n "$lvl" ] && [ "$lvl" -le "${LOW_BATT:-15}" ] && systemctl --user is-active -q lodos.service; then
      R=/mnt/SSD/LodOS; L=$R/orch/logs/runner.log
      echo "$(date '+%F %T') PHONE BATTERY ${lvl}% -> lock phone, save, poweroff" >> $L
      $D shell 'rm -f /data/local/tmp/lodos/work; for p in $(pgrep -x keeper.sh); do kill $p; done' >/dev/null 2>&1
      sleep 12   # keeper restores brightness + closes black screen
      $D shell 'dumpsys power | grep -q mWakefulness=Awake && input keyevent 26' >/dev/null 2>&1
      systemctl --user stop lodos.service
      cd $R && bash orch/checklist.sh >/dev/null 2>&1
      git -C $R add -A >/dev/null 2>&1; git -C $R commit -qm "phone battery ${lvl}%: state saved before poweroff" >/dev/null 2>&1
      [ -f $R/orch/NO_SHUTDOWN ] || systemctl poweroff
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
