set -e
cd /mnt/SSD/LodOS
source orch/env.sh
D=$(ls -d native/lodos-w?)
grep -q 'DisplayActivity_touch' $D/src/*.rs
grep -q 'DisplayActivity_key' $D/src/*.rs
(cd $D && cargo test --release input_map 2>&1 | tee /tmp/T066.cargo.log | tail -5)
n=$(grep -Eo 'test result: ok\. [0-9]+ passed' /tmp/T066.cargo.log | awk '{s+=$4} END{print s+0}')
[ "$n" -ge 6 ] || { echo "input_map tests $n<6"; exit 1; }
cd android
./gradlew -q testDebugUnitTest --tests '*TouchMapper*' assembleDebug
x=$(ls app/build/test-results/testDebugUnitTest/TEST-*TouchMapper*.xml)
t=$(grep -o 'tests="[0-9]*"' $x | head -1 | tr -dc 0-9); f=$(grep -o 'failures="[0-9]*"' $x | head -1 | tr -dc 0-9)
[ "$t" -ge 4 ] && [ "$f" -eq 0 ] || { echo "TouchMapper tests=$t failures=$f"; exit 1; }
echo PASS
