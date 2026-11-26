#!/usr/bin/env bash
# Daily desktop checklist from task files. No AI.
R=/mnt/SSD/LodOS; T=$R/orch/tasks
D=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Masaüstü")
out="$D/LodOS-$(date +%F).md"
field(){ sed -n "s/^$2: *//p" "$1" | head -1; }
{
  echo "# LodOS — $(date '+%F') (güncelleme $(date +%H:%M))"
  echo
  echo "Durum: [x] bitti · [~] çalışıyor · [ ] sırada · [!] takıldı · [?] senden cevap bekliyor"
  echo
  for f in $(ls "$T"/T*.md 2>/dev/null | sort); do
    case "$(field "$f" status)" in
      done) m="[x]";; doing) m="[~]";; blocked) m="[!]";; user) m="[?]";; *) m="[ ]";;
    esac
    echo "- $m $(field "$f" id) — $(field "$f" title) (deneme $(field "$f" attempts)/$(field "$f" max))"
  done
  echo
  echo "## Bugünkü olaylar"
  echo '```'
  grep "^$(date +%F)" "$R/orch/logs/runner.log" 2>/dev/null | tail -25
  echo '```'
  q=$(sed -n '/^## OPEN/,/^## /p' "$R/ai.md" | grep '^- ')
  [ -n "$q" ] && { echo; echo "## Senden beklenenler"; echo "$q"; }
} > "$out"
echo "$out"
