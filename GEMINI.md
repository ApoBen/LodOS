# Rules for agy agents (LodOS). Terse. Obey strictly.

ctx: /mnt/SSD/LodOS/ai.md = project state (read GOAL/DECISIONS/ENV only if needed). Your task file is given in the prompt.

## scope
- write only inside /mnt/SSD/LodOS. Exceptions: ~/development/android (sdkmanager installs), ~/.gradle, ~/.cache.
- NO sudo/root. If root needed: append `NEEDS_ROOT: <exact cmd>` to your task NOTES and stop.
- do NOT edit: ai.md, CLAUDE.md, GEMINI.md, orch/*.sh, orch/review_prompt.md, other tasks' files.
- do NOT run git commit/reset/checkout/push/rebase. Runner commits.
- third-party code: only GPL-3-compatible; record source URL+license+commit in THIRD_PARTY.md. Clones go to third_party/src/ (gitignored).
- research output -> docs/research/. Env vars: `source orch/env.sh` (once T001 done).

## phone (adb -s <phone-serial>, POCO X7 Pro, NOT rooted, user's personal phone)
allowed: install/uninstall/am start/force-stop/dumpsys/logcat for packages `org.lodos*` only; push/exec under /data/local/tmp/lodos/ only; getprop; read-only system info (dumpsys SurfaceFlinger/gpu, /proc, vulkan info).
forbidden: touching orch/phone/* or /data/local/tmp/lodos/{keeper*,work,saved} or screen brightness; other apps' data, /sdcard user files, settings put, pm clear/uninstall of non-org.lodos pkgs, reboot, input injection outside our app, anything irreversible.

## output
at end append to your task file:
```
## NOTES a<attempt>
did: <1-3 lines>
files: <paths>
issues: <or none>
```
Be terse. No prose essays. Don't claim success you didn't verify; the runner runs the accept script.
