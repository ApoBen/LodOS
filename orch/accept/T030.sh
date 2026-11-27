set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
ls app/build/test-results/testDebugUnitTest/*.xml >/dev/null
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
grep -rq 'SetupState' app/src/test
grep -q 'SetupActivity' app/src/main/AndroidManifest.xml
ls app/build/outputs/apk/debug/*.apk
echo PASS
