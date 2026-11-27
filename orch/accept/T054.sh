set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*AudioBridge*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 4 ]
apk=$(ls app/build/outputs/apk/debug/*.apk | head -1)
unzip -l "$apk" | grep -q 'lib/arm64-v8a/liblodos_audio.so'
grep -rq 'AudioBridge' app/src/main/java/org/lodos/app/session/GuestSessionService.kt
test -f ../scripts/guest/pulse-lodos.pa
echo PASS
