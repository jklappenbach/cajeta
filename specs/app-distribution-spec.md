# app-distribution — distributing applications through olla

Spec status: **draft** (2026-08-30)

## 1. Definition

**1.1 Purpose.** Let olla distribute APPLICATIONS, not only libraries: an
archive a person installs and then runs as a command, rather than one a
project links.

**1.2 What is already true, and is not the problem.** A `.cja` is LLVM
bitcode plus per-platform native artifacts under `native/<os>-<arch>/`,
and `--emit=exe` links an executable from it locally
(`EntryKind::RuntimeBitcode` exists for that input). So an application
archive fetches, content-addresses, signs and verifies exactly as a
library does. Transport is not what is missing.

**1.3 The gap this closes.** Everything AROUND execution is unspecified.
Nothing states whether an archive is a library or an application; nothing
says what command it installs as; nobody re-declares its capabilities the
way a consuming project does; its dependency set is re-resolved at install
time rather than being the set its publisher tested; and once installed it
has no lifecycle — no notion of which version is current, no upgrade, no
removal.

**1.4 Why the trust asymmetry forces this.** A library's code runs when
the consumer's own program runs, under the consumer's capability
declaration, which the compiler statically verifies. An application's runs
when a user types a command, under the application's declaration and
nobody else's. `publisher-trust` §5.6 already requires that verification
relaxation never extend to applications, and §5.6.1 records that the
clause has no mechanism until an archive states which it is. This spec is
where that field is defined.

**1.5 Scope.**
- The archive kind, as a declared and published fact (§2).
- Command naming and collision (§3).
- Capability consent at install and at upgrade (§4).
- The published lockfile, so an install reproduces what the publisher
  tested (§5).
- The release artifact set and platform selection (§6).
- The installed-application lifecycle: install, list, upgrade, uninstall
  (§7).

**1.6 Non-goals.**
- **Platform binaries as separate registry entries.** DECIDED 2026-08-30
  (§9.3): a release's platform binaries belong to that release, not to a
  list of sibling coordinates. npm's per-platform-package pattern is
  explicitly rejected — it keeps the registry model simple by pushing the
  fan-out into names, and names are what users read.
- **Apple platform distribution.** `apple-targets-spec` owns signing,
  notarization and the App Store, and hands distribution to Xcode.
- **New sandbox enforcement.** §4 surfaces and gates on the EXISTING
  capability declaration; it does not extend what the sandbox enforces.
  Ten of the fourteen specified capabilities are still reserved names
  that the build tool does not parse.
- **Changing `publisher-trust`.** This spec supplies the field §5.6
  needs; it does not revisit the verification model.
- **The olla catalog and web UI.** Indexing and presenting the kind is
  server work in the `cajeta-olla` repo. This spec defines what is
  published; §8 is the contract handed over.

**1.7 Constraints, verified against the source.**
- `cajeta install` is TAKEN — it resolves a project's dependencies
  (`BuildToolCommands.cpp:1066`). `cajeta run` is TAKEN — script-units §7
  made it a first-class verb. Neither can be silently repurposed.
- `cajeta upgrade` already prompts on a dependency's capability
  additions, rolls back on refusal, and records the accepted set. §4
  should reuse that flow, not invent a second one.
- `settings.build.binaries` already registers named binaries, each with
  its own `entryMethod`. The entry point is therefore already TRANSMITTED
  in the published manifest sidecar; nothing models or reads it.
- `~/.olla/` is the existing machine-global store, laid out
  `<name>/<version>/…`. It has no notion of a current version.
- `--emit=exe` writes to `<output-dir>/exe/<details.name>`, which
  collides with the project's own object tree when `details.name` equals a
  top-level package name (`buildtool-exe-package-name-collision-spec`).
  An installed command must not inherit that trap.

## 2. The archive kind

**2.1** An archive declares whether it is a LIBRARY or an APPLICATION.

**2.2** When a manifest declares no kind, the archive is a library. This
is safe rather than a violation of `publisher-trust` §5.6.1: before this
spec there was no way to publish an application, so every archive
predating it is a library in fact. §5.6.1 forbids guessing when the answer
is genuinely unknown, and here it is not.

**2.3** When an upload declares no kind after this lands, the repository
refuses it. The default in 2.2 is a migration path for archives already
published, not a permanent shape.

**2.4** When an archive is published, its kind travels in the release
metadata that the repository root signs. An unsigned kind would let a
mirror recategorize an application as a library and, through
`publisher-trust` §5.6, obtain a relaxation that does not apply to it.

**2.5** When an application is requested as a dependency, resolution
fails. Applications are installed, not linked.

**2.6** When a library is installed as an application, the install fails
and says so. There is nothing to run.

## 3. Command naming

**3.1** An application declares the command name it installs as. It is
never derived from `details.name`: `(name, version)` is globally unique
and a command name is not, so deriving one manufactures collisions out of
names that were legitimately distinct.

**3.2** When an application declares several binaries, each declares its
own command name.

**3.3** When an install would place a command that already exists, it
stops and reports what holds the name. Silently winning is how a
package manager replaces a command a user relies on.

**3.4** When a colliding install is wanted anyway, an explicit rename
performs it. The user chooses the name; the tool does not choose for
them.

**3.5** When two installed applications declare the same command name,
both remain installed and at most one holds the command. Uninstalling the
holder does not silently promote the other.

**3.6** A declared command name is validated at publish time against the
character set a shell command can safely carry.

**3.7** An installed command's on-disk path is independent of the
application's package structure, so the `--emit=exe` collision of §1.7
cannot reach it.

## 4. Capability consent

**4.1** When an application is installed, the capabilities it requires
are shown before anything is written, and the install proceeds only on
acceptance.

**4.2** The consent covers the application AND its dependency closure.
The union is what will actually run; a per-package list that a reader has
to add up themselves is a list nobody reads.

**4.3** When an upgrade widens the capability set, the addition is shown
and the upgrade proceeds only on acceptance — the flow `cajeta upgrade`
already implements for dependencies.

**4.4** When an upgrade does not widen the capability set, it does not
prompt. A prompt that fires on every upgrade trains the user to accept it.

**4.5** When consent is refused, nothing is installed or changed.

**4.6** The accepted capability set is recorded, so a later upgrade can
tell what was actually agreed to rather than re-deriving it.

**4.7** Consent is per application, not global. Accepting `network` for
one application says nothing about another.

## 5. The published lockfile

**5.1** An application publishes the lockfile it was built and tested
against.

**5.2** When an application is installed FROM ITS IR ARTIFACT, its
dependency set comes from that published lockfile rather than from a fresh
resolve. A fresh resolve can legitimately choose versions the publisher
never ran.

**5.2.1** When an application is installed from a PLATFORM BINARY, there
is no install-time resolution and the lockfile is not consulted. It is
still published, as the provenance record of what went into that binary,
and it is what an audit reads. This asymmetry is a consequence of §9.3's
decision: the two artifact forms need different things at install.

**5.3** The published lockfile is covered by the same signed path as the
release hash, so a mirror cannot substitute a dependency set.

**5.4** When the lockfile names an artifact that cannot be fetched or
does not verify, the install fails and nothing is written.

**5.5** A library does not publish a lockfile. Its consumer resolves it
against their own graph, which is the entire difference between the two
kinds.

## 6. The release artifact set and platform selection

**6.1** A release carries a SET of artifacts, not one. Each is either the
portable IR archive or a binary built for one platform.

**6.2** A release carries at least one artifact. A release carrying none
is not installable and is refused at upload.

**6.3** At most one IR artifact, and at most one binary per platform. Two
answers for one platform is an ambiguity nothing downstream can resolve.

**6.4** Every artifact in the set is named, with its own hash, inside the
signed release metadata. A per-platform hash that a mirror could supply
unsigned would leave the platform binaries — the ones that execute
directly, with no compile step in between — as the only unverified thing
in the chain.

**6.5** When an application is installed, a binary matching the host
platform is preferred, and the IR artifact is the fallback.

**6.6** When neither a matching binary nor an IR artifact exists for the
host, the install fails BEFORE fetching anything, naming the platforms the
release does support. Selection reads the signed metadata, so this costs
no download.

**6.7** When installing from the IR artifact, the local AOT link is what
produces the command, so a missing per-platform native artifact inside the
closure surfaces at install. The release records which platforms its IR
path can actually link for, so §6.6 answers correctly rather than failing
late.

**6.8** When an application's dependency closure narrows the set of
platforms its IR path supports, the recorded set reflects the closure and
not just the application's own native artifacts.

**6.9** A library release carries exactly one artifact: its IR archive.
Nothing above changes how a library is published or resolved.

## 7. Lifecycle

**7.1** An installed application has a current version, which is what its
command runs.

**7.2** Installed applications can be listed, with their versions and the
commands they hold.

**7.3** An application can be upgraded, subject to §4.3.

**7.4** An application can be uninstalled, which removes its command.

**7.5** When an application is uninstalled, artifacts still needed by
something else are retained. The artifact store is shared with the
library cache; uninstall is not a licence to prune it.

**7.6** Installing a second version of an application replaces the
current one rather than accumulating. `(name, version)` remains
addressable in the store; what changes is which one the command runs.

**7.7** Installed-application state is machine-global and lives beside
`~/.olla/`, not inside any project.

## 8. What the repository serves and refuses

Server-side, and a contract on `cajeta-olla` rather than work in this
repo — the split `publisher-trust` §1.7 already draws.

**8.1** The kind is served in the signed release metadata (§2.4).

**8.2** The artifact set (§6.1-6.4), the platforms the IR path can link
for (§6.7), and the published lockfile (§5.1) are served for
applications.

**8.2.1** A release is one catalog entry however many artifacts it
carries. Platform binaries must not surface as versions, tags, or sibling
coordinates — the decision in §9.3 is about what a reader sees as much as
about storage.

**8.3** An upload declaring no kind is refused (§2.3).

**8.4** An upload declaring kind APPLICATION with no command name, no
entry method, no lockfile, or no artifact at all (§6.2) is refused. Each is required for the archive
to be installable at all, and an archive that cannot be installed should
not enter the registry.

**8.5** The catalog distinguishes the two kinds, so a search for
something to run does not return libraries.

## 9. Open questions

**9.1 Verb naming.** `cajeta install` and `cajeta run` are both taken
(§1.7). RECOMMENDATION: a noun-first group — `cajeta app install`,
`cajeta app list`, `cajeta app upgrade`, `cajeta app uninstall` — which
leaves both existing verbs unambiguous and gives the lifecycle one home.

**9.2 Where the kind and command are declared.** RECOMMENDATION:
`details.kind` (`library` | `application`), and a `command` field on each
entry in `settings.build.binaries`, defaulting to the binary's registry
name. `details` is already the strict-schema identity block, which is
what the kind is; the command belongs with the binary it launches.

**9.3 Install-time AOT vs prebuilt — DECIDED 2026-08-30 (Julian): BOTH,
within one release.** A release may carry the portable IR archive, one
binary per platform, or both. Platform binaries belong to the release
they came from; they do not register as sibling coordinates in a list of
tags.

Rejected alternatives, and why:

- **IR only.** Installing means linking: a toolchain must be present and
  it costs build-scale time. Correct as a floor, insufficient as the only
  answer.
- **Platform binaries as separate archives** (`dev.acme.tool.linux-x64`,
  npm's pattern). Keeps the registry's per-release model untouched by
  pushing the fan-out into names — and names are what users read. A
  release that fragments across coordinates is worse to browse, worse to
  retract, and worse to reason about than one that carries its own
  artifacts.

The consequence is that release metadata carries a set of (platform,
hash) pairs rather than one hash (§6.4). That is a change to
`publisher-trust`'s shipped signed path — see §10.

**9.4 Does an application pin its toolchain version?** A published
lockfile fixes dependencies but not the compiler that will link them. An
application built and tested on one toolchain may not link on another.
9.3 keeps the IR path, so this still needs an answer — but only for that
path: a platform binary is already linked and does not care what compiles
it. STILL OPEN.

## 10. What 9.3 costs the shipped publisher-trust code

Recorded here rather than discovered during implementation.

**10.1** `ReleaseMetadata::sha256` is a single field and
`releaseIntegrityFor` compares fetched bytes against that one value.
Both grow a platform dimension.

**10.2** `specs/schemas/release-metadata.json` gains the artifact set. The
signed payload is what carries it (§6.4), so this is a schema change on
the signed path, not an additive field beside it.

**10.3** `specs/schemas/publisher-trust-protocol-v1.md` §3.4 describes a
release as having one hash. It is a draft awaiting confirmation, so it can
absorb this before anyone implements against it.

**10.4** Olla's catalog keys one `sha256` per version row
(`src/routes/resolve.ts`, `getVersion`). A version becomes one-to-many
with artifacts. That is a migration on the server side.

**10.5 The timing argument, which is the reason to decide this now.**
Olla serves the PLAIN resolve body today: no `signed` member and no
`/v2/org-keys` — verified in `cajeta-olla/src/routes/resolve.ts`. The
signed release-metadata format therefore has ZERO production instances,
and `publisher-trust` Unit 6 is fenced precisely on olla not serving it
yet. Changing the shape now costs code edits. Changing it after olla ships
the signed path costs a format migration on documents already signed and
cached in the field. The envelope's `format` version exists for that, and
not spending it is worth some hurry.
