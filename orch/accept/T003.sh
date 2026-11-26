set -e
f=/mnt/SSD/LodOS/docs/research/S1-gpu-bridge.md
test -f $f
for h in Sources Licenses Vortek Wrapper "Present path" Recommendation "Next tasks"; do
  grep -q "^## $h" $f || { echo "missing heading: $h"; exit 1; }
done
[ $(wc -l < $f) -ge 60 ] || { echo "too short"; exit 1; }
test -f /mnt/SSD/LodOS/THIRD_PARTY.md
echo PASS
