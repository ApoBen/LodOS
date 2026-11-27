set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
ls app/src/main/java/org/lodos/app/session/DesktopLaunchPlan.kt >/dev/null
grep -rq 'client=session\|"session"' app/src/main/java/org/lodos/app/DisplayActivity.kt
./gradlew -q -Pkotlin.compiler.execution.strategy=in-process testDebugUnitTest assembleDebug 2>&1 | tail -20
n=$(grep -ho '<testcase' app/build/test-results/testDebugUnitTest/TEST-*DesktopLaunchPlan*.xml | wc -l)
echo "plan tests=$n"; [ "$n" -ge 6 ]
! grep -l '<failure' app/build/test-results/testDebugUnitTest/TEST-*.xml
grep -qi 'start desktop' ../docs/research/P1-setup.md
echo PASS
