# Windows Release CI — Plan

Derived from `windows-release-ci-spec.md`. Tasks ordered by dependency. All
changes are Windows-gated; none alter the Linux/macOS build paths.

## 1 Unblock the Windows toolchain fetch + configure

Get the fork toolchain onto the runner and make CMake select the LLVM-23 tools.

### TDD
- The Windows CI leg is the test harness: each fix is validated by a
  `workflow_dispatch` run (`mode=dry-run`, `targets=x86_64-w64-mingw32`).

### Deliverables
- `1a` [x] Anchor the LLVM toolchain in the writable workspace on Windows
  (`DEST="$PWD/cajeta-llvm"`) instead of `$HOME` (which is the read-only `/`
  mount on the self-hosted MSYS2 shell). Linux/macOS keep `$HOME`.
- `1b` [x] Pin `-DCAJETA_CLANG=<fork>/bin/clang.exe` on Windows so the
  runtime-bitcode compile uses the fork's LLVM-23 clang, not MSYS2's LLVM-22.
- `1c` [x] Pin `-DCAJETA_LLVM_LINK=<fork>/bin/llvm-link.exe` on Windows. The
  link step otherwise used MSYS2's LLVM-22 `llvm-link`, which cannot read
  LLVM-23 bitcode ("Not an int attribute ... Reader: 'LLVM 22.1.4'"). The `-D`
  override also defeats the self-hosted runner's stale `CMakeCache.txt`.
- `1d` [x] `setup.sh` forwards `"$@"` to its `cmake` configure call so CI can
  inject the two `-D` pins. Inert for every other caller (no extra args).

### Acceptance Criteria
- [x] Fetch LLVM step passes on Windows.
- [x] Configure step passes (toolchain-major check satisfied).
- [x] Build step passes (runtime bitcode links; full C++ build completes).

## 2 Quiet Windows-only build-warning noise

Keep a genuine warning visible by suppressing system-header noise.

### TDD
- Inspect the Windows Build-step log; confirm the suppressed warnings are gone
  and no cajeta-source warnings were hidden.

### Deliverables
- `2a` [x] Add `-Wno-pragma-pack -Wno-missing-declarations` to
  `CAJETA_RT_FLAGS` under `if(WIN32)` in `src/CMakeLists.txt`. These warnings
  originate entirely in MSYS2's `<windows.h>` family (`winnt.h`, `wingdi.h`,
  `winuser.h`, `mstcpip.h`), not in cajeta source. Output-only — does not change
  the emitted bitcode.

### Notes / not changed
- `-Woverride-module` (1×, from assembling `cajeta_fp128_builtins.ll`) is
  inherent to the vendored target-neutral `.ll` and is identical on every
  platform. Silencing it would be a global change, so it is intentionally left
  as-is per the "no global changes" constraint.

### Acceptance Criteria
- [ ] Windows Build-step log shows no `-Wpragma-pack` / `-Wmissing-declarations`
  noise and no newly-hidden cajeta warnings.

## 3 Reach green on build → smoke → tests

### TDD
- Full dry-run dispatch reaching the "Run release tests" step.

### Deliverables
- `3a` [x] Build step green.
- `3b` [x] Smoke test (`cajeta --version`) green.
- `3c` [x] Run release tests green (run 27490512587).
- `3d` [x] WiX provision + native installers + staging green (run 27490512587).
- `3e` [x] Archive (zip): MSYS2 MINGW64 has neither `zip` nor `python` on this
  runner, so the step failed with `python: command not found`. Replace with
  `cmake -E tar cf "${STAGE_DIR}.zip" --format=zip "${STAGE_DIR}"` (cmake is
  always present). Windows-only step (`if: matrix.archive_ext == 'zip'`).

### Acceptance Criteria
- [ ] The `build (x86_64-w64-mingw32)` job conclusion is `success`
  (everything through Archive + Upload artifact).

## 4 Cross-OS verification mandate (CARRY-OVER)

**Mandate:** No genuine cajeta *code* bug was found while fixing Windows so far
— every change is build-system (CMake/workflow/`setup.sh`) or Windows-only
warning suppression, none of which alters behavior on Linux/macOS. **If a later
iteration must `#ifdef _WIN32` a real code path** (in `runtime/native/` or the
compiler), the following MUST be checked afterward on the other OSes:

### Deliverables
- `4a` [ ] If any `#ifdef _WIN32` code-behavior gate is added, re-run the Linux
  (x86_64 + aarch64) and macOS (arm64) legs to confirm the non-Windows branch
  is unchanged and still green.
- `4b` [ ] Audit each Windows `-D`/flag pin to confirm it is genuinely
  Windows-scoped and the Linux/macOS configure is byte-for-byte unchanged.

### Acceptance Criteria
- [ ] All four host legs (linux-x64, linux-arm64, macos-arm64, windows-x64)
  green on a full `targets=all` dispatch before merging to `main`.
