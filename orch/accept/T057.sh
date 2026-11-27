set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*TerminalLaunchSpec*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 6 ]
[ "$(grep -c 'android.intent.category.LAUNCHER' app/src/main/AndroidManifest.xml)" -ge 2 ]
grep -qi 'terminal' app/src/main/AndroidManifest.xml
grep -qi 'termux' ../THIRD_PARTY.md
cd ..
adb get-state >/dev/null 2>&1 || { echo "no phone"; exit 1; }
rm -f docs/screens/T057-terminal.png
timeout 15m bash scripts/phone-terminal-smoke.sh >/tmp/T057.run.log 2>&1 || { tail -30 /tmp/T057.run.log; exit 1; }
test -s docs/screens/T057-terminal.png
python3 -I -c "import sys;d=open(sys.argv[1],'rb').read();assert d[:8]==b'\x89PNG\r\n\x1a\n' and len(d)>20000,len(d)" docs/screens/T057-terminal.png
echo PASS
