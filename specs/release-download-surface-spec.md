# release-download-surface — spec

## 1. Definition

Every release publishes compiler binaries, archives, native installers and
`cvm`, for four triples. A reader looking for a download sees them in two
places: the repository `README.md` and the cajeta.dev home page. Only the
first is maintained, only partly, and by hand-shaped generation that has
already drifted from what the release actually ships.

This spec covers the surface that points a reader at the latest binaries, and
keeping it correct on every release without anyone editing a link.

### 1.1 What is wrong today

Measured 2026-09-23 against `README.md` at v0.29.0 and `release.yml`.

- **1.1.1** The README table's fifth column is headed `Installer` and contains
  the `cvm` link. The native installers are not linked anywhere.
- **1.1.2** `cpack` builds `.deb`, `.rpm`, `.msi` and `.pkg` and the release
  publishes them, and no document mentions that they exist.
- **1.1.3** The cajeta.dev home page (`site/src/pages/index.astro`) carries no
  download links at all and is never touched by a release.
- **1.1.4** Three of four rows show `—` for cvm because at v0.29.0 cvm built
  only on linux-x64. That is now fixed, which means the table is stale in the
  other direction too.

### 1.2 Scope

The README release block, the cajeta.dev home page, and the release job step
that regenerates both. One generated manifest feeds both surfaces.

### 1.3 Non-goals

- **1.3.1** A separate `README.md.template` file. The `BEGIN:`/`END:` marker
  block already IS the template: the prose around it is hand-written and
  permanent, and only the block between the markers is generated. A whole-file
  template would put hand-written prose under generation and lose it.
- **1.3.2** Anything to do with enumerating releases. `cvm-distribution` Unit
  2A compiles a catalog of EVERY release, appended to by the release job, so
  `cvm list` does not query GitHub each time it is asked. That is a different
  artifact with a different consumer and a different lifetime. This surface
  shows the LATEST release only, for a human to click, and it neither produces
  nor extends that catalog.
- **1.3.3** Changing what the release builds. If an installer is not produced,
  this surface says so rather than inventing a link.

## 2. The latest release, rendered twice

The release job already holds the list of what it just published. The failure
mode to avoid is each surface deriving asset names independently, which is
what `update-release-docs.sh` does today by rebuilding names from a tag and a
triple, and which is why the installers it never names are invisible.

This is one release's asset list, not a history. Nothing here needs a previous
release, so nothing here needs a catalog.

- **2.1** When the release job completes, the assets it published are
  described once: version, and per triple the archive, compiler binary, cvm
  binary and each native installer, each with its URL and digest.
- **2.2** When a surface needs a download link, it reads that description
  rather than rebuilding the name from a tag and a triple.
- **2.3** When an asset is absent for a triple, it is omitted rather than
  recorded with a dead URL, and the rendering shows the absence.

### 2.4 Relationship to cvm's catalog

`cvm-distribution` 2A compiles a catalog of every release so `cvm list` can
answer without a GitHub query. The two touch the same asset names and serve
different readers, so they stay separate artifacts.

- **2.4.1** When this surface is regenerated, it does not read, write or
  extend that catalog.
- **2.4.2** When the two would share anything, it is the code that spells an
  asset name for a triple, never the file. A schema change made to serve
  `cvm list` must not be able to break the home page.

## 3. The README block

- **3.1** When a release publishes, the block between `BEGIN:RELEASE` and
  `END:RELEASE` is regenerated and everything outside the markers is untouched.
- **3.2** When the table is rendered, each row carries the archive, the
  compiler binary, cvm, and every native installer that exists for that triple.
- **3.3** When a column is added or renamed, the header names what the column
  holds. The current header says `Installer` over cvm links.
- **3.4** When an asset for a triple is missing, its cell reads as absent and
  the row still renders.
- **3.5** When the markers are missing from the file, the run fails rather than
  silently writing nothing.

## 4. The cajeta.dev home page

- **4.1** When a release publishes, the home page shows the same latest
  downloads as the README, from the same manifest.
- **4.2** When the site is built, the download data is imported as data rather
  than pattern-matched out of page markup. The page is Astro, and rewriting
  markup with a regex breaks the first time the markup is reformatted.
- **4.3** When a reader arrives on a known platform, the page offers that
  platform's download first. Every platform stays reachable.
- **4.4** When the manifest has not changed, a site rebuild produces the same
  output.

## 5. Running on every release

- **5.1** When a production release completes, both surfaces are updated and
  committed in the same job that published the release.
- **5.2** When the run is a dry run, both renderings are produced and shown as
  a diff, and nothing is committed.
- **5.3** When the release is a prerelease, neither surface is updated, because
  a reader landing on the home page should not be sent to an rc.
- **5.4** When the rendered surface does not contain the version just
  published, the job fails. Silent staleness is the failure mode where the
  home page keeps advertising an old release.
- **5.5** When an installer build was skipped, the release still succeeds and
  the surfaces show that installer as absent. The installer steps are
  non-fatal today and this must not make them fatal by proxy.

## 6. Open questions

- **6.1** Does the home page link the GitHub release asset directly, or a
  stable `latest` redirect? A stable URL survives in a blog post. A direct
  asset URL is honest about the version. Recommendation: show the version and
  link the asset directly, matching the README.
- **6.2** Should `sh.cajeta.dev` (the install script the README already cites)
  spell asset names from the same helper as 2.4.2? It resolves them by its own
  logic today and is a third place the naming can drift.
