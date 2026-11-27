#!/usr/bin/env bash
# T049 S5 audio spike (PC): guest pulseaudio module-pipe-sink (FIFO) -> socat relay -> host unix socket
# -> native/lodos-audio (WavBackend) -> build/T049.wav. Prints `AUDIO ok bytes=N`.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[ -f orch/env.sh ] && source orch/env.sh
OUT=$ROOT/build/T049.wav
SOCKDIR=$ROOT/build/T049-audio
LOG=$ROOT/build/T049-audio.log
BIN=$ROOT/native/lodos-audio/target/release/lodos-audio
log() { echo "[pc-audio] $*" >&2; }

(cd native/lodos-audio && cargo build --release -q)

# guest pkgs (idempotent)
bash scripts/pc-guest.sh run /bin/sh -c '
missing=""
for p in pulseaudio pulseaudio-utils sox socat; do
  dpkg -s $p 2>/dev/null | grep -q "^Status: install ok installed" || missing="$missing $p"
done
[ -z "$missing" ] && exit 0
echo "installing:$missing"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q && apt-get install -y -q --no-install-recommends $missing && apt-get clean' >&2

rm -rf "$SOCKDIR"; mkdir -p "$SOCKDIR"; rm -f "$OUT"
HPID=
cleanup() {
  [ -n "$HPID" ] && kill "$HPID" 2>/dev/null && wait "$HPID" 2>/dev/null || true
  pkill -x pulseaudio 2>/dev/null || true
  rm -rf "$SOCKDIR"
}
trap cleanup EXIT

LODOS_AUDIO_SOCK=$SOCKDIR/audio.sock LODOS_AUDIO_BACKEND=wav LODOS_AUDIO_WAV=$OUT LODOS_AUDIO_WAV_TRIM=1 \
  "$BIN" 2>"$LOG" &
HPID=$!
for _ in $(seq 50); do [ -S "$SOCKDIR/audio.sock" ] && break; sleep 0.1; done
[ -S "$SOCKDIR/audio.sock" ] || { log "host socket missing"; cat "$LOG" >&2; exit 1; }

# guest: pulse (no default.pa) with pipe-sink paced by system clock; suspend-on-idle -> no traffic when idle
EXTRA_BINDS="-b $SOCKDIR:/run/lodos-audio" bash scripts/pc-guest.sh run /bin/bash -c '
set -e
rm -rf /tmp/lodos-pa; mkdir -p /tmp/lodos-pa
F=/tmp/lodos-pa/sink.fifo; S=/tmp/lodos-pa/native.sock
sox -q -n -r 48000 -c 2 -b 16 /tmp/lodos-pa/sine.wav synth 2 sine 440 vol 0.5
pulseaudio -n --daemonize=no --exit-idle-time=-1 --use-pid-file=no --realtime=no --high-priority=no \
  --disable-shm=yes --log-target=stderr --log-level=warn \
  -L "module-pipe-sink file=$F sink_name=lodos format=s16le rate=48000 channels=2 use_system_clock_for_timing=yes" \
  -L "module-native-protocol-unix socket=$S auth-anonymous=1" \
  -L "module-suspend-on-idle timeout=1" 2>/tmp/lodos-pa/pa.log &
PA=$!
for i in $(seq 100); do [ -S $S ] && [ -p $F ] && break; sleep 0.1; done
[ -S $S ] || { echo "pulse failed"; cat /tmp/lodos-pa/pa.log; exit 1; }
socat -u OPEN:$F,rdonly UNIX-CONNECT:/run/lodos-audio/audio.sock &
RELAY=$!
sleep 0.5
paplay -s unix:$S -d lodos /tmp/lodos-pa/sine.wav
sleep 0.5
kill $RELAY $PA 2>/dev/null || true
wait $PA 2>/dev/null || true
grep -iE "error|fail" /tmp/lodos-pa/pa.log | head -5 >&2 || true
echo guest-done' >&2

sleep 0.3
kill "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null || true; HPID=
BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
[ "$BYTES" -gt 192000 ] || { log "wav too small ($BYTES)"; cat "$LOG" >&2; exit 1; }
echo "AUDIO ok bytes=$BYTES"
