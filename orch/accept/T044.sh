set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*LiteMode*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 6 ]
test -s ../docs/research/lite-mode.md
echo PASS
