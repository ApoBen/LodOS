set -e
cd /mnt/SSD/LodOS/docs/research
python3 -I -c '
import json,sys
j=json.load(open("mali-g720-vk.json"))
s=json.dumps(j)
assert "Mali" in s, "no Mali deviceName"
assert "BC1" in s or "bc1" in s.lower(), "no BC format info"
assert "geometryShader" in s, "no features"
print("deviceName ok, keys:", list(j)[:12])
'
test -s mali-g720-vk.md
echo PASS
