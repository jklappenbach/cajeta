# Get all PDFs out of git — sidecar `.txt` metadata, binaries gitignored

_2026-06-14. Directive: stop using Git LFS for PDFs entirely; no split._

## End state

- **Every** PDF in the repo (all 217, under `research/**` and `plans/gpu/**`) is
  **removed from git** (untracked; no LFS) and added to **`.gitignore`**. The
  binaries stay on disk in their existing directories — just not tracked.
- Each PDF gets a sibling **`<name>.pdf.txt`**, tracked in git, in the same
  directory, containing the paper's **name (title)**, a one-line **description**,
  and the **download URL**. Anyone can re-fetch the original from the URL.
- The `*.pdf filter=lfs` rule is removed from `.gitattributes` (nothing PDF is
  LFS anymore).

Rationale: half the PDFs in LFS and half elsewhere makes no sense; one uniform,
LFS-free scheme. Avoids the GitHub LFS budget entirely going forward.

## Steps

1. Source **name + description + URL** for all 217 PDFs (verify URLs; flag any
   unverified rather than guessing).
2. Write `<path>.pdf.txt` for all 217 (name / description / source URL).
3. `git rm --cached` every tracked PDF (keep the bytes on disk).
4. Add `*.pdf` to `.gitignore`; remove `*.pdf filter=lfs` from `.gitattributes`.
5. `git add` the 217 `.txt` + `.gitignore` + `.gitattributes`; commit; push.

## Notes

- This does not touch git history, so it doesn't by itself reclaim GitHub's
  already-counted LFS storage (that needs a budget bump or repo recreate) — but
  it stops all future PDF/LFS usage and makes the repo self-describing.
- URL sourcing: 126/127 of the first tranche already verified; the rest + names +
  descriptions sourced the same way.
