# Cajeta Website — Implementation Plan

> Status: **Plan / design** (no site code scaffolded yet).
> Target location: `./site` (i.e. `/home/julian/code/cpp/cajeta-two/site`).
> Decided: **Next.js 15 + Fumadocs** stack · hosting decided later (build static-friendly) · caramel-cube logo as **SVG placeholder** (markup embedded below).

This document is the build spec for the public Cajeta website: a marketing +
documentation + package-registry site for the Cajeta programming language.

---

## 1. Goals

Build a modern, professional, **React-based** marketing + documentation site for
Cajeta with a warm "caramel" identity (browns & creams), custom fonts, a
glossy caramel-cube logo, and sophisticated, accessible UI controls.

Required top-level destinations (from the brief):

1. **What is Cajeta** — landing / elevator pitch.
2. **Why Cajeta** — motivation & differentiators.
3. **Code Examples** — real Cajeta snippets with syntax highlighting.
4. **Full Documentation** — the `cajeta-docs/` tree, browsable with search.
5. **Cajeta Repo** — the package registry (Maven-Central-style: search, bundling, publish).
6. **Contact** — reach Julian Klappenbach.

### Non-goals (v1)
- Not the live registry backend itself — the Repo page is the *front door* to
  `repo.cajeta.org` (see §8), not the registry server.

### Supersedes the existing Astro docs site
`./site` **replaces** the existing **`../cajeta-docs-site/`** (an Astro/AstroWind
site). Its docs are stale hand-copies styled with generic Tailwind `prose` (the
"raw md" look to be fixed). We migrate its *value* — the category taxonomy and
branding — into `./site`, wire docs to the **canonical `cajeta-docs/`** tree so
edits auto-reflect, then **delete `cajeta-docs-site/`**. Full migration plan in
**§14**.

### 1.1 Repo boundaries — decided

Two repos, not three. The boundary falls where **content ownership** falls, not
where the tech stack changes:

- **`cajeta`** (this repo) — compiler + canonical `cajeta-docs/` + the wire
  **spec** (`cajeta-docs/specs/`) + **`./site`**. The site lives **in-repo**.
- **`cajeta-olla`** — the registry **service** (server, `/v2` API, web app), in
  its own repo: **https://github.com/jklappenbach/cajeta-olla** (infra plan
  there; see also the §15 pointer below).

**Why the site stays in-repo (and Olla does not).** Both have a non-C++ stack, so
"different stack" is *not* the test. The test is **what each consumes**:

- **The site consumes *living content from this repo*.** Per §9/§14 it renders
  `../cajeta-docs/**` (processed into styled, searchable pages via the Fumadocs
  pipeline, §14.2), plus `../samples/**`, `../VERSION`, and `README.md`/
  `Features.md` quoted verbatim. The "processed cajetadocs → made available" step
  is a **build transform over files in the same working tree**. Splitting the
  site out would turn four relative `../` reads into cross-repo sync (submodule +
  CI trigger on every docs/version change) just to recover the auto-reflect
  guarantee the plan is built around. Co-location is what makes auto-reflect free.
- **Olla consumes a *stable, versioned contract*.** It needs only
  `cajeta-docs/specs/*`, pulled as a submodule / published schema package and
  conformance-tested. That contract changes rarely and deliberately, so Olla can
  version and deploy on its own clock.

**Release cadence confirms it.** The site *should* ship in lockstep with the docs
and `VERSION` it renders (coupling is correct here); Olla *should not* redeploy
because the compiler did (independence is correct there).

**Practical notes for the in-repo site:**
- Path-scope CI so the C++ and site builds don't trigger each other —
  `on: push: paths: ['site/**']` for the site workflow.
- Cloudflare Pages builds fine from a **subdirectory**: set the project root to
  `site/` (§15.3 step D).
- If a real web-contributor pool ever appears, the escape hatch is a git
  submodule pulling `cajeta-docs` — strictly more plumbing, no benefit today.

---

## 2. Tech stack

Per decision: **Next.js + Fumadocs**.

| Concern            | Choice                                                            |
|--------------------|-------------------------------------------------------------------|
| Framework          | **Next.js 15** (App Router, React Server Components), TypeScript   |
| Styling            | **Tailwind CSS v4** (CSS-first `@theme` config)                    |
| Components         | **shadcn/ui** (Radix primitives — accessible, sophisticated)      |
| Icons              | **lucide-react** (consistent, modern line icons)                  |
| Docs engine        | **Fumadocs** (`fumadocs-ui` + `fumadocs-mdx`) — MDX, sidebar, search |
| Code highlighting  | **Shiki** (bundled via Fumadocs) + custom Cajeta grammar (§6.4)   |
| Fonts              | `next/font` self-hosting — **Fraunces** (display), **Inter**/**Geist** (body), **JetBrains Mono** (code) |
| Animation          | **Framer Motion** (subtle, restrained — hero/scroll reveals)      |
| Forms (Contact)    | shadcn `Form` + `react-hook-form` + `zod`; submit via Formspree/Resend (§7.6) |
| Search             | Fumadocs built-in (Orama static search) for docs; registry search is separate (§8) |
| Lint/format        | ESLint + Prettier (+ `prettier-plugin-tailwindcss`)               |
| Package manager    | **npm** (Node 22 already present; lockfile committed)             |

### Why this stack
- Fumadocs gives a first-class, themable docs experience (sidebar tree, TOC,
  full-text search, MDX components) that we can recolor to the caramel palette —
  far less hand-wiring than rolling docs in a plain SPA.
- shadcn/ui + Radix delivers the "sophisticated icons and controls" ask with
  real accessibility (focus management, keyboard nav) instead of bespoke widgets.
- Next.js supports both `output: 'export'` (static) and SSR/ISR, so the
  "decide hosting later" choice stays open (see §10).

---

## 3. Design system

> **Inspiration:** see `plans/cajeta-site-design-references.md` — a curated list of
> award-winning / critically-acclaimed brown·black·cream sites (Aesop, OXMAN,
> Cartier W&W, Awwwards Brown + CSSDA Beige honorees) and the design patterns to
> borrow.

### 3.1 Color palette — "Caramel, Cream & Clay" (hearth / Mexican artisan)

Warm, edible, earthen, hand-made. The world of cajeta: a clay **olla**, caramel,
a warm family **kitchen**, wood, comfort. Defined as Tailwind v4 `@theme`
tokens and CSS variables (light + dark).

| Token             | Hex       | Role                                            |
|-------------------|-----------|-------------------------------------------------|
| `cream-50`        | `#FBF6EE` | Page background (light)                          |
| `cream-100`       | `#F5EBDD` | Card / raised surface (light)                    |
| `cream-200`       | `#ECDcC5` | Borders, dividers on cream                       |
| `caramel-300`     | `#E0A85B` | Glossy accent / highlights                       |
| `caramel-400`     | `#C68A3E` | Primary brand fill (buttons, links)              |
| `caramel-500`     | `#B97A2E` | Primary hover / emphasis                         |
| `toffee-600`      | `#8A5A22` | Strong accent, badges                            |
| `cocoa-700`       | `#6B4423` | Headings on cream, dark surfaces                 |
| `espresso-800`    | `#4A2C16` | Dark background (dark mode base)                 |
| `espresso-900`    | `#2A1A0F` | Body text (light) / deepest dark bg              |
| `glaze`           | `#FFE6B8` | Top-face glaze / glow highlight                  |
| `terracotta-400`  | `#C26B4A` | Clay / olla accent; registry section             |
| `terracotta-600`  | `#9E4E32` | Deep clay; badges, seals                         |
| `wood-700`        | `#6B4A2E` | Wood panels, dividers, frames                    |
| `comal-900`       | `#241E1A` | Worn-iron / hearth near-black (dark base)        |
| `gold-400`        | `#C9A24B` | Foil / gold accent — premium moments only        |

Semantic mapping (shadcn tokens): `--primary` = `caramel-400`, `--background` =
`cream-50` (light) / `espresso-900` (dark), `--foreground` = `espresso-900` /
`cream-50`, `--accent` = `caramel-300`, `--muted` = `cream-100` / `espresso-800`,
`--border` = `cream-200` / `cocoa-700`. Provide a full light/dark token set.

Gradients: a signature "molten caramel" gradient
(`from-caramel-300 via-caramel-400 to-toffee-600`) for the hero and CTA
surfaces; a subtle radial `glaze`→transparent glow behind the logo.

### 3.1.1 Texture & material system — "hearth / Mexican artisan"
The unifying metaphor is **the cajeta kitchen** — a warm family kitchen where the
caramel is made. The site is built from its materials: a clay **olla**, caramel,
**comal & cast iron**, **wood**, **terracotta**, kraft — *family, comfort, hand-made*.
Textures are **subtle and tasteful** (low-opacity grain overlays + a few real
material photographs for heroes), layered over a modern, legible grid — **crafted,
not kitschy**.

| Material / texture | Feeling | Where it's used |
|--------------------|---------|-----------------|
| **Terracotta / clay pottery** (the olla) | earthen, hand-thrown, warm | the **Olla registry** section *literally*; section backgrounds, matte-ceramic card surfaces, the terracotta accent |
| **Caramel (glossy amber)** | sweet, comfort, the hero | the cube logo, primary CTAs/links, glossy highlights, "pour" accents |
| **Worn cast iron / kitchen** (comal, cookware) | hearth, family kitchen, worn-in | dark sections, footer, code blocks (where code "cooks"), scrolled nav — weathered near-black metal (`comal-900`) |
| **Wood (organic grain)** | natural, the family table | panels, dividers, card frames, the "surface" sections rest on |
| **Kraft / paper** | honest, readable | the cream canvas + docs pages (paper = legible), labels/badges |
| **Grain / imperfection (organic)** | hand-made, family, comfort | low-opacity noise/grain overlay site-wide; slightly irregular edges; soft warm shadows |

Implementation notes:
- Prefer **CSS + subtle overlays** (noise PNG/SVG at 3–6% opacity, `mix-blend`,
  warm-tinted shadows) plus a few **real material photos** (clay, wood, caramel) for
  hero/section backgrounds — kept light (WebP/AVIF, lazy-loaded) so the site stays fast.
- Texture is **accent, not wallpaper**: body type sits on clean cream/paper for
  readability; reserve heavy material for hero, section breaks, the registry, the footer.
- A **gold/foil accent** (`gold-400`) for premium moments only — seals, the logo
  lockup, "release" badges.
- **Sourcing:** curated CC0 / free-commercial assets per material in
  `plans/cajeta-texture-board.md` (Poly Haven, ambientCG, TextureCan, Unsplash) +
  performance budget and the Blender "kitchen" hero plan.

### 3.2 Typography

- **Display / headings:** **Fraunces** — a soft, warm, optical serif that reads
  "artisan / confection" and pairs naturally with caramel. Use the "Soft" /
  high-optical-size axis for big hero text.
- **Body / UI:** **Inter** (or **Geist Sans**) — clean, neutral, excellent at
  small sizes. Pick one; default **Inter**.
- **Code / mono:** **JetBrains Mono** (or **Geist Mono**) for all `.cajeta`
  snippets and CLI blocks.

All self-hosted via `next/font/google` (or `@fontsource`) so there's no runtime
FOUT and no third-party request — keeps the static-export path clean.

Type scale: fluid `clamp()` based scale; generous line-height on body (1.6),
tight on display (1.05). Tracking slightly negative on large Fraunces headings.

### 3.3 Logo / icon — glossy caramel cube

A perfect cube of caramel, glistening, no wrapper. Delivered now as a
**hand-built SVG** (browns gradient + glossy highlight), usable immediately as
favicon + header mark; swappable later for a photoreal Blender render or
photograph (asset spec in §11).

The logo is delivered as a standalone file — **`plans/assets/cajeta-cube.svg`** (the
canonical source of truth; moves to **`site/public/cajeta-cube.svg`** when the
site is scaffolded), referenced as the favicon + nav logo. It's an isometric
cube: lightest glazed top face, mid-tone left face, darker right face, a white
specular highlight + glint, an ambient glow, and a soft contact shadow. Open it
in a browser to preview; swap for a photoreal render/photo later (asset spec
§11). The markup lives in that file (kept here only as an initial reference below).

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" role="img" aria-label="Cajeta caramel cube">
  <defs>
    <linearGradient id="cj-top" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#FFE6B8"/>
      <stop offset="0.5" stop-color="#E0A85B"/>
      <stop offset="1" stop-color="#C68A3E"/>
    </linearGradient>
    <linearGradient id="cj-left" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#B97A2E"/>
      <stop offset="1" stop-color="#7E4F1F"/>
    </linearGradient>
    <linearGradient id="cj-right" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#8A5A22"/>
      <stop offset="1" stop-color="#5A3617"/>
    </linearGradient>
    <radialGradient id="cj-glow" cx="0.5" cy="0.45" r="0.6">
      <stop offset="0" stop-color="#FFE6B8" stop-opacity="0.9"/>
      <stop offset="1" stop-color="#FFE6B8" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="cj-shadow" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#2A1A0F" stop-opacity="0.45"/>
      <stop offset="1" stop-color="#2A1A0F" stop-opacity="0"/>
    </radialGradient>
  </defs>

  <!-- ambient glow -->
  <rect x="0" y="0" width="128" height="128" fill="url(#cj-glow)"/>
  <!-- contact shadow -->
  <ellipse cx="64" cy="112" rx="40" ry="9" fill="url(#cj-shadow)"/>

  <!-- cube faces (isometric) -->
  <!-- top -->
  <polygon points="64,20 104,42 64,64 24,42" fill="url(#cj-top)"/>
  <!-- left -->
  <polygon points="24,42 64,64 64,104 24,82" fill="url(#cj-left)"/>
  <!-- right -->
  <polygon points="104,42 64,64 64,104 104,82" fill="url(#cj-right)"/>

  <!-- edge sheen -->
  <polyline points="24,42 64,64 104,42" fill="none" stroke="#FFE6B8" stroke-opacity="0.5" stroke-width="1.5"/>
  <!-- glossy specular highlight on the top face -->
  <ellipse cx="58" cy="38" rx="13" ry="6" fill="#FFFFFF" fill-opacity="0.55" transform="rotate(-26 58 38)"/>
  <!-- small secondary glint -->
  <circle cx="80" cy="46" r="2.6" fill="#FFFFFF" fill-opacity="0.7"/>
</svg>
```

Logo lockup: cube mark + wordmark **"Cajeta"** in Fraunces. Provide a
monochrome variant for tight spaces and an `apple-touch-icon` / maskable PNG
export at build (or via the asset spec in §11).

### 3.4 Look & feel
- **Organic, hand-made, hearth-warm** — driven by the texture system (§3.1.1).
  Rounded, slightly-imperfect cards on cream/paper; soft warm shadows (tinted
  `comal`/`espresso`, never pure black); generous whitespace; a low-opacity grain
  overlay site-wide. Material moments (terracotta, wood, worn iron, caramel) at the
  hero, section breaks, and the registry. Restrained, weighty motion (fade/slide-in,
  a slow caramel-gloss shimmer on the logo). Dark mode = **worn-iron / comal**
  surfaces with caramel + gold accents (the kitchen after dark). Family-table comfort —
  crafted, never kitschy.

---

## 4. Information architecture

```
/                     What is Cajeta  (landing / hero)
/why                  Why Cajeta      (motivation, comparisons)
/examples             Code Examples   (curated, highlighted snippets)
/docs/**              Full Documentation (Fumadocs over cajeta-docs/)
/repo                 Cajeta Repo     (package registry front door)
/contact              Contact         (Julian)
```

Global chrome:
- **Header:** cube logo + wordmark, primary nav (What · Why · Examples · Docs ·
  Repo · Contact), GitHub link, theme toggle, "Get Started" CTA, command-palette
  (⌘K) search hook into docs.
- **Footer:** brief blurb, nav columns (Product / Docs / Community), version
  badge (`0.5.1` from `VERSION`), license, social/contact, "made with caramel".

---

## 5. Page-by-page content

Content below is drawn from real project sources: `README.md`, `Features.md`,
`samples/Tour/`, `cajeta-docs/`, and `cajeta-docs/specs/`.

### 5.1 `/` — What is Cajeta (landing)
- **Hero:** caramel-cube logo with glow; headline + the README elevator pitch:
  > *"A hybrid systems / application language combining C++-style true
  > templates, multiple inheritance, and operator overloading; Java-style class
  > semantics; and Rust-inspired ownership — with a single explicit allocation
  > idiom that lets the caller pick stack or heap at initialization."*
  Sub-line: *"Comfort, with true zero-copy Views, a fast networking stack, and
  memory management free of reference counting."*
  Primary CTA → **Get Started** (`/docs`), secondary → **View on GitHub**.
- **At-a-glance strip:** LLVM 22 · stack/heap allocation · `#`-transfer ownership
  · true templates · multiple inheritance · XPU (CPU/CUDA/AMD/Vulkan) · 1717
  tests / 32 shards.
- **Tagline card:** "Cajeta — Spanish for caramel. A language that won't fight you."
- **Three pillars:** *Familiar* (C#/Java/TS feel at home) · *Fast* (zero-copy
  Views, no refcounting, LLVM-native) · *Safe-ish* (compile-time borrow checker
  without Rust's friction).
- **Code teaser:** the allocation idiom (`stack` / `heap`) snippet (§5.3).
- **Feature grid** (icon cards) → deep links into `/docs`.
- **Status banner:** "Compiler in active development; language design past v1."
- Closing CTA band on the molten-caramel gradient.

### 5.2 `/why` — Why Cajeta
- **The thesis:** comfort + performance without GC or refcounting; a memory
  model that "won't fight you" (quote README).
- **Differentiators** (expanded cards):
  - *Explicit allocation at the use site* — `stack Point(3,4)` vs `heap Point(5,12)`;
    same type, storage is metadata. No `new` keyword.
  - *Single ownership + static borrow checker* — `#`-transfer; use-after-move is
    a compile error (`CAJETA_ERROR_USE_AFTER_MOVE`).
  - *Zero reference counting* — heap blocks are plain memory; drop chain frees in
    reverse declaration order at scope exit.
  - *True templates (monomorphization)* — not type-erased generics.
  - *Multiple inheritance for behavior*, single inheritance for state; vtable at offset 0.
  - *XPU portability* — one `@Kernel` source → CUDA / HIP / Vulkan / CPU, with
    graceful CPU degradation (cite `cajeta-xpu-matrix.md`).
  - *Structured concurrency* — `async`/`await`/`scope`/`spawn`, stackful fibers.
  - *Compiler that teaches* — abundant linting, verbose errors, AI-friendly.
- **Comparison table** — Cajeta vs **Rust** (friendlier, less borrow-checker
  fighting; trades some guarantees) vs **C++** (memory model + safer templates)
  vs **Java** (no GC, native, finer control). Keep it honest (README is candid:
  *"doesn't have the 'safety' of Rust, but…"*).
- CTA → `/examples` and `/docs`.

### 5.3 `/examples` — Code Examples
Curated, highlighted, copy-buttoned snippets pulled from `samples/Tour/`. Group
with tabs / anchored sections. Confirmed-real snippets:

- **Hello World** (`samples/HelloWorld/.../HelloWorld.cajeta`)
- **Allocation** — `stack`/`heap` (`AllocationDemo.cajeta`)
- **Ownership transfer** — `#owner` (`OwnershipDemo.cajeta`)
- **Templates / monomorphization** — `Box<T>`, streams reduce (`TemplatesDemo.cajeta`)
- **Streams + lambdas** — `filter`/`reduce` (`StreamsDemo.cajeta`)
- **JSON Tier-1 codec** — `Json.toBytes<User>` (`JsonDemo.cajeta`)
- **Async / structured concurrency** — `async`, `scope`, `spawn` (`AsyncDemo.cajeta`)

Each block: filename caption, Cajeta-highlighted code, "copy", and (where the
Tour README has it) expected output. A "See all 40+ samples on GitHub" link.
Build-time approach: read the actual sample files so snippets never drift
(script that imports from `../samples/Tour/...` into MDX, or a small loader).

### 5.4 `/docs/**` — Full Documentation (Fumadocs)
- Source: the `cajeta-docs/` tree (`stdlib/`, top-level specs, `specs/`).
- Sidebar groups (from real files):
  - **Language:** `Lang.md`, `UnifiedClasses.md`, `MemoryModel.md`,
    `FieldOwnership.md`, `Primitives.md`, `FloatingPointModel.md`,
    `OperatorOverloading.md`.
  - **Types & generics:** `TemplateWildcard.md`, `CaptureConversion.md`,
    `MethodLevelTemplate.md`, `MultiClassing.md`.
  - **Collections & streams:** `Collections.md`, `Streams.md`,
    `StreamParallelism.md`, `Hashing.md`.
  - **Features:** `Lambdas.md`, `Annotations.md`, `AspectModel.md`,
    `ErrorModel.md`, `Views.md`.
  - **Concurrency:** `Thread.md`, `AsyncStatus.md`.
  - **Codec / IO:** `codec/Json.md`, `io/`.
  - **Compiler & build:** `Compilation.md`, `CompilerModes.md`, `BuildTool.md`,
    `LintRules.md`, `HarnessDesign.md`, `ArchiveManagement.md`, `Embedded.md`.
  - **XPU:** `../cajeta-xpu.md`, `../cajeta-cpu.md`, `../cajeta-vulkan.md`,
    `../cajeta-amd.md`, `../cajeta-xpu-matrix.md`.
  - **Specs (v1):** `specs/repository-protocol-v1.md`, `manifest-v1.json`,
    `lockfile-v1.json`, `action-catalog-v1.json`, `capabilities-v1.json`,
    `extension-api-v1.md`, `toolchain-registry-v1.md`, `schema-versioning.md`,
    `tour-build-your-first-package.md`.
- **Single source of truth = `../cajeta-docs/`** (canonical, current). The site
  is a pure *consumer*; we never hand-copy/author doc bodies in `site/`. The
  sync + auto-reflect mechanism and structure-preservation are specified in §14.
- Features: full-text search (Orama), TOC, "edit on GitHub", prev/next,
  versioned-docs later. Recolor `fumadocs-ui` to the caramel palette (this is
  what replaces the old generic-`prose` "raw md" look).

### 5.5 `/repo` — Cajeta Repo (package registry front door)
The Maven-Central-style registry. Design reflects the **real** protocol in
`cajeta-docs/specs/repository-protocol-v1.md` + `tour-build-your-first-package.md`.

- **Hero:** "The Cajeta package registry" — search bar front-and-center
  (`repo.cajeta.org`). Tagline: signed, content-addressed, reproducible.
- **Search & browse** *(depends on registry search API — see §8 / open question)*:
  package search box, category/keyword filters, results cards (name, latest
  version, description, license, capabilities, retracted/CVE badge).
- **Featured / well-known bundles:** `stdlib@1.0.0` (from the capability probe's
  `well-known-bundles`), core modules: `cajeta.lang`, `cajeta.collection`,
  `cajeta.codec.json`, `cajeta.threading`, `cajeta.xpu`.
- **How it works** (illustrated, from the protocol):
  - *Resolve & bundle* — `POST /v2/bundle` with `have`/`want` digest
    negotiation; transitive MVS pinning; `tar.zst` stream.
  - *Content-addressed* — `GET /v2/blob/<sha256>`; cache keys match by construction.
  - *Signed & attested* — ed25519 detached `.sig`, SLSA v1 provenance,
    transparency log (`/v2/transparency-log/<sha256>`).
  - *Mirrors & capability probe* — `/.well-known/cajeta-capabilities.json`,
    regional mirrors, graceful v1↔v2 fallback.
  - *Publish* — `POST /v2/publish` (multipart: archive + signature + key-id +
    attestation + metadata) with DNS/`.github` namespace verification.
- **Quickstart cards** (from the package tour): `cajeta init` → `cajeta build` →
  `cajeta add <dep>` → `cajeta release` (signed publish to `/v2/publish`).
  Consumer side: `cajeta install foo-1.2.3.cja --require-signature --require-attestation`.
- **Driver matrix** table: `filesystem` · `http` (bearer/mTLS) · `git` ·
  `maven-compat` (deferred).
- **"Host your own registry"** CTA → `/docs/specs/repository-protocol-v1`.
- **NOTE:** if a richer registry web-app / search design arrives from another
  machine (see §8), this page upgrades from "front door + explainer" to a true
  searchable catalog UI. Build it so the search section is a drop-in slot.

### 5.6 `/contact` — Contact
- Heading + short blurb (Julian Klappenbach, creator of Cajeta).
- **Contact form:** name, email, subject, message (shadcn `Form` +
  `react-hook-form` + `zod`); submit via Formspree or Resend serverless route
  (§7.6) — choice depends on hosting.
- Direct links: email `jklappenbach@gmail.com`, GitHub, (optional) socials.
- "Contributing" callout → repo + issues. Honeypot/anti-spam on the form.

---

## 6. Component & module inventory

### 6.1 Layout / chrome
`SiteHeader`, `MainNav`, `MobileNav` (Radix `Sheet`), `ThemeToggle`,
`CommandMenu` (⌘K), `SiteFooter`, `VersionBadge`.

### 6.2 Marketing primitives
`Hero`, `CaramelCube` (animated SVG logo w/ glow), `PillarCard`, `FeatureGrid`,
`ComparisonTable`, `CTABand`, `StatStrip`, `SectionHeading`, `GradientPanel`,
`NoiseOverlay`.

### 6.3 Content primitives
`CodeBlock` (Shiki + copy button + filename + optional output), `Tabs`,
`Callout`, `Badge`, `Card`, `Table`, `Accordion`, `Tooltip`, `Steps`
(quickstart) — most from shadcn/ui, Fumadocs supplies MDX-side equivalents.

### 6.4 Cajeta syntax highlighting
- Cajeta isn't a built-in Shiki language. Options:
  - **(A)** Author a minimal TextMate grammar for Cajeta (keywords: `package`,
    `class`, `final`, `public/private`, `static`, `stack`, `heap`, `async`,
    `await`, `scope`, `spawn`, primitive types `int32/int64/float64`, the `#`
    transfer operator, `@Annotations`, generics `<...>`). Register it with Shiki.
    Reuse work from **`cajeta-prism`** (the project's existing syntax-highlight
    effort) if its grammar is portable.
  - **(B)** Interim: alias to **Java** or **Rust** grammar for "good enough"
    coloring until (A) lands.
- Caramel-tuned Shiki theme (two: light/dark) matching the palette.

### 6.5 Registry components
`PackageSearch` (slot), `PackageCard`, `DriverMatrix`, `ProtocolDiagram`,
`PublishFlow`, `BundleExplainer`.

---

## 7. Project structure (`./site`)

```
site/
  app/
    layout.tsx              # root: fonts, theme provider, header/footer
    page.tsx                # / What is Cajeta
    why/page.tsx
    examples/page.tsx
    repo/page.tsx
    contact/page.tsx
    docs/
      layout.tsx            # Fumadocs DocsLayout
      [[...slug]]/page.tsx  # Fumadocs page renderer
    api/contact/route.ts    # (server route — only if SSR hosting; else Formspree)
  content/
    docs/                   # MDX (generated/copied from ../cajeta-docs by prebuild)
  components/
    ui/                     # shadcn/ui
    marketing/  content/  registry/  layout/
  lib/
    samples.ts              # loads real snippets from ../samples/Tour at build
    cajeta-grammar.ts       # Shiki language registration
    site-config.ts          # nav, metadata, version (read ../VERSION)
  public/
    cajeta-cube.svg         # logo (markup in §3.3)
    favicon.ico  apple-touch-icon.png  og-image.png
  source.config.ts          # fumadocs-mdx config
  next.config.mjs           # (output:'export' toggle behind env)
  tailwind.config / globals.css   # @theme tokens (palette §3.1)
  package.json  tsconfig.json  .eslintrc  .prettierrc
  scripts/sync-docs.ts      # copy cajeta-docs -> content/docs + meta.json
  README.md                 # how to dev/build/deploy this site
```

### 7.6 Contact submission
- **Static hosting path:** Formspree (or Web3Forms) — no server. Form posts to
  their endpoint; env-config the form ID.
- **SSR hosting path:** `app/api/contact/route.ts` + **Resend** to email
  `jklappenbach@gmail.com`. Choose when hosting is decided (§10).

---

## 8. Search strategy

**Two separate, scoped searches — never merged.** One keyboard shortcut
(**Cmd/Ctrl+K**) opens a search panel whose target depends on where you are:
inside `/repo` and its children it searches the **registry** (library entries,
§8.2); everywhere else it searches the **static site content** via Orama (§8.1).
Same hotkey, context-routed to the correct index — the two never merge or
cross-query.

### 8.1 Site search — Orama static (over `cajeta-docs/`)
Active everywhere **except** the `/repo` section. Opened by **Cmd/Ctrl+K** (or a
visible search box). Scope: docs + static site content only — it does **not**
query the registry.
- **Fumadocs built-in static search (Orama).** Index is built at build time,
  shipped to the browser, runs **100% client-side** — **no backend, $0, works on
  any static host.** Ideal for the current ~50-file doc set.
- Wiring: Fumadocs `createFromSource` + a static `/api/search` route; the ⌘K
  command palette opens it.
- **Scale escape hatch:** if docs grow to hundreds of pages and the client index
  gets heavy, switch to **Algolia DocSearch** (free for qualifying open-source
  docs) or **Orama Cloud** — drop-in for Fumadocs, no UI rewrite.

### 8.2 Registry search — **dynamic, scoped to `/repo` and its children**
Lives **only** in the registry section: inside `/repo/**`, **Cmd/Ctrl+K**
launches this library-search panel (plus a visible search box) instead of the
site search. It never appears elsewhere, and the site search (§8.1) never
queries it. A continuously-growing catalog of *published packages*, not static MDX. The protocol provides `GET /v2/resolve?name=…` (constraint
resolve) + a per-package version index, but **no full-text catalog search
endpoint.**

> **Key constraint:** a build-time static index (8.1's approach) **cannot see
> libraries published after the last site build** — it goes stale the moment a
> new lib lands. Dynamic packages therefore require a **live index updated at
> publish time**, not a build-time snapshot.

- **Principle: registry search belongs to the registry backend.** A package
  index (Typesense / Meilisearch / Postgres FTS / Algolia) is *registry*
  infrastructure — the site only *presents* results. → This is the design to
  push from the other machine (§13.6).
- **Required registry-side additions (new protocol work):**
  1. **Index on publish.** `POST /v2/publish` upserts the package (name,
     version, description, keywords, author, README excerpt) into the search
     index; **yank/retract** updates or removes it. New libs become searchable
     within seconds — **no site rebuild needed.**
  2. **`GET /v2/search?q=…`** — full-text query endpoint (paged, returns name /
     version / description / score). This is what the site calls.
- **Search index — decided: Algolia** (typo tolerance by default; see §15.5.1).
  D1 + R2 stay the system of record; Algolia is a derived index updated on
  publish. Cost: **$0** via the Algolia for Open Source program (200k records +
  200k requests/mo). Escape hatch: self-hosted **Typesense/Meilisearch** behind
  the same `lib/registry` interface if metered cost ever bites.
- **`/repo` builds today:** explainer + quickstart + protocol + driver matrix
  are fully buildable now; the search box is a **pluggable slot** (`lib/registry.ts`)
  that calls `GET /v2/search` once it exists. Interim fallback: live exact-name
  lookup via `GET /v2/resolve` (client fetch to `repo.cajeta.org`, CORS) so
  "jump to package" works before full search lands. We never fake results.

### 8.3 One contextual hotkey (implementation)
- **Single shortcut, route-aware target.** Cmd/Ctrl+K is bound once, globally.
  A small dispatcher checks the current route and opens the matching panel:
  - inside `/repo/**` → `RegistrySearch` (live `GET /v2/search`), else
  - → `SiteSearch` (Orama static).
  There is exactly **one** search hotkey site-wide; only *what it searches*
  changes by context. No second/alternate key.
- **Distinct panels behind it:** `SiteSearch` and `RegistrySearch` are separate
  components with separate clients (`lib/search-docs.ts` vs `lib/registry.ts`) —
  no shared index, no result merging. The dispatcher (`lib/search.ts`) just
  picks which to mount based on `usePathname()`.
- This keeps docs search **$0, offline, zero-dependency** and decoupled from
  registry-backend availability, while package search stays self-contained to
  the section that owns that data — all under one consistent keypress.

---

## 9. Content provenance (so nothing drifts)

- Snippets: load from `../samples/Tour/**` and `../samples/HelloWorld/**` at
  build (don't hand-copy).
- Docs: sync from `../cajeta-docs/**` via `scripts/sync-docs.ts` (Approach A, §5.4).
- Version: read `../VERSION` (currently `0.5.1`) into `site-config.ts` for the
  footer/badge. **`VERSION` is law** — the site always reflects it and never
  hard-codes a version. (README prose still says `0.1.0`; that's stale — correct
  it in the repo as a follow-up.)
- Pitch/feature copy: quote `README.md` + `Features.md` verbatim where possible.

---

## 10. Hosting & deployment — cheapest options

Build is **static-export-capable** (`output: 'export'`), so the **free tier of
every static host applies**. The only guaranteed recurring cost is the
**domain** (~$12/yr, e.g. `cajeta.org`).

| Host | Cost | Free serverless (form / search proxy) | Notes |
|------|------|----------------------------------------|-------|
| **GitHub Pages** | **$0** | ❌ none | Simplest for an OSS repo already on GitHub; free Actions builds, TLS, custom domain. No functions → Contact form via Formspree/Web3Forms free tier. Mind project-path `basePath` unless using a custom domain. |
| **Cloudflare Pages** | **$0** | ✅ Pages Functions (100k req/day) | **Top pick.** Best CDN, unlimited bandwidth, 500 builds/mo, free TLS/domains. Room for Contact form *and* a search proxy on the free tier. Next.js via static export or `@cloudflare/next-on-pages`. |
| **Netlify** | **$0** | ✅ Functions + **Netlify Forms** (100 subs/mo) | Zero-code Contact form via Netlify Forms; 100GB/mo, 300 build-min/mo. Old Astro site already had `netlify.toml`. |
| **Vercel** | **$0 (Hobby)** | ✅ Functions + ISR | Best Next.js SSR/ISR DX. Hobby tier is non-commercial — fine for personal/OSS; revisit if monetized. 100GB/mo. |

**Recommendation**
- **Cheapest + simplest for OSS:** **GitHub Pages** (pure static, $0) +
  **Formspree/Web3Forms** (free) for the Contact form.
- **Cheapest with headroom** (form + future search proxy on one platform, best
  CDN): **Cloudflare Pages** ($0). ← recommended default.
- Either way the site stays **static-first**; pick SSR (Vercel/Cloudflare/Netlify
  functions) only when Contact or a registry proxy needs it — all on free tiers.

Keep regardless: `next-sitemap`, OpenGraph image, robots.txt, canonical URLs for
SEO; provide static `out/` + optional `Dockerfile` instructions in `site/README.md`.

---

## 11. Asset spec — caramel-cube logo

- **Now:** `plans/assets/cajeta-cube.svg` (§3.3) — favicon + nav mark (placeholder).
- **In progress (Blender):** three photoreal render proposals matching the
  caramel reference — `plans/assets/cajeta-cube-{1,2,3}.png` (1024², transparent),
  scenes `P1_Studio`/`P2_Softbox`/`P3_Dramatic` in `plans/assets/cajeta-cube.blend`.
  See `plans/cajeta-cube-proposals.md`. Pick one → export favicon/OG sizes and
  replace the SVG placeholder.
- **Future hero idea:** an animated **rotating 3D caramel cube** in the landing
  hero that **reflects/refracts the page** — the web banner/background showing
  *through* and *on* the glossy clearcoat (real-time via Three.js/R3F with an
  env-map of the page, or a pre-rendered rotation GIF/webm with the banner in
  the Blender scene as the environment). Captures the wet-caramel gloss in motion.
- **Upgrade later** (photoreal render / photograph), provide:
  - `og-image.png` 1200×630 (cube on cream gradient + wordmark).
  - `apple-touch-icon.png` 180×180, `favicon.ico` (16/32/48), maskable
    `icon-512.png` with safe-zone padding.
  - Optional `cube-hero.webp` (large, glossy, for the landing hero).
  - If rendered in Blender: studio HDRI, subsurface-scattering caramel material,
    specular glaze, soft contact shadow, transparent background; export PNG @2x.

---

## 12. Build phases (when scope expands beyond plan)

1. **Scaffold** — `create-next-app` (TS, App Router, Tailwind) under `site/`;
   add shadcn/ui, Fumadocs, fonts; commit baseline; drop in `plans/assets/cajeta-cube.svg`.
2. **Design system** — palette tokens, typography, theme provider, dark mode,
   header/footer, logo component. One styled placeholder page to lock the look.
3. **Marketing pages** — `/` (What), `/why`, with real copy + feature grid + CTAs.
4. **Examples** — sample loader + `CodeBlock` + Cajeta highlighting (interim
   Java/Rust alias, then real grammar).
5. **Docs migration** (§14) — Fumadocs wiring, `sync-docs.ts` sourcing
   canonical `cajeta-docs/`, `docs-manifest.ts` taxonomy, caramel recolor,
   search, grouped `/docs` index. Verify auto-reflect.
6. **Repo** — protocol explainer, quickstart, driver matrix, search shell (§8).
7. **Contact** — form + submission path (per hosting).
8. **Polish** — SEO/OG, motion, a11y pass (keyboard/contrast), Lighthouse,
   responsive QA, `site/README.md`.
9. **Retire `cajeta-docs-site/`** (§14.4) — reconcile old-only content into
   `cajeta-docs/`, add redirects, then `git rm -r cajeta-docs-site/` on a branch.
10. **Deploy** — finalize hosting choice, CI build, sitemap/robots.

---

## 13. Open questions / decisions for Julian

1. **Registry search API** — is there a catalog/search design on the other
   machine? (Blocks full `/repo` search; see §8.2.) Needs two additions to the
   protocol: **index-on-publish** (upsert in `POST /v2/publish`, remove on
   yank) and **`GET /v2/search?q=`**. → *you offered to push it.*
2. **Hosting target** (§10) — recommended default **Cloudflare Pages** ($0, free
   Functions for form/search proxy) or **GitHub Pages** ($0, pure static +
   Formspree). Confirm the choice; it fixes the Contact form path (§7.6) and the
   registry-search proxy option (§8.2).
3. **Version** — ✅ **resolved:** the **`VERSION` file (`0.5.1`) is law**; the
   README prose `0.1.0` is stale. The site reads the version from `VERSION`
   (§9) and never hard-codes one. Follow-up: correct the README prose in the repo.
4. **Domain** — ✅ **resolved (§15.2):** canonical root **`cajeta.dev`** (owned),
   site at apex, registry at **`olla.cajeta.dev`**, DNS via Cloudflare; specs'
   `repo.cajeta.org` → `olla.cajeta.dev`. (Still optional: split docs to
   `docs.cajeta.dev` vs keep at `/docs`.)
5. **Logo** — keep SVG, or commission a Blender/photo render later? (§11)
6. **Stale docs taxonomy** — the old Astro `category` values (`CajetaTorch`,
   `CajetaML`, `StandardLibrary`, `Structs`…) reference docs that no longer
   exist as such in `cajeta-docs/`. Confirm the new category→file mapping in
   §14.2 (built from the *current* `cajeta-docs/` tree).
7. **Contact channels** — beyond email/GitHub, any socials / Discord to list?
8. **GitHub URL** — old site links `github.com/jklappenbach/cajeta`; the active
   repo is `cajeta-two`. Which is the public canonical repo for links?

---

## 14. Docs migration & retirement of `cajeta-docs-site/`

Goal: kill the "raw md" look, **keep the structure**, **keep an automatic
md→styled-html pipeline**, make doc edits **auto-reflect**, and **delete the old
Astro site** once `./site` reaches parity.

### 14.1 What the old site actually does (so we preserve the good parts)
- Docs are **`src/pages/docs/*.md`** — Astro auto-routes `.md` in `pages/`.
- Each file has front-matter: `title`, `category`, `description`, and
  `layout: '~/layouts/MarkdownLayout.astro'`.
- `MarkdownLayout.astro` wraps the body in **generic Tailwind `prose`**
  (blue links, default type) — *this is the unstyled "raw md" look to replace.*
- `pages/docs/index.astro` auto-discovers via `import.meta.glob('./*.md')` and
  **groups by `category`** in a fixed order:
  **`Language → Stdlib → Tooling → Process → Status → Reference`**
  (each category has a tagline/subtitle/icon). **← keep this taxonomy.**
- `navigation.ts` defines header/footer doc menus per category.
- **Problem:** these `.md` are **hand-copied & stale** — divergent filenames
  (`CajetaTorch`, `CajetaML`, `StandardLibrary`, `Structs`, `CajetaHttp`,
  `ImplementationStatus`…) that don't match the live `cajeta-docs/` tree. Editing
  a real doc in `cajeta-docs/` does **not** update the site today.

### 14.2 New docs pipeline in `./site` (Fumadocs, single source of truth)

**Source of truth:** `../cajeta-docs/**` only. Never copy bodies by hand.

**Auto-reflect mechanism (keeps "md → styled html, automatically"):**
- **Dev:** `fumadocs-mdx` source dir points at the synced content; a chokidar
  watcher (or symlink, below) makes saving a `cajeta-docs/*.md` hot-reload the
  page. Two viable wirings:
  - **(A) Symlink** `site/content/docs` → `../cajeta-docs` (zero copy; edits are
    literally the same files). Simplest auto-reflect; caveat: Fumadocs wants a
    `meta.json` sidebar + per-file front-matter (title/order) which canonical
    `.md` lack — solved by the manifest in the next bullet rather than editing
    canonical files.
  - **(B) `scripts/sync-docs.ts` prebuild + `--watch`** copies `cajeta-docs/**`
    → `site/content/docs/**`, and **injects** front-matter (derive `title` from
    the first `# H1`, `category`/order from the manifest) so canonical `.md`
    stays clean. **Recommended** — keeps the repo docs annotation-free and gives
    Fumadocs what it needs. CI/build runs it; dev runs it in `--watch`.
- **Build:** the same sync runs as a `prebuild` step, so production always
  matches the committed `cajeta-docs/`.

**Structure preservation:** a single `site/lib/docs-manifest.ts` reproduces the
old taxonomy against the *current* tree — e.g.:
| Category | Current `cajeta-docs/` files (illustrative) |
|----------|---------------------------------------------|
| Language | `stdlib/Lang.md`, `stdlib/UnifiedClasses.md`, `stdlib/MemoryModel.md`, `stdlib/FieldOwnership.md`, `stdlib/Primitives.md`, `stdlib/FloatingPointModel.md`, `OperatorOverloading.md`, `stdlib/Views.md`, `stdlib/Lambdas.md`, `stdlib/ErrorModel.md` |
| Stdlib   | `stdlib/Collections.md`, `stdlib/Streams.md`, `stdlib/StreamParallelism.md`, `stdlib/Hashing.md`, `stdlib/codec/Json.md`, `stdlib/Annotations.md`, `stdlib/AspectModel.md` |
| Concurrency | `stdlib/Thread.md`, `stdlib/AsyncStatus.md` |
| Tooling  | `Compilation.md`, `CompilerModes.md`, `BuildTool.md`, `LintRules.md`, `HarnessDesign.md`, `ArchiveManagement.md` |
| XPU      | `../cajeta-xpu.md`, `../cajeta-cpu.md`, `../cajeta-vulkan.md`, `../cajeta-amd.md`, `../cajeta-xpu-matrix.md` |
| Specs    | `specs/*` (repository-protocol, manifest, lockfile, action-catalog, capabilities, extension-api, toolchain-registry, schema-versioning, tour) |
| Status   | `Features.md`, `Embedded.md` |

The manifest is the one place to curate categories/order/titles → emitted as
Fumadocs `meta.json` files by the sync script. (Final mapping = open question §13.6.)

**Styling (replaces generic `prose`):** Fumadocs `DocsLayout` + MDX components
recolored to the caramel palette (§3.1): styled headings (Fraunces), caramel
links, callouts, tables, Shiki code blocks with the Cajeta grammar (§6.4),
sidebar tree, TOC, breadcrumb, prev/next, and Orama search. A docs landing
(`/docs`) reproduces the old grouped index (category cards w/ taglines).

### 14.3 Salvage list (pull from old site before deleting)
- Category taxonomy + per-category taglines/subtitles/icons (`docs/index.astro`).
- Branding bits in `src/config.yaml` (site name, metadata, theme defaults) and
  `src/assets/favicons/` (compare with new caramel favicon — likely replace).
- Any doc prose that exists **only** in the old copies and not in `cajeta-docs/`
  → reconcile into `cajeta-docs/` first (don't lose content). Diff the two sets
  during migration; the old set may contain notes worth porting back to canon.
- Contact/About copy from `src/pages/contact.astro` / `about.astro` if useful.

### 14.4 Retirement steps (order matters — delete last)
1. Build `./site` docs to **parity**: every doc reachable, categorized, styled,
   searchable; links updated; redirects for any old `/docs/<Name>` URLs that
   should survive (map to new slugs).
2. Reconcile any old-only content back into `cajeta-docs/` (§14.3).
3. Verify auto-reflect: edit a `cajeta-docs/*.md`, confirm the dev site updates.
4. **`git rm -r cajeta-docs-site/`** and remove its references (CI, Netlify/
   Vercel configs, any links). Commit on a branch; PR for review.
5. Update root README / links to point at `./site`.

> Deletion is destructive and the old site holds the only copy of some content
> (taxonomy, possibly drifted doc text). Steps 1–3 must complete and be verified
> before step 4. The user has authorized removal ("get rid of cajeta-docs-site").

---

## 15. Registry "Olla" — moved to its own repo

The Olla registry **service + infrastructure** plan now lives in its **own repo**
(per §1.1): **https://github.com/jklappenbach/cajeta-olla** →
`plans/olla-infrastructure-plan.md`. It covers name/boundaries, the
`cajeta.dev` domain scheme, Cloudflare setup, R2 artifacts, the D1 catalog,
Algolia search, and cost.

**What stays in *this* (`cajeta`) repo:**
- The wire **spec** — `cajeta-docs/specs/` (`repository-protocol-v1.md`,
  `manifest-v1.json`, `lockfile-v1.json`). Olla consumes it and conformance-tests
  against it.
- The build-tool **client** — `src/cajeta/buildtool/repo/`.
- The **`/repo` front-door page** and registry-search wiring in the site — §5.5,
  §8.2, §8.3.
- **Spec/protocol follow-ups** the registry needs: index-on-publish + `GET
  /v2/search?q=` (§8.2 / §13), and the `repo.cajeta.org` → `olla.cajeta.dev`
  endpoint rename across `cajeta-docs/specs/*` and the build-tool default.
