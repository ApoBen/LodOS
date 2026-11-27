set -e
cd /mnt/SSD/LodOS
d=docs/research/P2-compat-presets.md
[ "$(wc -l < $d)" -ge 60 ]
grep -qi 'license' $d; grep -qi 'winlator' $d; grep -qi 'gamenative' $d; grep -q 'BOX64_DYNAREC' $d
python3 -I - <<'P'
import json
j=json.load(open('data/compat/presets.json'))
p=j['presets']; ids=[x['id'] for x in p]
assert {'stable','balanced','performance'} <= set(ids), ids
assert len(p)>=5, len(p)
assert any('hades' in x['id'].lower() for x in p)
for x in p:
    for k in ('box64','wine','dxvk','source'): assert k in x,(x['id'],k)
print('presets',len(p))
P
echo PASS
