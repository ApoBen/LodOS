#!/system/bin/sh
# Runs ON the phone (shell uid). Dims to brightness 0 when: plugged + awake + no user input for 5 min + PC says work is active.
# Restores on any touch/key, unplug, screen off, or stale work flag.
B=/data/local/tmp/lodos; mkdir -p $B
echo $$ > $B/keeper.pid
IDLE=${IDLE:-60}
last=""; since=$(date +%s); dim=0
restore(){
  [ -f $B/saved ] || return
  read br mode < $B/saved
  settings put system screen_brightness_mode "$mode"
  settings put system screen_brightness "$br"
  rm -f $B/saved; dim=0; echo "$(date) restore" >> $B/keeper.log
}
trap 'restore; exit 0' INT TERM
restore   # in case a previous run died while dimmed
while true; do
  p=$(dumpsys power)
  ua=$(echo "$p" | sed -n 's/.*mLastUserActivityTime[^=]*=\([0-9]*\).*/\1/p' | head -1)
  awake=0; echo "$p" | grep -q 'mWakefulness=Awake' && awake=1
  plugged=0; echo "$p" | grep -q 'mIsPowered=true' && plugged=1
  now=$(date +%s)
  work=0; [ -f $B/work ] && [ $((now - $(stat -c %Y $B/work))) -lt 180 ] && work=1
  if [ "$ua" != "$last" ]; then last=$ua; since=$now; [ $dim = 1 ] && restore; fi
  if [ $dim = 1 ] && { [ $awake = 0 ] || [ $plugged = 0 ] || [ $work = 0 ]; }; then restore; fi
  if [ $dim = 0 ] && [ $awake = 1 ] && [ $plugged = 1 ] && [ $work = 1 ] && [ $((now - since)) -ge $IDLE ]; then
    echo "$(settings get system screen_brightness) $(settings get system screen_brightness_mode)" > $B/saved
    settings put system screen_brightness_mode 0
    settings put system screen_brightness 0
    dim=1; echo "$(date) dim" >> $B/keeper.log
    # brightness change itself may bump activity; resync
    sleep 2; last=$(dumpsys power | sed -n 's/.*mLastUserActivityTime[^=]*=\([0-9]*\).*/\1/p' | head -1)
  fi
  sleep 10
done
