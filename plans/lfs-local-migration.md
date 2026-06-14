# LFS migration: move research/sigraph + plans/gpu PDFs off GitHub LFS

_Written 2026-06-14. STATUS: going-forward swap DONE (commit fcc7305 on main);
destructive all-refs history purge DEFERRED (see "Decision" below)._

## What was done (fcc7305)

- All 127 PDFs under `research/sigraph/` + `plans/gpu/` copied to
  `/home/julian/cajeta-research-lfs/`, every one sha256-verified vs its LFS oid.
- Each PDF removed from the repo and replaced by `<name>.pdf.txt` (canonical
  source URL + sha256; 126 verified, 1 idTech5 talk flagged UNVERIFIED).
- Pushed to main — clean, no LFS-budget hit (no new LFS objects).

## Decision: full force-push history purge was DEFERRED, not done

The all-refs `git filter-repo` rewrite was intentionally NOT run, because:
1. **It would not reclaim GitHub's LFS quota** — GitHub keeps historical objects
   counted until the repo is recreated or Support is asked. So the rewrite's only
   benefit is shrinking full-history clones, which LFS-skip already handles.
2. **It is disruptive** — force-pushing all branches + re-tagging rc1-3 would
   clobber the live PHOENIX self-hosted-runner clone and the `ci/*` branches in
   active use, for ~no practical gain.

To actually restore LFS access / reclaim quota: **bump the GitHub LFS data pack
or recreate the repo** (separate task). If a full history purge is still wanted
later (e.g. as part of a repo recreate), the mechanism is below.

## Decisions (from the user)

- **Scope:** every PDF under `research/sigraph/` (47) and `plans/gpu/` (80) =
  **127 files ≈ 2.2 GB**. All other LFS PDFs (collections, concurrency, crypto,
  ml, sorting, llvm-ir — ~90 files, domain-relevant) **stay on GitHub LFS**.
- **Local store:** the binaries move to **`/home/julian/cajeta-research-lfs/`**
  on this Linux workstation, mirroring their repo paths.
- **Repo replacement:** each migrated `…/foo.pdf` is replaced by a committed
  **plain-text `…/foo.pdf.txt`** containing the **canonical source URL** of the
  paper (arXiv abstract page / DOI / project page), so anyone without the local
  store can re-fetch the original. (NOT a pointer to the local store.)
- **Sequencing:** **migrate now**, then **re-gate GA** on the rewritten HEAD and
  re-tag `v0.7.0` (the rewrite changes `8038ed0`'s SHA).

## CRITICAL caveat — GitHub does NOT auto-reclaim LFS quota

A history rewrite makes fresh clones/HEAD lean, but **GitHub keeps the LFS
objects counted against storage even after they're unreferenced**. GitHub's own
docs: removing LFS files from history does not reduce usage; you must **delete &
recreate the repo, or contact GitHub Support** to reclaim. So the rewrite alone
will **not** restore the currently-cut-off LFS access. To get back under budget
you'll ALSO need one of: bump the LFS data pack (temporary), recreate the repo,
or a support ticket. The rewrite is still the right structural step (stops future
bloat, gets the binaries local), but it is not sufficient by itself. **Confirm
you understand this before the force-push.**

## Other caveats

- **Prereq not yet verified:** we must confirm the actual PDF bytes are present
  on this box (smudged), not just LFS pointers — the budget cutoff may have
  blocked fetches. If any are pointers-only, we cannot move them to the local
  store and must fetch from source first. CHECK before deleting anything.
- **Source-URL accuracy:** 127 papers need canonical URLs looked up
  individually. Famous ones (NeRF, 3DGS, instant-ngp, ReSTIR, Nanite…) are
  unambiguous; obscure ones aren't. Any URL not confidently verified gets the
  paper title + a `# UNVERIFIED` marker in its `.txt` for human review rather
  than a guessed link.
- **All clones re-clone:** force-pushing rewritten history to every branch + tag
  means the Windows box (and any other clone) must re-clone or hard-reset. The
  `v0.7.0-rc1/rc2/rc3` tags and the other branches all reference these PDFs, so
  filter-repo must rewrite ALL refs or the objects stay referenced.

## Mechanism (execution order)

1. **Verify bytes present** for all 127 (size > ~10 KB on disk = real content).
   Abort if any are pointer-only until fetched.
2. **Copy binaries → local store**, preserving relative paths:
   `/home/julian/cajeta-research-lfs/<repo-relative-path>`. Verify sha256 of each
   copy against the LFS oid.
3. **Source URLs:** look up each paper's canonical URL; write
   `<path>/<name>.pdf.txt` (URL, plus a title line; `# UNVERIFIED` where unsure).
4. **History rewrite (destructive):** fresh mirror clone; `git filter-repo
   --path research/sigraph/ --path plans/gpu/ --path-glob '*.pdf' ...` to strip
   ONLY the PDFs under those two trees from ALL refs (keep the other 90). Then add
   the `.txt` files at HEAD on each relevant branch.
   - Tool: `git filter-repo` (install if absent; do NOT use filter-branch).
   - Update `.gitattributes`: the `*.pdf filter=lfs` rule stays for the other 90;
     the two migrated trees no longer contain PDFs.
5. **Force-push** all rewritten branches + tags.
6. **Re-clone** here and on the Windows box.
7. **Re-gate GA:** dispatch the 4-target dry-run on the rewritten `main` HEAD;
   on 4/4 green, tag `v0.7.0` on the rewritten GA commit.
8. **Reclaim GitHub quota** separately (budget bump / repo recreate / support) —
   see the CRITICAL caveat.

## Open question to confirm before the force-push (step 4+)

Given GitHub won't auto-reclaim the quota, do you still want the full all-refs
rewrite now (lean repo going forward, GA re-gated), or bump the budget first to
restore access and do the rewrite without GA pressure? (User already chose
"migrate now" — re-confirm only because the quota-reclamation reality may not
have been priced in.)
