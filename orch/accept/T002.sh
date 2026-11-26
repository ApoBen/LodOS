set -e
source /mnt/SSD/LodOS/orch/env.sh
cd /mnt/SSD/LodOS/android
./gradlew -q assembleDebug
apk=app/build/outputs/apk/debug/app-debug.apk; test -f $apk
D="adb -s <phone-serial>"
$D install -r $apk
$D shell am force-stop org.lodos.app
$D logcat -c
$D shell am start -W -n org.lodos.app/.MainActivity
sleep 3
$D shell pidof org.lodos.app
! $D logcat -d -b crash | grep -q org.lodos.app
echo PASS
