set -e
cd /mnt/SSD/LodOS
out=$(timeout 20m bash native/vk-bridge/test/host_triangle.sh 2>&1) || { echo "$out" | tail -30; exit 1; }
echo "$out" | tail -10
echo "$out" | grep -Eq 'VKB host triangle ok center=[0-9a-fx,()]+ corner=[0-9a-fx,()]+ calls=[0-9]+'
l=$(echo "$out" | grep -Eo 'VKB batch calls batched=[0-9]+ unbatched=[0-9]+' | tail -1)
[ -n "$l" ] || { echo "no batch line"; exit 1; }
a=$(echo "$l" | sed -E 's/.*batched=([0-9]+).*/\1/'); b=$(echo "$l" | sed -E 's/.*unbatched=([0-9]+)/\1/')
echo "batched=$a unbatched=$b"; [ "$a" -lt "$b" ]
out2=$(VKB_NO_BATCH=0 timeout 20m bash native/vk-bridge/test/host_compute.sh 2>&1) || { echo "$out2" | tail -20; exit 1; }
echo "$out2" | grep -Eq 'VKB host compute ok'
grep -qi 'batching' native/vk-bridge/NOTES.md
echo PASS
