# C0 runbook — downstream LLVM fork + prebuilt-artifact pipeline

Goal: cajeta (CI + local) builds against a **prebuilt** LLVM/clang/lld toolchain that
carries our SPIR-V backend patches — so CI never builds LLVM and the Cajeta repo never
vendors LLVM source. This is the gate for every later Part C backend patch (ray query, …).

**Build contract** (must match — from `cpp/llvm-project/build/CMakeCache.txt`):
- Pinned base commit: **`203c0668d4b098714d1748de766e890fe6296891`** (LLVM 23-git)
- `CMAKE_BUILD_TYPE=Release` · `LLVM_ENABLE_RTTI=ON` · `LLVM_ENABLE_ASSERTIONS=OFF`
- `LLVM_TARGETS_TO_BUILD=X86;NVPTX;AMDGPU;SPIRV` · static libs (no dylib)
- **`LLVM_ENABLE_PROJECTS=clang;lld`** — the artifact MUST bundle a version-matched
  `clang-23` (cajeta compiles its runtime to bitcode via
  `find_program(clang-${LLVM_VERSION_MAJOR})`; there is no clang-23 on the dev box today).

---

## Step 1 — Fork, then rename to `cajeta-llvm`
On `github.com/llvm/llvm-project` → **Fork**:
- Owner: your account (the Fork button always creates it as `jklappenbach/llvm-project`).
- **Check "Copy the `main` branch only."** (We don't need every upstream branch.)
- Create fork.
- Then **Settings → rename the repo to `cajeta-llvm`.** The fork relationship survives
  (still shows "forked from llvm/llvm-project"), GitHub auto-redirects the old URL, and
  upstream sync / PRs are unaffected. The only thing tied to the name is the release
  asset URL in Step 5 — already written as `cajeta-llvm` below.

> Note: forking copies `main` at upstream's *current* HEAD, which may be ahead of our
> pinned commit. That's fine — Step 2 creates our branch at the exact pin.

## Step 2 — Create the patch branch at the pinned base
Locally (you already have the checkout at `cpp/llvm-project`):
```bash
cd /home/julian/code/cpp/llvm-project
git remote add fork git@github.com:jklappenbach/cajeta-llvm.git
git remote add upstream https://github.com/llvm/llvm-project.git   # for future rebases
git fetch fork
git branch cajeta-spirv 203c0668d4b098714d1748de766e890fe6296891
git push fork cajeta-spirv
```
This branch is where SPIR-V backend patches (ray query, cooperative-matrix wiring, …)
will live. v0 carries **zero** patches — the point of v0 is just to stand up the
pipeline so cajeta stops needing a local source build.

## Step 3 — Add the build workflow to the fork
Copy `fork-build-llvm.yml` (this dir) into the fork at
`.github/workflows/build-cajeta-llvm.yml` on the `cajeta-spirv` branch, commit, push.

**Runner choice (decide now):**
- **Best — self-hosted runner on your Strix Halo box.** A from-scratch clang+llvm build
  is heavy and can OOM at link on free GitHub runners. Self-hosted = your hardware,
  automated, fast. Register one: repo → Settings → Actions → Runners → New self-hosted
  runner (Linux x64), then the `runs-on: [self-hosted, linux, x64]` in the workflow
  matches it. *(Quick bootstrap alternative: skip CI for v0 — build locally with
  `LLVM_ENABLE_PROJECTS=clang;lld` added, `tar --zstd` the install tree, and
  `gh release create cajeta-llvm-23-r1 <tarball>` by hand. Automate via the workflow
  after.)*
- **Fallback — `ubuntu-22.04`** GH-hosted: flip the `runs-on` line; keep
  `LLVM_PARALLEL_LINK_JOBS=1`. Expect 1–2 h with cold ccache; risk of link OOM.

## Step 4 — Cut the first toolchain release
- Actions → **build-cajeta-llvm** → Run workflow → tag `cajeta-llvm-23-r1`.
  (Or `git tag cajeta-llvm-23-r1 && git push fork cajeta-llvm-23-r1`.)
- Produces a Release with `cajeta-llvm-23-r1-linux-x64.tar.zst` (+ `.sha256`).

## Step 5 — Point cajeta at the artifact
- **CI:** add the step from `cajeta-ci-consume.yml` to cajeta's CI job *before* the
  cmake configure. Update `LLVM_ASSET_URL` to the Step-4 asset. No cajeta source change
  (find_package honors the `LLVM_DIR` env var; bundled clang lands on PATH).
- **Local (optional):** `export LLVM_DIR=.../lib/cmake/llvm` + add `.../bin` to PATH,
  then `./build.sh`. Your current local source build keeps working untouched otherwise.

## Step 6 — Verify
A cajeta build that resolves `LLVM_DIR` to the extracted artifact and links clean is C0
done. After that, the loop for each Part C feature is: patch `cajeta-spirv` →
re-tag (`-r2`, …) → workflow rebuilds & republishes → bump cajeta's `LLVM_ASSET_URL`.

---

## Runner topology & artifact matrix

**Host artifacts ≠ codegen targets.** The targets `X86;NVPTX;AMDGPU;SPIRV` are all in
ONE toolchain build — a single host toolchain emits for all of them. A separate
artifact is needed only per **host platform** (where the cajeta compiler binary runs,
because the LLVM libs are native). The host matrix is defined by cajeta's existing
`release.yml`: `x86_64-linux-gnu`, `aarch64-linux-gnu`, `aarch64-apple-darwin`,
`x86_64-w64-mingw32`.

**Latent issue this fixes:** `release.yml` currently provisions LLVM from **distro
packages** (apt / brew / MSYS2 → LLVM ≤ 22), while dev builds on source LLVM 23. Any
release that exercises a 23-only feature (graphics SPIR-V; ray query) would lack it or
break. C0 must replace that distro-LLVM step in each release leg with the LLVM-23 artifact.

**Build priority (demand-driven):**
1. **`linux-x64` first** — unblocks ALL Part C dev + on-device testing on BOTH GPU
   vendors (see runners). Build this one now.
2. `aarch64-linux-gnu` next — double-serves release **and** Jetson Orin / cradle (arm64).
3. `aarch64-apple-darwin` — release parity.
4. **`x86_64-w64-mingw32` — RELEASE-ONLY, DEFERRED.** Native Windows is the fiddliest
   leg (mingw-w64 LLVM build) and tangles with MSVC-centric CUDA. **Not** needed for NV
   bring-up (see below). Do it only when shipping Windows binaries with 23-only features.

### Self-hosted runners (two boxes, both Linux-flavored)
| Box | Role | Toolchain it uses |
|---|---|---|
| Strix Halo (Linux) | AMD/Vulkan on-device tests + builds the `linux-x64` artifact | `linux-x64` (native) |
| Windows + NVIDIA, **via WSL2** | **NVIDIA on-device tests** (gpu-plan B5 + the 7 skipped NV exec tests) | `linux-x64` reused inside WSL2 |

**Key decision — NVIDIA testing runs in WSL2, not native Windows.** The Windows box's
GPU is the only NVIDIA hardware. Rather than gate NV bring-up on the hard native-Windows
toolchain, run **WSL2 (Ubuntu) + CUDA-on-WSL** there: install the `linux-x64` artifact +
CUDA toolkit (ptxas/fatbinary), register it as a second self-hosted **Linux** runner,
and run cajeta's gtest exec suite against the real NVIDIA GPU. So NV testing needs only
the `linux-x64` artifact (already #1), and the native `x86_64-w64-mingw32` leg stays
release-only. *(Verify on first setup: CUDA-on-WSL exposes libcuda + ptxas installs; the
alternative is a bare-metal Windows MSVC/CUDA path — the harder road.)*

## Maintenance
- **Rebase cadence:** periodically re-pin `cajeta-spirv` onto a newer upstream commit;
  drop patches that landed upstream. Record base commit + applied-patch list in the
  fork's branch README.
- **Upstream-first:** anything upstream will accept (ray query, cooperative-vector) goes
  up as a PR; the branch carries only not-yet-landed / in-review patches → minimal rebase debt.

## What I can/can't do
I generated the two workflow files + this runbook locally. The fork, the self-hosted
runner registration, the GitHub Release, and the multi-hour build are GitHub-account /
infrastructure actions on your side — I can't click those. Hand me any error output
(build config failure, link OOM, find_package not resolving) and I'll fix the files.
