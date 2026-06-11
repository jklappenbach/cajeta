# Repository cleanup audit — `cajeta-two`

**Date:** 2026-06-08
**Scope audited:** the cajeta compiler repo root and `src/`, `runtime/`, `test/`,
`tools/`, `samples/`, `ide-plugins/`, `cajeta-docs/`, `cajeta-docs-site/`, `plans/`,
`site/`, `scripts/`, `research/`, `util/`, `test-archive/`, `debug-tests/`,
`build-tools/`, `antlr4/`, `generated/`, build directories.

**Methodology**
- `git ls-files` to enumerate tracked files; `git status --ignored` and
  `git check-ignore -v` to separate ignored from tracked.
- `grep -r` across build files (`CMakeLists.txt`, `*.cmake`), scripts (`*.sh`,
  `*.cmd`, `*.yml`) and docs to find references to each candidate.
- For headers: cross-checked `#include` references and symbol usage.
- `du -sh` for on-disk sizes; `git log -1` for last-touched provenance.

**Headline result:** the git tree is clean of build artifacts — the recent
*"Added build created directories to .gitignore"* commit did its job. No `*.o`,
`*.cja`, `*.ll`, `*.bc`, `*.a`, `*.so` or build directories are tracked. The
genuine cleanup candidates are a couple of stale scratch/archive directories, one
orphan header, redundant on-disk build directories, and (judgment call) a very
large committed PDF corpus. Be conservative: most `plans/` entries marked
"DONE/COMPLETE" are *deliberately retained design records* and must not be removed.

---

## 1. Build artifacts / generated output checked into git

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `build/` (3.4 GB) | CMake/Ninja build tree (canonical, used by `build.sh`, `setup.sh`) | `git ls-files build/` → 0 tracked; `git check-ignore -v` matches `.gitignore: build/`; `setup.sh:220 mkdir -p build`; `build.sh` does `pushd build` | n/a (clean) | Nothing to do for git. See §6 for disk. |
| `cmake-build-debug/` (6 MB) | CLion default build dir | `git ls-files` → 0 tracked; all content ignored, but dir name itself **not** in `.gitignore` (`git check-ignore -v cmake-build-debug` → exit 1) | n/a (clean) | See §6 — add explicit ignore entry. |
| `generated/` (872 KB) | ANTLR-generated lexer/parser (`lex/`, `parser/`) | `git ls-files generated/` → 0; ignored via `.gitignore: generated/`; regenerated from `antlr4/*.g4` | n/a (clean) | Keep ignored. Not garbage. |
| `samples/Tour/build/` (12 MB) | sample build output | `git ls-files samples/Tour/build/` → 0; ignored | n/a (clean) | Keep ignored. |
| `site/` | generated cajetadoc HTML (`site/docs/`) | `git ls-files site/` → 0; ignored via `.gitignore: /site/docs/cajetadocs/` | n/a (clean) | Keep ignored. |

**Finding:** No tracked build artifacts anywhere. `git ls-files | grep -E '\.(o|cja|ll|bc|a|so|obj)$'` returns nothing.

---

## 2. Orphaned source files

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `util/` (`AnotherClass.cpp/.h`, `TestCppClass.cpp/.hpp`, `cpp-to-llvm.sh`; ~1 KB total) | Scratch C++→LLVM-IR experiment files | Referenced by **no** CMake/script/include anywhere (`grep -rn 'AnotherClass\|TestCppClass\|cpp-to-llvm'` outside `util/` → empty; not in `CMakeLists.txt`); last commit **2022-12-25** (`92616e9`). `cpp-to-llvm.sh` is a 2-line `clang -emit-llvm` one-liner | **HIGH** | Remove the `util/` directory. Pure scratch, dead since 2022. |
| `src/cajeta/type/CajetaEnum.h` | Header-only stub `class CajetaEnum : public CajetaClass` with a private ctor | `#include`d nowhere (`grep -rl 'include.*CajetaEnum.h'` → empty); symbol `CajetaEnum` used nowhere (`grep -rn CajetaEnum` outside the file → empty); no `CajetaEnum.cpp`; last touched **2023-01-09** | **MEDIUM** | Remove. It compiles into nothing (header, not in any GLOB of `.cpp`), so deletion cannot break the build. Confirm no in-flight enum work depends on it first. |

All other `src/`/`runtime/` `.cpp` are picked up by `file(GLOB_RECURSE ...)` and compile; no further provable orphans found. Names that *looked* suspicious (`Templates.cpp`, `InitTemplates.cpp`, `TempTeardown.h`, `*GoldenVectors*`) are all live, legitimately-named code.

---

## 3. Stale / superseded docs and plans

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `plans/*` marked **DONE/COMPLETE** (`grammar-new-removal.md`, `value-type-overloading-plan.md`, `gpu/matrix-value-type-plan.md`, …) | Completed feature plans | Each carries an explicit banner like *"Retained as the B1 design record"* / *"kept as the record of what changed"* | — | **DO NOT REMOVE.** Intentionally retained design records. Listed here only to pre-empt mistaken deletion. |
| `test-archive/` (8 `.cajeta` files in `code/`, `code-1/`; ~3 KB) | Archived old cajeta sample sources | Name is literally "archive"; referenced by no test/script/CMake (`grep -rn test-archive` outside dir → empty). Recent 2026-06-06 touch was an incidental repo-wide `new`→`heap` keyword sweep (`802a00f`), not real use | **MEDIUM** | Remove, or move to an out-of-tree archive. Not consumed by the build or tests; superseded by `test/` and `samples/`. |
| `exception-fix-plan.md` (repo root, 7.9 KB) | Active **open-bug** diagnosis (CP6f-3c exception-throw hang) | Header says *"Status: open bug, root cause not yet fixed"* | **LOW** | Keep (active). Only nit: it sits at repo root while every other plan lives under `plans/` — consider moving to `plans/lang/` or `plans/buildtool/` for consistency. Not deletion. |

No duplicate or removed-feature docs found in `cajeta-docs/`. `cajeta-docs/` (markdown reference) and `cajeta-docs-site/` (the Astro website source) serve different purposes and are not duplicates; `cajeta-docs-site/` has no tracked `node_modules`/`dist`/lockfiles.

---

## 4. Duplicate or dead scripts

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `regression_tests.sh` / `regression_tests.cmd` | Crash-isolated parallel gtest sweep | Referenced by **no** CI workflow or doc (`grep` in `.github/`, `README.md`, `RELEASING.md` → only `cajeta_tests` and `release_tests` are referenced) | **LOW** | Keep but verify. It is a standalone manual dev tool with a distinct purpose (per-suite process isolation), not a duplicate of `cajeta_tests.sh`. If the workflow has moved entirely to `cajeta_tests`/`release_tests`, it could be retired. Judgment call. |
| `*.cmd` ↔ `*.sh` pairs (`build`, `setup`, `cajeta_tests`, `release_tests`) | Windows/Unix variants | Paired by design | — | **DO NOT REMOVE.** Platform pairs, not duplicates. |

---

## 5. Empty / near-empty dirs, backups, temp/scratch files

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| (none found) | — | `git ls-files | grep -iE '\.(bak|orig|swp|tmp|old)$|~$'` → empty; on-disk `find` for `*~ *.bak *.orig *.swp *.tmp` (excl. build/git) → empty; only empty dir is `./.claude` (ignored) | — | Clean. No editor backups, swap files, or stray temp files. |

---

## 6. Duplicate build directories

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `build/` (3.4 GB) | Canonical build tree | Created/used by `setup.sh` (`mkdir -p build`) and `build.sh` (`pushd build`); explicitly in `.gitignore` | — | Keep. (3.4 GB is large but legitimate build output — `cmake --build` cache, objects.) |
| `cmake-build-debug/` (6 MB) | CLion's auto-created build dir | Referenced by **no** script, CMake, or doc; contents fully ignored, but the directory **name** is not an explicit `.gitignore` rule (`git check-ignore -v cmake-build-debug` exits 1 — it only collapses in `status --ignored` because every file inside happens to match a pattern) | **MEDIUM** | (a) Safe to `rm -rf cmake-build-debug/` on disk — it's regenerated by CLion on demand and duplicates `build/`. (b) Add an explicit `cmake-build-debug/` line to `.gitignore` so a stray non-build file there can't get committed accidentally. |

---

## 7. Judgment call — repository bloat (not "garbage", flagged for awareness)

| Path | What it is | Evidence | Confidence | Recommendation |
|------|-----------|----------|-----------|----------------|
| `research/` (1.3 GB, 144 tracked files) | Reference PDFs + `ResearchPlan.md` per topic (collections, concurrency, …) | `git ls-files research/` → 144 files, mostly `papers/*.pdf`; `du -sh research` → 1.3 GB; `.git` is 1.5 GB largely as a result | **LOW** | Deliberately curated (each subdir has a `ResearchPlan.md`), so **not** garbage. But committing ~1.3 GB of PDFs into the compiler repo bloats every clone and the `.git` history permanently. Consider moving the `papers/*.pdf` to Git LFS, a separate `cajeta-research` repo, or out-of-tree storage, keeping the `ResearchPlan.md` files. Purely a repo-hygiene suggestion. |

---

## Quick wins (HIGH confidence — safe to act on now)

1. **Delete `util/`** — 5 scratch C++/LLVM experiment files, dead since 2022, referenced nowhere, not in any CMake glob. (§2)

## Strong candidates (MEDIUM — verify, then act)

2. **Delete `src/cajeta/type/CajetaEnum.h`** — orphan header stub, `#include`d nowhere and symbol unused since 2023; cannot break the build. (§2)
3. **Delete `test-archive/`** — explicitly-named archive of old `.cajeta` sources, consumed by no build/test/script. (§3)
4. **Remove `cmake-build-debug/` from disk and add it to `.gitignore`** — redundant CLion build dir duplicating `build/`. (§6)

## Notes / do-not-touch

- All `plans/*` files marked DONE/COMPLETE are **intentionally retained** design records — leave them.
- `.cmd`/`.sh` script pairs are platform variants, not duplicates.
- `generated/`, `site/`, `samples/Tour/build/`, `build/` are correctly gitignored — no git action needed.
- `research/` (1.3 GB) is curated, not garbage — but see §7 re: repo bloat.
