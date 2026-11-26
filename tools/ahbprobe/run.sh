#!/bin/sh
# Push ahbprobe to phone, run as shell uid, pull JSON to docs/research/ahb-dmabuf.json
set -e
cd "$(dirname "$0")"
. ../../orch/env.sh
S=${ADB_SERIAL:-<phone-serial>}
adb -s $S shell mkdir -p /data/local/tmp/lodos
adb -s $S push ahbprobe /data/local/tmp/lodos/ahbprobe
adb -s $S shell "chmod 755 /data/local/tmp/lodos/ahbprobe && /data/local/tmp/lodos/ahbprobe /data/local/tmp/lodos/ahbprobe.json"
adb -s $S pull /data/local/tmp/lodos/ahbprobe.json ../../docs/research/ahb-dmabuf.json
python3 -I -m json.tool ../../docs/research/ahb-dmabuf.json >/dev/null && echo json ok
