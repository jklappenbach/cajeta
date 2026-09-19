# cvm distribution — spec

Status: **draft** 2026-09-19. Registered in [INDEX.md](INDEX.md).
Component: `tools/cvm`. Governed alongside `plans/installer/installer-plan.md`
decision D12.

## 1. Definition

cvm is the first binary a user runs. It arrives on a machine that has no
cajeta, no toolchain and no libraries anyone chose, and its job is to
list what is installable and install it.

That imposes two properties nothing else in the tree needs. It must be
**self-contained**, because there is nothing to depend on yet. And it
must be able to **enumerate** releases, because a user who does not
already know a version number cannot ask for one.

Neither property holds today.

### 1.1 Problem statement

Measured 2026-09-19 on proton, x86_64-linux-gnu.

- **cvm links against six shared objects.** `libssl.so.3`,
  `libcrypto.so.3`, `libz.so.1`, `libzstd.so.1`, `libm.so.6`,
  `libc.so.6`. A binary whose purpose is bootstrapping a bare machine
  cannot require OpenSSL 3 to already be present at that soname.
- **There is no way to list releases.** The verbs are `install`,
  `default`, `which`, `doctor`, `self update`, `--version`, `--help`. A
  user must already know a version to install one, or accept `latest`.
- **`install` hangs.** It resolves correctly, printing
  `downloading cajeta 0.28.0 (x86_64-linux-gnu)`, then never returns.
  Killed at 300 seconds, exit 124, with nothing written under
  `$CAJETA_HOME`. So the install path has never worked end to end.
- **The v0.29.0 release failed on all four legs**, 2h13m, in the Build
  cvm step: `no version of 'dev.cajeta.http' satisfies constraints
  [0.1.3]`. Fixed in `e8abf33e`, not yet verified by a green run.

What does work: the build, `--version`, `--help`, `which` and `doctor`,
all honouring `$CAJETA_HOME`. And the release workflow already builds cvm
per platform, publishes a bare executable per triple with a SHA-256, and
emits `.cvmselfmeta` for `index.json`. The CI half is written. It has
never run green.

### 1.2 Scope

Enumerating releases, installing one, and shipping cvm as a
self-contained binary on every supported platform with a release that
proves it.

### 1.3 Non-goals

- **The compiler's own packaging.** cvm installs the toolchain asset the
  release already publishes. How that asset is built is release.yml's.
- **Channels.** `latest` and an explicit version are the selectors. Beta
  and nightly channels are a later shape.

## 2. Enumeration

- **2.1** When a user runs `cvm list`, every installable release is
  printed, newest first, with the version and whether an asset exists
  for this host triple.
- **2.2** When a release has no asset for the host triple, it is shown as
  unavailable rather than omitted, so the absence is visible rather than
  looking like the release does not exist.
- **2.3** When a release is a draft or a pre-release, it is excluded
  unless explicitly asked for.
- **2.4** When the network is unavailable, `list` says so and exits
  non-zero rather than printing an empty list, which reads as "there are
  no releases".
- **2.5** When a release is already installed locally, `list` marks it,
  and marks which one is active.
- **2.6** When the enumeration is paginated, every page is followed. A
  truncated list that looks complete is worse than an error.

### 2.7 Where the list comes from

`index.json` is published **per release**, at both
`releases/download/<tag>/index.json` and the rolling
`releases/latest/download/index.json`. So a stable well-known URL already
exists, and it already carries per-triple assets with checksums. What it
cannot do is describe its siblings, because each release publishes its
own.

Enumeration therefore needs a second document that outlives any one
release: a **catalog**, written by the release job.

The alternative considered and rejected was having each cvm query
GitHub's releases API. It works, and it was the first draft of this
spec, but it puts a 60-per-hour unauthenticated rate limit in front of
every user, it couples cvm to where the project happens to be hosted, and
it answers the wrong question. GitHub reports what releases EXIST,
including ones whose platform legs failed partway or whose assets are
incomplete. What an installer needs is what is INSTALLABLE.

- **2.7.1** The catalog lives at a stable well-known URL, and that URL is
  the only location knowledge cvm carries.
- **2.7.2** The release job REGENERATES it from the published set rather
  than appending to it. An appended file drifts the first time a run is
  re-run or a release is removed.
- **2.7.3** It contains only releases whose every platform leg completed.
  A release that shipped three of four triples is not installable on the
  fourth and must not appear as though it is.
- **2.7.4** The release job is the only thing that queries GitHub. It is
  authenticated, it runs once per release rather than once per user, and
  the rate limit stops being a design constraint.
- **2.7.5** An entry carries the version, the per-triple asset URL and
  SHA-256, and whether the entry is valid.
- **2.7.6** A release can be marked **yanked**, and cvm refuses to
  install a yanked version unless explicitly forced. GitHub has no yank
  concept for releases, so without this a bad release cannot be
  retracted and an installer will keep offering it.
- **2.7.7** The release job FAILS when the catalog it just wrote does not
  contain the version it just published. Without that guard the failure
  mode is silent staleness, where `install latest` serves an old version
  while a newer one exists and nothing reports anything.
- **2.7.8** cvm contains no knowledge of GitHub. It fetches one document
  and parses it.

### 2.8 Where the catalog is stored

Committed to the repository and written by the release job. History then
shows every change, and a yank is a reviewable commit. The cost is that
the workflow needs write access to the default branch.

Two alternatives remain viable and are recorded rather than discarded. A
dedicated mutable release whose single asset is replaced each time needs
no repository write and reuses the upload path the job already has. And
`olla.cajeta.dev`, which cvm's own manifest already declares as a
repository, is existing infrastructure, though it is a package registry
rather than a release index.

## 3. Install

- **3.1** When `install` runs, it terminates. A download that cannot
  proceed fails with a reason inside a bounded time.
- **3.2** When a download stalls, it times out and says which URL and how
  long it waited.
- **3.3** When bytes arrive, the SHA-256 is verified before anything is
  written to the toolchain directory.
- **3.4** When verification fails, nothing is installed and the partial
  download is removed.
- **3.5** When an install succeeds, `which` reports the new toolchain and
  the shim resolves to it.
- **3.6** When `install` is given a version that `list` showed as
  unavailable for this triple, it refuses by name rather than downloading
  something that cannot run.

## 4. Self-contained on every platform

- **4.1** When cvm is built for release on any supported platform, it has
  **no dynamic dependency that the platform does not itself guarantee**.
  On Linux that means no libssl, libcrypto, libz or libzstd. On macOS it
  means nothing outside the system frameworks. On Windows it means no
  redistributable DLL.
- **4.2** When the release is built, the absence of those dependencies is
  ASSERTED by the workflow rather than assumed. `ldd`, `otool -L` and
  `dumpbin /dependents` each have a pass list, and an unexpected entry
  fails the leg.
- **4.3** When a static libc is not the right answer, it is not used. A
  fully static glibc binary breaks DNS resolution through NSS, and cvm
  downloads over the network, so Linux is either musl-static or
  glibc-dynamic with everything else static. This is measured, not
  assumed, because a binary that resolves no hostnames fails exactly
  where cvm is meant to work.
- **4.4** When the platform matrix runs, every leg produces a cvm and the
  release publishes it. A missing leg fails the release rather than
  shipping a partial platform set.

## 5. Proving it on a machine that has nothing

A dependency check on the build machine proves little, because the build
machine has every library.

- **5.1** When a release candidate exists, its Linux cvm is executed in a
  container with no toolchain and no development libraries, and it lists
  and installs successfully.
- **5.2** When the Windows binary exists, it is executed on the runner
  from a directory with no Visual C++ redistributable on the path.
- **5.3** When either check fails, the release does not publish.
