Task file = orch/tasks/T###.md. Frontmatter lines parsed by runner (keep exact keys):
---
id: T###
title: <short, Turkish ok>
status: todo|doing|done|blocked|user
model: gemini-4-neon-high|gemini-4-carbon-high|...
attempts: 0
max: 3            # last attempt auto-escalates to carbon-high
needs_phone: yes|no
deps: T001,T002   # empty if none
accept: orch/accept/T###.sh   # exit 0 = pass; must be objective & cheap
---
## GOAL / STEPS / OUTPUT  (terse, concrete paths)
Agents append `## NOTES a<n>`. Reviewer may append `## REVIEW`.
