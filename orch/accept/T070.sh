set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
ls app/src/main/java/org/lodos/app/settings/SettingsActivity.kt app/src/main/java/org/lodos/app/settings/SettingsEnv.kt >/dev/null
grep -q 'SettingsActivity' app/src/main/AndroidManifest.xml
./gradlew -q -Pkotlin.compiler.execution.strategy=in-process testDebugUnitTest --tests '*SettingsEnv*' --tests '*GuestLaunch*' assembleDebug 2>&1 | tail -20
n=$(grep -ho '<testcase' app/build/test-results/testDebugUnitTest/TEST-*SettingsEnv*.xml | wc -l)
echo "settings tests=$n"; [ "$n" -ge 5 ]
! grep -l '<failure' app/build/test-results/testDebugUnitTest/TEST-*.xml
echo PASS
