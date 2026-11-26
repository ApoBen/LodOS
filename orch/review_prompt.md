LodOS checkpoint review (headless, coordinator role). Be token-frugal: do NOT read whole repo or long logs.
1. Read ai.md. Then `tail -40 orch/logs/runner.log`, `git log --oneline -15`, and task files whose status changed (done/blocked) since last review (NOTES sections only).
2. For done tasks: spot-check acceptance logs (orch/logs/<id>.accept.log tail) and key files briefly. If quality is bad, reopen (status: todo, max += 2, add `## REVIEW` hint).
3. For blocked tasks: diagnose from NOTES + accept log tail; either rewrite the task (smaller scope / clearer steps / better model), split it, or mark `status: user` with a 1-line question in ai.md OPEN if only the user can resolve (e.g. NEEDS_ROOT).
4. Plan next: keep >=3 runnable `todo` tasks in orch/tasks following orch/tasks/FORMAT.md, ordered by ai.md PHASES. Small, verifiable tasks with real accept scripts in orch/accept/. Use gemini-4-neon-high by default, gemini-4-carbon-high for research/hard native/GPU work.
5. Update ai.md: TASKS (one line each), DECISIONS (new only), RISKS, LOG (one line). Terse, AI-oriented.
6. Never implement large code yourself; delegate via tasks. Do not edit orch/*.sh unless broken.
Finish with a one-line summary.
