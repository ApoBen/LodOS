set -e
cd /mnt/SSD/LodOS
source orch/env.sh
adb get-state >/dev/null 2>&1 || { echo "no phone"; exit 1; }
test -s scripts/phone-venus-spike.sh
timeout 20m bash scripts/phone-venus-spike.sh >/tmp/T055.run.log 2>&1 || { tail -30 /tmp/T055.run.log; echo "script failed (ok only if json says works=false)"; }
python3 -I - docs/research/S1-venus.json.json <<'PY'
import json,sys
j=json.load(open(sys.argv[1]))
for k in ("works","bridge_vkcube_fps","missing_exts","blockers","license","verdict"): assert k in j,k
assert j["verdict"] in ("adopt","reject","partial")
assert isinstance(j["bridge_vkcube_fps"],(int,float)) and j["bridge_vkcube_fps"]>0
if j["works"]: assert isinstance(j.get("vkcube_fps"),(int,float)) and j["vkcube_fps"]>0
else: assert j["blockers"],"works=false needs blockers"
print(j["verdict"],j.get("vkcube_fps"),j["bridge_vkcube_fps"])
PY
test -s docs/research/S1-venus.md
adb shell 'pgrep -f "vkcube|virgl|vtest"' >/dev/null 2>&1 && { echo "processes left on phone"; exit 1; }
echo PASS
