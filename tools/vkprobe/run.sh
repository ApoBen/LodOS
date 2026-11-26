#!/bin/sh
# Push vkprobe to phone, run, pull JSON to docs/research/mali-g720-vk.json
set -e
cd "$(dirname "$0")"
. ../../orch/env.sh
S=${ADB_SERIAL:-<phone-serial>}
adb -s $S shell mkdir -p /data/local/tmp/lodos
adb -s $S push vkprobe /data/local/tmp/lodos/vkprobe
adb -s $S shell "chmod 755 /data/local/tmp/lodos/vkprobe && /data/local/tmp/lodos/vkprobe /data/local/tmp/lodos/vkprobe.json"
adb -s $S pull /data/local/tmp/lodos/vkprobe.json ../../docs/research/mali-g720-vk.json
python3 -I -m json.tool ../../docs/research/mali-g720-vk.json >/dev/null && echo json ok
