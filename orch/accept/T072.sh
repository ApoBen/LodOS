set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
ls app/src/main/java/org/lodos/app/onboarding/OnboardingChecks.kt app/src/main/java/org/lodos/app/onboarding/OnboardingActivity.kt >/dev/null
grep -q 'OnboardingActivity' app/src/main/AndroidManifest.xml
grep -q 'REQUEST_IGNORE_BATTERY_OPTIMIZATIONS' app/src/main/AndroidManifest.xml
ls app/src/main/res/values-tr/strings.xml >/dev/null
./gradlew -q -Pkotlin.compiler.execution.strategy=in-process testDebugUnitTest --tests '*Onboarding*' assembleDebug 2>&1 | tail -20
n=$(grep -ho '<testcase' app/build/test-results/testDebugUnitTest/TEST-*Onboarding*.xml | wc -l)
echo "onboarding tests=$n"; [ "$n" -ge 6 ]
! grep -l '<failure' app/build/test-results/testDebugUnitTest/TEST-*.xml
grep -qi 'onboarding' ../docs/research/P1-setup.md
echo PASS
