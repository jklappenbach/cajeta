# Library discovery — the compiler MCP finds the library for a goal

## 1. Definition

### 1.1 Purpose

An agent writing cajeta knows what it wants to do and not which published
library does it. Today the compiler's MCP server (`cajeta compiler-mcp`)
answers only from skills already on disk: the stdlib corpus embedded in the
compiler and the `.cja` archives of dependencies the project has resolved.
A library the project has not yet added is invisible, so the agent either
writes the thing itself or asks the developer.

This spec adds one question to the MCP server: given a goal, which published
libraries on Olla accomplish it, at which version, and which skill should be
read next. Olla is the registry (`https://olla.cajeta.dev`) and already
searches package name, description, keywords and readme. The registry side
of this work, indexing each published version's skills so a goal matches
what a library can do rather than what its readme happens to mention, is
`cajeta-olla/specs/olla-skill-index-spec.md`. This spec is the client side
and the contract between the two.

Requested by Julian 2026-09-25.

### 1.2 Problem it solves

Measured 2026-09-25 against the live registry: `/v2/search?q=identity`
returns `dev.cajeta.xgboost`, because its readme contains the word, and not
`dev.cajeta.cloud`, which holds the identity port. `q=websocket` finds
`dev.cajeta.http` correctly. The search works. It ranks prose above
purpose, because purpose is not indexed: no manifest fills the `keywords`
column Olla stores, and the skills inside every published archive, which
state in machine-facing frontmatter exactly what a library is for, are never
read by the registry.

### 1.3 Scope

- **1.3.1** A `findLibraries` tool on the compiler MCP server and a
  `cajeta find-libraries` subcommand over the same core.
- **1.3.2** Reading a skill from a library the project has not resolved, so
  the agent can judge a candidate before adding the dependency.
- **1.3.3** The contract with Olla's search response: what a hit carries and
  how hits are ranked.
- **1.3.4** `details.keywords` in `cajeta.json`, published with the package.

### 1.4 Non-goals

- **1.4.1** Editing the project. The tool reports the dependency line to add
  and the agent edits `cajeta.json`. Adding, resolving and locking stay with
  the build tool's existing verbs.
- **1.4.2** A new search backend. Olla's providers (D1 FTS5 trigram, Algolia)
  stay. Embedding search is a later provider behind the same contract.
- **1.4.3** Authentication. Search and skill reads are anonymous, like
  `/v2/resolve` and `/v2/blob`.
- **1.4.4** Ranking by popularity or downloads. Olla does not record them.

### 1.5 Systems

`src/cajeta/buildtool/skill/` (SkillSearch, SkillIndex, SkillUri, SkillGet),
`src/cajeta/buildtool/mcp/CompilerMcpServer.cpp`,
`src/cajeta/buildtool/repo/HttpRepository.cpp` (the libcurl client that
already speaks `/v2/resolve` and `/v2/blob`), `cajeta.json` `details`,
Olla `/v2/search` and the endpoints the registry spec adds.

---

## 2. Feature: the goal query

- **2.1** When an agent calls `findLibraries` with a goal in plain words, the
  result is a ranked list of published libraries, each with its name, the
  version to depend on, its description, and the skills that matched.
- **2.2** When a goal is answered by the stdlib, the stdlib skills that match
  are reported first and marked as stdlib, from the embedded corpus and
  without a network round trip. A library is never recommended for what
  `cajeta.*` already does.
- **2.3** When a goal is answered by a dependency the project already
  resolved, that library is reported and marked as present, from the local
  archive. The agent is not told to add what it has.
- **2.4** When the query reaches the registry, it goes to the repositories
  named in the project's `cajeta.json` in priority order, and to central
  when the working directory has no project.
- **2.5** When the registry cannot be reached, the tool fails naming the
  repository and the cause. It does not answer from stale results and it
  does not pretend the goal is unserved.
- **2.6** When a hit carries matched skills, each is given as a
  `cja-skill://<library>@<version>/<id>` URI with its title and description,
  so the next call is `getSkills` on that URI.
- **2.7** When a hit is reported, it carries the manifest line to add,
  `"<name>": "<version>"`, with the exact latest version and never a
  wildcard (the wildcard resolver finding of 2026-09-25 is open).
- **2.8** When the same goal is put to `cajeta find-libraries` on the command
  line, the answer is the same list in the CLI's text and JSON forms. The
  core is transport-agnostic, as skill discovery's was.

---

## 3. Feature: reading a skill before adding the dependency

- **3.1** When `getSkills` is given a URI whose library is not resolved
  locally, the skill payload is fetched from the registry that reported it,
  by library, version and id, and returned like a local one.
- **3.2** When the fetched skill is returned, the result says it came from
  the registry and names the version, so the agent knows it is reading
  guidance for a library it has not added.
- **3.3** When the library is resolved locally, the local archive wins and
  the network is not touched, as today.
- **3.4** When a remote fetch fails, the error names the repository, the
  library and the version. It never falls back to a different version.

---

## 4. Feature: the contract with the registry

- **4.1** When Olla answers a search, a hit may carry matched skills (id,
  title, description, version found in) and matched classes (the class-index
  spec). Both fields are optional and a client that does not know them is
  unaffected.
- **4.2** When a query matches a skill's title or description, or a package's
  description or keywords, those hits rank above hits that matched only the
  readme. Purpose outranks prose. This is the fix for §1.2.
- **4.3** When a query is restricted to skills, a query parameter does it.
  `findLibraries` uses the default, which searches everything, and reads the
  matched fields to explain each hit.
- **4.4** When a client needs a skill's payload for a published version, the
  registry serves it by library, version and id, and serves the version's
  skill index as a list. These reads need no resolution of the archive.
- **4.5** When a skill index is served for a version, it is the
  `skills/index.json` the compiler wrote into that archive, so the client
  parses it with the SkillIndex it already has.

---

## 5. Feature: keywords in the manifest

- **5.1** When `cajeta.json` carries `details.keywords`, an array of short
  strings, they are published with the package and land in Olla's existing
  `keywords` column, which nothing fills today.
- **5.2** When a manifest has no keywords, publishing proceeds unchanged.
  Keywords improve ranking and are not required.
- **5.3** When a keyword contains whitespace or exceeds 40 bytes, `cajeta
  info` reports it and publish refuses it. A keyword is a term, not a
  sentence.
- **5.4** The libraries this project publishes (cloud, http, codec, logging,
  unit, primavera, ml, xgboost) gain keywords in their next release.

---

## 6. Open questions

- **6.1** Is Olla's trigram FTS with field weighting (§4.2) enough as the
  matcher, or should an embedding provider be scoped now? Recommendation:
  FTS with weights first. The tool's contract (§2, §4) does not change when
  a provider does, and the skill index gives FTS the right text to match.
- **6.2** Should `getSkills` fetch remote payloads (§3), or should the agent
  add the dependency and resolve before reading? Recommendation: fetch. The
  point of reading the skill is deciding whether to add the dependency.
- **6.3** Should `findLibraries` ever consult more than one repository per
  call? Recommendation: all of the project's repositories, in priority
  order, with hits labelled by repository, so a private registry and central
  both answer.
- **6.4** Should the stdlib answer (§2.2) suppress registry hits entirely
  when it matches, or lead them? Recommendation: lead them. A library may
  still be the better answer and the agent can see both.

## 7. Acceptance criteria (spec-level)

- **7.1** `findLibraries("register users with confirmation")` against the
  live registry returns `dev.cajeta.cloud` above `dev.cajeta.xgboost` once
  cloud's next version is published with its identity skill, and the hit
  carries the `cloud-identity` skill URI.
- **7.2** `findLibraries("sha256")` reports the stdlib first and no library
  for it.
- **7.3** `getSkills` on a URI for an unresolved library returns the same
  payload the resolved archive holds, byte for byte.
- **7.4** With the network unreachable, `findLibraries` fails naming the
  repository, and `searchSkills`, `listSkills` and `getSkills` on local
  corpora behave exactly as before.
- **7.5** A manifest with a keyword that has whitespace fails `cajeta info`.
