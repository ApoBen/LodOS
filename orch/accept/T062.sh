set -e
f=/mnt/SSD/LodOS/docs/research/S4-wine.md
for s in '## Options' '## DXVK' '## Proot' '## Recommendation' '## Sources'; do grep -q "^$s" $f; done
[ $(grep -c 'http' $f) -ge 5 ]
grep -qi 'arm64ec' $f; grep -qi 'license' $f
[ $(wc -l < $f) -ge 60 ]
echo PASS
