set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
grep -rlq 'BugReport' app/build/test-results/testDebugUnitTest/
grep -rqiE 'redact' app/src/test
grep -q 'FileProvider' app/src/main/AndroidManifest.xml
ls app/build/outputs/apk/debug/*.apk >/dev/null
echo PASS
