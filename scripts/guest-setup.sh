#!/usr/bin/env bash
# Idempotent guest (Debian/proot) setup: apt/dpkg fixes for proot on Android + base packages.
# usage: scripts/guest-setup.sh   (needs scripts/phone-rootfs.sh done; PKGS env overrides package list)
# libpython3-stdlib: python3-minimal lacks multiprocessing (needed for ipc smoke)
set -euo pipefail
cd "$(dirname "$0")/.."
PKGS=${PKGS:-"dbus procps python3-minimal libpython3-stdlib"}
R="bash scripts/phone-run.sh"

t0=$(date +%s)
# 1) config fixes (all idempotent)
$R '
set -e
# apt: _apt sandbox user needs setgroups/seteuid -> unreliable under proot; run as (fake) root
mkdir -p /etc/apt/apt.conf.d /etc/dpkg/dpkg.cfg.d
printf "%s\n" "APT::Sandbox::User \"root\";" "Acquire::Retries \"3\";" "APT::Install-Recommends \"false\";" > /etc/apt/apt.conf.d/01lodos
# dpkg: fsync is slow on f2fs/fuse and pointless here
printf "%s\n" force-unsafe-io > /etc/dpkg/dpkg.cfg.d/01lodos
# android paranoid-network groups (supplementary gids leak into guest)
for g in aid_inet:3003 aid_net_raw:3004 aid_net_admin:3005 aid_net_bw_stats:3006 aid_net_bw_acct:3007 aid_everybody:9997; do
  n=${g%%:*}; id=${g##*:}
  grep -q "^$n:" /etc/group || echo "$n:x:$id:root" >> /etc/group
done
# DNS (keep existing if non-empty regular file)
if [ -L /etc/resolv.conf ] || ! grep -q "^nameserver" /etc/resolv.conf 2>/dev/null; then
  rm -f /etc/resolv.conf; printf "nameserver 8.8.8.8\nnameserver 1.1.1.1\n" > /etc/resolv.conf
fi
# no init system: keep maintainer scripts from trying to start services
printf "#!/bin/sh\nexit 101\n" > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d
echo cfg-ok
'
t1=$(date +%s)

# 2) packages (skip apt entirely if all already installed)
$R "
set -e
missing=''
for p in $PKGS; do dpkg -s \$p 2>/dev/null | grep -q '^Status: install ok installed' || missing=\"\$missing \$p\"; done
if [ -z \"\$missing\" ]; then echo 'pkgs already installed'; exit 0; fi
echo \"installing:\$missing\"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \$missing
apt-get clean
"
t2=$(date +%s)
echo "guest-setup done: cfg $((t1-t0))s, pkgs $((t2-t1))s"
