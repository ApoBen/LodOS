set -e
cd /mnt/SSD/LodOS
bash tools/ahbprobe/build.sh
bash tools/ahbprobe/run.sh
cd docs/research
python3 -I -c '
import json
j=json.load(open("ahb-dmabuf.json"))
for k in ["ahb_alloc_ok","ahb_fd","dma_heap","vk_ahb_import","vk_ext_dma_buf","vk_memory_fd"]:
    assert k in j, "missing key "+k
assert j["ahb_alloc_ok"] is True, "AHB alloc failed"
print({k:j[k] for k in ["ahb_alloc_ok","vk_ahb_import","vk_ext_dma_buf"]})
'
test -s ahb-dmabuf.md
echo PASS
