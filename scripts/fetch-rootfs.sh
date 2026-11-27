#!/usr/bin/env bash
# Fetch minimal Debian 13 (trixie) rootfs tarball. Rootless, no qemu, no extraction.
# usage: scripts/fetch-rootfs.sh [arm64|amd64]   (or ARCH env; default arm64)
# Primary: images.linuxcontainers.org (debian/trixie/$ARCH/default, latest build, SHA256SUMS verified).
# Fallback: Docker Hub official debian:trixie linux/$ARCH layer (digest verified), recompressed to xz.
# Output: build/rootfs/debian-trixie-$ARCH.{tar.xz,sha256} + SOURCE[-$ARCH].txt. Idempotent.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/rootfs"
ARCH="${1:-${ARCH:-arm64}}"
case "$ARCH" in arm64|amd64) ;; *) echo "unsupported ARCH: $ARCH (arm64|amd64)" >&2; exit 2;; esac
NAME="debian-trixie-$ARCH"
TAR="$OUT/$NAME.tar.xz"
SUMF="$OUT/$NAME.sha256"
# arm64 keeps legacy SOURCE.txt name
if [[ "$ARCH" == arm64 ]]; then SRCF="$OUT/SOURCE.txt"; else SRCF="$OUT/SOURCE-$ARCH.txt"; fi
LXC="https://images.linuxcontainers.org"
CURL=(curl -fsSL --retry 3 --connect-timeout 20)
FORCE="${FORCE:-0}"

mkdir -p "$OUT"
log() { echo "[fetch-rootfs] $*" >&2; }

local_ok() {
  [[ -s "$TAR" && -s "$SUMF" ]] && (cd "$OUT" && sha256sum -c --status "$NAME.sha256")
}

finish() { # $1=url $2=upstream sha $3=method
  (cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.sha256")
  {
    echo "method: $3"
    echo "url: $1"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "upstream_sha256: $2"
    echo "local_sha256: $(cut -d' ' -f1 "$SUMF")"
    echo "size: $(stat -c %s "$TAR")"
  } > "$SRCF"
  log "done: $TAR ($(du -h "$TAR" | cut -f1))"
}

fetch_lxc() {
  local path url sums sha tmp
  path=$("${CURL[@]}" "$LXC/meta/1.0/index-system" | awk -F';' -v a="$ARCH" '$1=="debian"&&$2=="trixie"&&$3==a&&$4=="default"{print $6}' | tail -1)
  [[ -n "$path" ]] || { log "lxc: trixie $ARCH not in index"; return 1; }
  url="$LXC${path}rootfs.tar.xz"
  sums=$("${CURL[@]}" "$LXC${path}SHA256SUMS") || { log "lxc: no SHA256SUMS"; return 1; }
  sha=$(awk '$2=="rootfs.tar.xz"{print $1}' <<<"$sums")
  [[ ${#sha} -eq 64 ]] || { log "lxc: rootfs.tar.xz not in SHA256SUMS"; return 1; }

  if [[ "$FORCE" != 1 ]] && local_ok && [[ "$(cut -d' ' -f1 "$SUMF")" == "$sha" ]]; then
    log "up to date ($sha), skip download"; return 0
  fi
  log "lxc: downloading $url"
  tmp="$TAR.part"
  "${CURL[@]}" -o "$tmp" "$url"
  echo "$sha  $tmp" | sha256sum -c --status || { log "lxc: checksum mismatch"; rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$TAR"
  finish "$url" "$sha" "lxc"
}

fetch_docker() {
  local repo=library/debian tag=trixie reg=https://registry-1.docker.io tok idx man dig layer sha tmp
  local A_IDX="application/vnd.oci.image.index.v1+json,application/vnd.docker.distribution.manifest.list.v2+json"
  local A_MAN="application/vnd.oci.image.manifest.v1+json,application/vnd.docker.distribution.manifest.v2+json"
  command -v python3 >/dev/null || { log "docker: python3 needed for json"; return 1; }
  tok=$("${CURL[@]}" "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$repo:pull" \
        | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
  idx=$("${CURL[@]}" -H "Authorization: Bearer $tok" -H "Accept: $A_IDX" "$reg/v2/$repo/manifests/$tag")
  dig=$(A="$ARCH" python3 -c 'import sys,json,os
a=os.environ["A"]
for m in json.load(sys.stdin)["manifests"]:
  p=m.get("platform",{})
  if p.get("os")=="linux" and p.get("architecture")==a and (a!="arm64" or p.get("variant","v8")=="v8"): print(m["digest"]); break' <<<"$idx")
  [[ -n "$dig" ]] || { log "docker: no $ARCH manifest"; return 1; }
  man=$("${CURL[@]}" -H "Authorization: Bearer $tok" -H "Accept: $A_MAN" "$reg/v2/$repo/manifests/$dig")
  layer=$(python3 -c 'import sys,json;l=json.load(sys.stdin)["layers"];assert len(l)==1,len(l);print(l[0]["digest"])' <<<"$man")
  sha=${layer#sha256:}
  if [[ "$FORCE" != 1 && -s "$SRCF" ]] && local_ok && grep -qx "upstream_sha256: $sha" "$SRCF"; then
    log "up to date (docker $sha), skip download"; return 0
  fi
  log "docker: downloading layer $layer"
  tmp="$OUT/layer.tar.gz.part"
  "${CURL[@]}" -H "Authorization: Bearer $tok" -o "$tmp" "$reg/v2/$repo/blobs/$layer"
  echo "$sha  $tmp" | sha256sum -c --status || { log "docker: digest mismatch"; rm -f "$tmp"; return 1; }
  gzip -dc "$tmp" | xz -T0 -6 > "$TAR.part"
  rm -f "$tmp"; mv -f "$TAR.part" "$TAR"
  finish "$reg/v2/$repo/blobs/$layer (debian:$tag linux/$ARCH)" "$sha" "docker"
}

if [[ "${METHOD:-}" == docker ]]; then fetch_docker; exit; fi
fetch_lxc || { log "lxc failed, trying docker fallback"; fetch_docker; } || {
  if local_ok; then log "network fetch failed, keeping existing valid $TAR"; exit 0; fi
  log "all sources failed"; exit 1
}
