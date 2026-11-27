set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*StorageAccess*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 5 ]
g=$(ls app/build/test-results/testDebugUnitTest/*GuestLaunch*.xml | head -1)
n=$(grep -o 'tests="[0-9]*"' $g | head -1 | tr -dc 0-9); [ "$n" -ge 14 ]
grep -q 'MANAGE_EXTERNAL_STORAGE' app/src/main/AndroidManifest.xml
grep -rq 'sdcard' app/src/main/java/org/lodos/app/session
echo PASS
