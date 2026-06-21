# CLAUDE.md — cajeta

Project-level guidance for the Cajeta language repository. The workspace-level
`CLAUDE.md` a directory up (`/home/julian/code/CLAUDE.md`) still applies; this file
adds repo-specific conventions and imports the spec→plan→develop governing memory.

@td-project-workflow.md

## Skill discovery — search before you implement

Before writing cajeta code against a library/package/class/method, run
`cajeta search-skill <canonical-name>` to find skills (implementation guidance)
shipped inside the resolved dependencies. If it returns URIs you don't already
hold, fetch them with `cajeta get-skills <uri>[,<uri>...]`; if you already hold a
URI's payload, skip the fetch (the URI is a stable cache key). See
`docs/SkillDiscovery.md`. Search is fuzzy (typo-tolerant, over names and titles).
