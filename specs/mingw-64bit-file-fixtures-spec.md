# mingw-64bit-file-fixtures — defect (found by the 2026-08-13 device-tests nightly)

## 1. Definition

**1.1 Symptom.** The `windows-nvidia` leg of `device-tests.yml` never reached a
test: the build broke.

```
FAILED: test/CMakeFiles/cajeta_test.dir/expression/FileIo64Tests.cpp.obj
test/expression/FileIo64Tests.cpp:84:17: error: '::pwrite' has not been declared; did you mean 'write'?
ninja: build stopped: subcommand failed.
```

**1.2 Found.** 2026-08-13, run 31682002702 (first scheduled `device-tests`
nightly), job `windows-nvidia` on `PHOENIX`, building `596d72de`. The file was
added the day before by cajeta-llm Units 3–4 and had only ever been compiled
on Linux.

**1.3 Root cause — three POSIX assumptions, only one of which the compiler
catches.** `test/expression/FileIo64Tests.cpp`'s fixture helper
`makeSparseWithTailMark` builds the multi-GiB sparse files that Unit 4's
bit-31 tests are *about*:

1. **`::pwrite`** — not in MinGW's CRT at all. This is the hard error, and the
   only one of the three that fails loudly.
2. **`::ftruncate(fd, (off_t) size)`** — MinGW's `off_t` is 32 bits, so a
   `2^31 + 16` length wraps negative. It compiles. It would then silently
   defeat the exact bit-31 truncation the test exists to pin: the fixture
   would not be ≥ 2 GiB, so `readHonorsLengthPastBit31` could pass while
   proving nothing.
3. **Sparseness.** The file header states "the files are sparse so the disk
   cost is metadata only". POSIX `ftruncate` past EOF gives that for free;
   NTFS does not — without `FSCTL_SET_SPARSE` the extend reserves the full
   length and the tail write forces a zero-fill of everything below it. 2 GiB
   of pointless I/O per run, and the 8 GiB fixture would be worse if it were
   not already Linux-gated.

**1.4 Blast radius.** The whole `cajeta_test` binary is one target, so a single
non-portable test TU takes down every Windows test — including the device
tests this workflow exists to measure. The same class of break landed the day
before (`800bd63d`, `::setenv` in the buildtool toolchain-path test) and is
addressed there by `test/PortableEnv.h`.

**1.5 Reproduce.** Build `cajeta_test` under MSYS2 MINGW64, or run
`device-tests.yml` with `legs=windows`.

## 2. Acceptance

- **2.1** `cajeta_test` compiles under MSYS2 MINGW64 g++ — no `::pwrite`, and
  no 32-bit `off_t` on a path that carries a ≥ 2 GiB length.
- **2.2** The Windows fixture file's *logical* length is the full ≥ 2 GiB, so
  the bit-31 case is genuinely exercised there and not merely skipped past.
- **2.3** The Windows fixture is sparse: bytes actually allocated on disk stay
  a small fraction of the logical length, as the file header claims.
- **2.4** The Linux path is byte-for-byte unchanged — `#ifdef _WIN32` only, no
  behavioural edits to the POSIX branch.

**Verified 2026-08-13** with MSYS2 MINGW64 g++ on `PHOENIX` against a
standalone extraction of the three helpers: compiles clean with `-Wall`, and
the 2^31+16 fixture reports `size=2147483664 onDisk=67895296` with the tail
marker read back correctly — 2.1–2.3 all hold.
