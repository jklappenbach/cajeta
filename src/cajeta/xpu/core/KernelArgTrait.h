//
// KernelArg admissibility check for @Kernel parameter lists.
//
// Per CajetaXPU.md §3.1.1, @Kernel functions take only parameters whose
// types satisfy the KernelArg trait — primitives, POD structs, Buffer<T>,
// Texture<...>, Sampler, and @PushConstant structs (Vulkan only).
//
// v1 admits:
//   - primitives (anything with PRIMITIVE_FLAG)
//   - Buffer<T> (any T) — matched by canonical-name prefix
//   - user types declared `class X implements KernelArg` (the marker
//     interface from runtime/src/cajeta/xpu/core/KernelArg.cajeta)
//
// v1 does NOT yet admit:
//   - POD structs without an explicit `implements KernelArg`
//     (structural-trait check lands when the broader trait system does)
//   - Texture<Format,Dim> / Sampler — types not yet declared
//   - @PushConstant structs — Vulkan-only, deferred to phase 5
//
// Variance check (CajetaXPU-Variance.md row 2 — launch arg model):
// The KernelArg surface is intentionally minimal so future Vulkan work
// can add a per-arg `descriptor-set, binding` annotation without
// changing the trait shape. The check here is pure type-membership;
// the descriptor-partition pass lives in step 9+ on the Vulkan
// codegen path and reads its inputs from the parameter's marker
// annotations (e.g. @PushConstant) — not from a new trait shape.
//

#pragma once

#include <memory>

namespace cajeta {
    class CajetaType;
    using CajetaTypePtr = std::shared_ptr<CajetaType>;

    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    // Is `type` admissible as a parameter to an @Kernel method?
    bool isKernelArgAdmissible(const CajetaTypePtr& type);

    // Validate every parameter of `method`. Throws cajeta::Exception
    // (errorId "XPU-K01") with a clear diagnostic on the first non-
    // admissible parameter found. No-op when `method` is not a @Kernel
    // — call sites can invoke this unconditionally on any method
    // about to be codegen'd.
    void validateKernelParams(const MethodPtr& method);

} // namespace xpu
} // namespace cajeta
