# Markdown JCEF render surface — user verification checklist

**Status:** experimental, opt-in. The default `swing` surface is unchanged and
not under test here. This drives verification of the new **`jcef`** (Chromium)
in-comment markdown rendering surface — the part I **cannot** validate headless
(JCEF is disabled in CI/headless and the off-screen capture is async/native).

Work top-to-bottom. Each item has a **PASS/FAIL/notes** line — fill it in and
hand the file back to me to drive the next iteration.

Commit under test: `84bd3cc1` (branch `main`).

---

## 0. Build & launch

Two ways. **runIde** (fast, IDEA sandbox) is recommended for iteration; the
**install-in-CLion** path verifies the real target.

### Option A — runIde sandbox
```
cd /home/julian/code/cpp/cajeta-five/ide-plugins/idea
./gradlew runIde
```
- [ ] Sandbox IDE launches. — **PASS / FAIL:**

### Option B — install the built zip in CLion
Artifact: `/home/julian/code/cpp/cajeta-five/ide-plugins/idea/build/distributions/cajeta-idea-0.1.0.zip`
(rebuild with `./gradlew -p ide-plugins/idea buildPlugin` if stale).
CLion → Settings → Plugins → ⚙ → *Install Plugin from Disk…* → pick the zip → restart.
- [ ] Plugin installs & CLion restarts clean. — **PASS / FAIL:**

---

## 1. Turn on the JCEF surface

1. Open a project containing `.cajeta` files (this repo works — e.g. open
   `/home/julian/code/cpp/cajeta-five`).
2. Settings → Languages & Frameworks → **Cajeta**.
3. Confirm **"Render markdown in comments (Obsidian-style)"** is checked.
4. Set **"Markdown render surface"** → **`jcef`** → Apply/OK.
5. **Open (or reopen) a `.cajeta` file.** Fold regions are built at file-open, so
   after switching the surface you must reopen the file (or close+reopen the
   editor tab) for the new renderer to take effect.

- [ ] The `jcef` option is present in the dropdown and persists after OK. — **PASS / FAIL:**

> Good test file — paste this into a scratch `Demo.cajeta` (whole-line comments
> are what get folded+rendered):
> ```
> // # Heading one
> // Some **bold**, _italic_, and `inline code`.
> //
> // | col A | col B |
> // |-------|-------|
> // | 1     | two   |
> //
> // ```
> // fenced code block
> // ```
> //
> // > a blockquote
> //
> // - bullet one
> // - bullet two
> class Demo {}
> ```

---

## 2. Core rendering (the whole point)

With the surface = `jcef` and the file (re)opened, the comment block should
collapse into a rendered card.

- [ ] **Renders at all** (not blank, not raw text, no exception banner). — **PASS / FAIL:**
- [ ] **Table** has borders + a shaded header row (this is the big win over Swing). — **PASS / FAIL:**
- [ ] **Fenced code block** has a background + border; monospaced. — **PASS / FAIL:**
- [ ] **Blockquote** shows a left accent bar. — **PASS / FAIL:**
- [ ] **Headings / bold / italic / inline code / lists** look correct. — **PASS / FAIL:**
- [ ] **Background matches the editor** (no white box on a dark theme). — **PASS / FAIL:**
- [ ] **Side-by-side:** switch surface back to `swing`, reopen — is `jcef`
      clearly better? Worth keeping? — **PASS / FAIL / verdict:**

---

## 3. The parts I expect to be wrong (highest-value feedback)

These are the async/native bits I couldn't test — please look closely.

- [ ] **First-paint flow:** on open, does it briefly show the Swing fallback and
      then swap to the JCEF image, or does it stay on the fallback forever
      (JCEF never arrives)? — **PASS / FALLBACK-STUCK / notes:**
- [ ] **Height accuracy:** is the card the right height, or is content
      **clipped** at the bottom / is there **extra blank space**? (Height comes
      from `document.body.scrollHeight` — clipping = measurement/timing bug.)
      — **OK / CLIPPED / EXTRA-SPACE / notes:**
- [ ] **HiDPI scaling:** on a scaled display (e.g. 150%/200%), is the image
      **crisp** or **blurry / half-size / double-size**? (Capture ignores device
      scale today — likely needs a fix.) Note your display scale.
      — **CRISP / BLURRY / WRONG-SIZE / scale = ___ :**
- [ ] **Off-screen window flash:** does a stray window/flicker appear anywhere on
      screen when a file opens? (The OSR browser is hosted in a window parked at
      -32000,-32000 — should be invisible.) — **NO-FLASH / FLASH / notes:**
- [ ] **Many blocks / big file:** open a file with several comment blocks. All
      render? Any slowness, beachball, or only the first one renders?
      — **PASS / notes:**
- [ ] **Edit a comment** (expand by clicking in, change text, click away). Does
      it re-render with the new content? — **PASS / FAIL / notes:**
- [ ] **Theme switch:** change IDE theme (dark↔light) and reopen the file — do
      colors/background follow? — **PASS / FAIL / notes:**

---

## 4. Stability & resource use

- [ ] **No leaked browsers:** open/close several `.cajeta` files, scroll around.
      Check Help → *Diagnostic Tools* → *Activity Monitor* (or `ps aux | grep
      jcef_helper`): does the `jcef_helper` process count **stay bounded** (not
      growing per fold/file)? The design renders-once-then-disposes, so it should.
      — **BOUNDED / GROWING / count notes:**
- [ ] **No EDT freezes / "not responding".** — **PASS / FAIL:**
- [ ] **Toggling surface back to `swing`** + reopen → returns to the Swing
      rendering with no errors. — **PASS / FAIL:**

---

## 5. Diagnostics to capture if anything fails

- Log file: **Help → Show Log in Files** (or sandbox:
  `ide-plugins/idea/build/idea-sandbox/system/log/idea.log`).
- Grep for our failures:
  ```
  grep -iE "JCEF markdown|JcefHtmlImageRenderer|JBCef" <path-to>/idea.log
  ```
- [ ] Paste any `WARN`/exception lines here:
  ```
  (paste)
  ```

---

## 6. Overall verdict (your call)

- [ ] **Ship `jcef` as opt-in experimental** as-is (good enough to offer).
- [ ] **Iterate** — list the top 1–3 issues from §3/§4 for me to fix next:
  1.
  2.
  3.
- [ ] **Park it** — Swing upgrade is enough for now.

---

### Reference — what I built (so feedback maps to code)
- `JcefHtmlImageRenderer.kt` — off-screen OSR render → `scrollHeight` via
  `JBCefJSQuery` → paint OSR component to a `BufferedImage` → dispose browser.
  *(suspect: capture timing, HiDPI scale, off-screen window coaxing CEF to paint)*
- `JcefMarkdownBlockView.kt` — caches the image; Swing fallback until ready;
  `bindRepaint` re-measures the fold when the image arrives.
- `MarkdownBlockViewFactory.create` — picks `jcef` when the setting is on **and**
  `JBCefApp.isSupported()`, else `swing`.
- `MarkdownHtmlTheme` / `EditorMarkdownPalette` — one themed HTML doc for both
  surfaces; JCEF gets an opaque editor-matched background.
