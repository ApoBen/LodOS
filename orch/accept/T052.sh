set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*SessionUiModel*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 6 ]
grep -rq 'POST_NOTIFICATIONS' app/src/main/java/
grep -rlq 'GuestSessionService' app/src/main/java/org/lodos/app/ --include=*Activity*.kt
echo PASS
