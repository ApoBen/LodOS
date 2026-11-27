#!/bin/sh
# Guest-side launcher; the app binds its host socket at /run/lodos/audio.sock.
set -eu
mkdir -p /run/lodos/pulse
pulseaudio -n --daemonize=no --exit-idle-time=-1 --use-pid-file=no --realtime=no \
  --high-priority=no --disable-shm=yes --log-target=stderr --log-level=warn \
  -F /usr/share/lodos/pulse-lodos.pa &
PA_PID=$!
for i in $(seq 1 100); do
  [ -S /run/lodos/pulse/native.sock ] && [ -p /run/lodos/pulse/sink.fifo ] && break
  sleep 0.1
done
[ -S /run/lodos/pulse/native.sock ] && [ -p /run/lodos/pulse/sink.fifo ] || exit 1
if [ -n "${LODOS_AUDIO_RELAY:-}" ] && [ -x "$LODOS_AUDIO_RELAY" ]; then
  RELAY=$LODOS_AUDIO_RELAY
elif [ -x /usr/lib/lodos/lodos-audio-relay ]; then
  RELAY=/usr/lib/lodos/lodos-audio-relay
else
  RELAY=
fi
if [ -n "$RELAY" ]; then
  "$RELAY" --fifo /run/lodos/pulse/sink.fifo --sock /run/lodos/audio.sock &
else
  socat -u OPEN:/run/lodos/pulse/sink.fifo,rdonly UNIX-CONNECT:/run/lodos/audio.sock &
fi
RELAY_PID=$!
cleanup() {
  kill "$RELAY_PID" "$PA_PID" 2>/dev/null || true
  wait "$RELAY_PID" "$PA_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
wait "$PA_PID"
