set -e
cd /mnt/SSD/LodOS/android
source ../orch/env.sh
timeout 25m ./gradlew -q --no-daemon testDebugUnitTest assembleDebug 2>&1 | tail -20
! grep -l '<failure' app/build/test-results/testDebugUnitTest/*.xml
f=$(ls app/build/test-results/testDebugUnitTest/*SessionController*.xml)
n=$(grep -o 'tests="[0-9]*"' $f | head -1 | tr -dc 0-9); [ "$n" -ge 10 ]
grep -q 'GuestSessionService' app/src/main/AndroidManifest.xml
grep -q 'foregroundServiceType' app/src/main/AndroidManifest.xml
grep -rq 'lodos-init' app/src/main/java/org/lodos/app/session/
grep -rq 'guest-' app/src/main/java/org/lodos/app/bugreport/
test -s ../docs/research/guest-session.md
echo PASS
