set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*GuestLaunchLite*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 5 ]
grep -rq 'Lite' app/src/main/java/org/lodos/app/guest/GuestLaunch.kt
grep -rqE 'LiteModeStore|LiteProfile' app/src/main/java/org/lodos/app/session/
echo PASS
