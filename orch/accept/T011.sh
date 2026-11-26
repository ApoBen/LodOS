set -e
cd /mnt/SSD/LodOS
bash scripts/guest-setup.sh
out=$(bash scripts/phone-run.sh 'dpkg -s dbus | grep -m1 Status; ps -o pid= -p 1 >/dev/null && echo ps-ok; python3 -c "import multiprocessing as m; l=m.Lock(); l.acquire(); print(\"ipc ok\")"; dbus-uuidgen | wc -c')
echo "$out"
echo "$out" | grep -q 'Status: install ok installed'
echo "$out" | grep -qx ps-ok
echo "$out" | grep -qx 'ipc ok'
echo "$out" | grep -qx 33
echo PASS
