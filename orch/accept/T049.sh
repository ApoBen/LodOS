set -e
cd /mnt/SSD/LodOS
source orch/env.sh
(cd native/lodos-audio && timeout 10m cargo test 2>&1 | tail -5) | grep -q "test result: ok"
(cd native/lodos-audio && timeout 10m cargo ndk -t arm64-v8a check --features android 2>&1 | tail -3)
rm -f build/T049.wav
timeout 8m bash scripts/pc-audio-test.sh
python3 -I - build/T049.wav <<'PY'
import sys,wave,struct,math
w=wave.open(sys.argv[1]); assert w.getsampwidth()==2
rate=w.getframerate(); ch=w.getnchannels(); n=w.getnframes(); assert n>rate*1, n
s=struct.unpack('<%dh'%(n*ch), w.readframes(n))[::ch]
peak=max(abs(x) for x in s); assert peak>2000,'silent'
# zero-crossing freq estimate on loud part
seg=[x for x in s if True][rate//4:rate//4+rate]
zc=sum(1 for a,b in zip(seg,seg[1:]) if (a<0)!=(b<0))
f=zc/2; print('freq~',f,'rate',rate,'peak',peak); assert 380<f<500, f
PY
test -s docs/research/S5-audio.md
! pgrep -x 'lodos-audio|pulseaudio' >/dev/null
echo PASS
