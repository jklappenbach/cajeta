## Summary

<!-- One or two sentences on what changed and why. -->

## Test plan

<!-- What was run / what to run when reviewing locally. -->

- [ ] `cd build && cmake -G Ninja -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm .. && ninja`
- [ ] `ctest -j $(nproc)` (or relevant `--gtest_filter` for partial)

## Variance check (required for any change touching `xpu/core/*`)

<!--
Mandatory only when this PR adds or modifies an API surface in
src/cajeta/xpu/core/ or runtime/src/cajeta/xpu/core/. Otherwise
delete this section.

Per cajeta-docs/CajetaXPU-Variance.md, every xpu.core API must clear
the three-column check before landing — the NVIDIA implementation is
the only one that exists today; AMD and Vulkan must be designed in,
not retrofitted. The expensive case is rewriting kernels later.
-->

**Surface(s) added/changed:**

<!-- e.g. Buffer<T>.alloc, Stream.sync, @Kernel argument trait, ... -->

**Variance rows applicable** (from `cajeta-docs/CajetaXPU-Variance.md` §2):

<!-- e.g. Rows 2 (launch arg model), 3 (allocator), 6 (sync primitives) -->

**Three-column check:**

- NVIDIA: <how it works on the implementation that exists>
- AMD: <how it would work — mental model — without forcing an API redesign>
- Vulkan: <how it would work — mental model — without forcing an API redesign>

**Decision:**

<!--
One of:
- ✓ All three pass — lands in xpu.core as designed
- ⟳ Restructured so [the divergent dimension] becomes a parameter; updated [N]
- → Moved to vendor namespace because [backend] genuinely can't support it
-->

**New variance rows discovered** (append to `CajetaXPU-Variance.md` §2 in the same PR):

<!-- e.g. "Row 13: cooperative-matrix tile shapes …" — or "none" -->
