#!/usr/bin/env bash
# PC side: while lodos.service is active and phone is attached, keep keeper.sh running on phone and refresh the work flag.
D="adb -s <phone-serial>"; K=/mnt/SSD/LodOS/orch/phone/keeper.sh
while true; do
  if $D get-state 2>/dev/null | grep -q device; then
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
