#!/usr/bin/env bash
# PC (x86_64) rootless Debian trixie guest — mirror of the phone guest for desktop-stack bring-up without phone.
# usage:
#   scripts/pc-guest.sh setup            fetch+extract rootfs, apply guest-setup cfg, install desktop pkgs (idempotent)
#   scripts/pc-guest.sh run <cmd...>     run cmd in guest (as fake root)
#   scripts/pc-guest.sh shell            interactive bash in guest
# env: PC_BACKEND=proot|bwrap (default proot if build/proot/x86_64/proot exists or builds, else bwrap)
#      EXTRA_BINDS='-b host:guest ...' (proot syntax; also accepted for bwrap as host:guest pairs)
#      PKGS overrides package list for setup
# Host $XDG_RUNTIME_DIR is bound to /run/host-xdg; guest gets XDG_RUNTIME_DIR=/run/host-xdg (+WAYLAND_DISPLAY passthrough).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RFS=$ROOT/build/pc-rootfs
TARBALL=$ROOT/build/rootfs/debian-trixie-amd64.tar.xz
PROOT=$ROOT/build/proot/x86_64/proot
STATE=$ROOT/build/pc-guest          # proot tmp + /dev/shm backing
PKGS=${PKGS:-"phoc phosh foot wayland-utils weston dbus elogind"}
# D3 systemd-free swap (same as phone scripts/guest-init.sh): elogind Conflicts systemd
INIT_PKGS="sysvinit-core libpam-elogind dbus-x11"
DROP_PKGS="systemd systemd-sysv dbus-user-session libpam-systemd"
log() { echo "[pc-guest] $*" >&2; }

ensure_rootfs() {
  [ -x "$RFS/bin/sh" ] && return 0
  [ -s "$TARBALL" ] || bash scripts/fetch-rootfs.sh amd64
  log "extracting $TARBALL -> $RFS"
  rm -rf "$RFS.part"; mkdir -p "$RFS.part"
  # rootless: no chown, no device nodes (guest /dev is host-bound)
  tar -C "$RFS.part" --no-same-owner --exclude='./dev/*' -xJf "$TARBALL"
  mv "$RFS.part" "$RFS"
}

ensure_proot() {
  [ -x "$PROOT" ] && return 0
  log "building proot x86_64"; bash scripts/build-proot-pc.sh >&2 || return 1
}

backend() {
  if [ -n "${PC_BACKEND:-}" ]; then echo "$PC_BACKEND"; return; fi
  if ensure_proot; then echo proot; else log "proot build failed -> bwrap fallback"; echo bwrap; fi
}

GUEST_ENV=(HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
  TERM="${TERM:-xterm}" LANG=C.UTF-8 XDG_RUNTIME_DIR=/run/host-xdg)
[ -n "${WAYLAND_DISPLAY:-}" ] && GUEST_ENV+=(WAYLAND_DISPLAY="$WAYLAND_DISPLAY")
[ -n "${DISPLAY:-}" ] && GUEST_ENV+=(DISPLAY="$DISPLAY")

exec_guest() { # args: command argv
  ensure_rootfs
  mkdir -p "$STATE/tmp" "$STATE/shm" "$STATE/empty-xdg"; chmod 1777 "$STATE/shm"
  local xdg=${XDG_RUNTIME_DIR:-}
  [ -n "$xdg" ] && [ -d "$xdg" ] || xdg=$STATE/empty-xdg
  mkdir -p "$RFS/run/host-xdg"
  local be; be=$(backend)
  case "$be" in
    proot)
      # same flags as phone (scripts/phone-run.sh); /tmp is guest-private (rootfs /tmp)
      PROOT_TMP_DIR=$STATE/tmp exec "$PROOT" -0 -r "$RFS" \
        --link2symlink --sysvipc --kill-on-exit \
        -b /dev -b "$STATE/shm:/dev/shm" -b /proc -b /sys -b "$xdg:/run/host-xdg" \
        ${EXTRA_BINDS:-} -w /root \
        /usr/bin/env -i "${GUEST_ENV[@]}" "$@"
      ;;
    bwrap)
      local extra=() b
      for b in ${EXTRA_BINDS:-}; do [ "$b" = -b ] && continue; extra+=(--bind "${b%%:*}" "${b#*:}"); done
      exec bwrap --unshare-user --uid 0 --gid 0 --bind "$RFS" / \
        --dev-bind /dev /dev --bind "$STATE/shm" /dev/shm --proc /proc --ro-bind /sys /sys \
        --bind "$xdg" /run/host-xdg "${extra[@]}" --chdir /root \
        /usr/bin/env -i "${GUEST_ENV[@]}" "$@"
      ;;
    *) log "unknown backend $be"; exit 2;;
  esac
}

setup() {
  ensure_rootfs
  # guest-setup.sh logic (cfg part), PC variant: no android groups
  "$0" run /bin/sh -c '
set -e
mkdir -p /etc/apt/apt.conf.d /etc/dpkg/dpkg.cfg.d
printf "%s\n" "APT::Sandbox::User \"root\";" "Acquire::Retries \"3\";" "APT::Install-Recommends \"false\";" > /etc/apt/apt.conf.d/01lodos
printf "%s\n" force-unsafe-io > /etc/dpkg/dpkg.cfg.d/01lodos
if [ -L /etc/resolv.conf ] || ! grep -q "^nameserver" /etc/resolv.conf 2>/dev/null; then
  rm -f /etc/resolv.conf; printf "nameserver 8.8.8.8\nnameserver 1.1.1.1\n" > /etc/resolv.conf
fi
printf "#!/bin/sh\nexit 101\n" > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d
echo cfg-ok'
  "$0" run /bin/sh -c "
set -e
missing=''
for p in $PKGS $INIT_PKGS; do dpkg -s \$p 2>/dev/null | grep -q '^Status: install ok installed' || missing=\"\$missing \$p\"; done
if [ -z \"\$missing\" ]; then echo 'pkgs already installed'; exit 0; fi
echo \"installing:\$missing\"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
drop=''
for p in $DROP_PKGS; do dpkg -s \$p >/dev/null 2>&1 && drop=\"\$drop \${p}-\"; done
apt-get install -y -q --no-install-recommends --allow-remove-essential $INIT_PKGS \$missing \$drop
apt-get clean
"
  # Debian ships phosh as /usr/libexec/phosh (not in PATH) -> expose it
  # PATH-level `sh` -> bash (POSIX mode): dash `command -v a b c` only resolves the 1st arg (breaks multi-arg
  # lookups e.g. orch/accept/T036.sh). /bin/sh stays dash, so #!/bin/sh system scripts are unaffected.
  "$0" run /bin/sh -c '[ -x /usr/libexec/phosh ] && ln -sf /usr/libexec/phosh /usr/local/bin/phosh; ln -sf /bin/bash /usr/local/bin/sh; sh -c "command -v phoc phosh foot wayland-info"'
  "$0" run /bin/sh -c 'dpkg-query -W -f="\${Package} \${Version}\n" '"$PKGS"
}

cmd=${1:-}; shift || true
case "$cmd" in
  run)   [ $# -gt 0 ] || { echo "usage: $0 run <cmd...>" >&2; exit 2; }; exec_guest "$@";;
  shell) exec_guest /bin/bash -l;;
  setup) setup;;
  *) sed -n '2,10p' "$0" >&2; exit 2;;
esac
