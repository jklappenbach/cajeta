# runtime-path-nul-termination — defect investigation (found during cajeta-llm Unit 4)

## 1. Definition

**1.1 Claim as filed.** cajeta-llm spec §3.8: "When a path is built at
runtime by concatenation or substring, it is NUL-terminated before reaching
the native layer." Filed 2026-08-08 as one of three stdlib defects split out
of that spec (its decision 13.8), on the concern that a runtime-constructed
`String` — in particular a mode-2 windowed substring, which has **no NUL at
its window end** by design (slice-spec §7.1) — could reach `open(2)` as a
data pointer and read past the window into whatever bytes follow.

**1.2 The seam, audited.** Every `File` intrinsic that takes a path routes
its String argument through `loadStringArg`
(`MethodCallExpression.cpp` — `loadPathArg` is a direct wrapper), which
unwraps a class String by calling the runtime's canonical mode-aware
accessor `__cajeta_string_cstr` (`cajeta_rt_shared.c`). That helper:

- returns the data pointer directly for a full-window heap root (mode 0)
  and a static literal root (mode 1) — both builders guarantee a trailing
  NUL;
- **materializes** inline-form text and mode-2 windowed views into a
  per-thread scratch ring and NUL-terminates the copy — exactly the case
  §3.8 worries about.

`cajeta_rt_process.c` documents the same helper as "the canonical
mode-aware accessor" for its argv seam.

**1.3 Verdict.** Cannot reproduce at the File seam. The concern was valid
when filed against the pre-`__cajeta_string_cstr` unwrap (a bare
`bytes + 8` GEP), but the cstr routing closed it for every String-taking
File intrinsic. No other native seam takes a cajeta String as a path
without passing through either `loadStringArg` or `__cajeta_string_cstr`.

## 2. Pin

`test/expression/FileIo64Tests.cpp` (`runtimeConstructedPathOpens`,
cajeta-llm 4.1.3) pins both construction forms — a `+`-concatenated path
and a mode-2 windowed substring whose source deliberately carries trailing
garbage (`"…###"`) that an un-terminated window would leak into the
filename — and verifies the files land at the exact expected paths,
checked outside the JIT.

## 3. Status

Closed as already-fixed-by-cstr, with the regression pin above. If a future
seam takes paths around `loadStringArg`, this spec is the checklist: route
it through `__cajeta_string_cstr`, never through a bare `bytes + 8`.
