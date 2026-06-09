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
//   - POD structs by value — a plain `class X { <all-primitive fields> }`
//     with no inheritance and no marker interface (Item 7). Marshalled
//     field-by-field through the kernelParams ABI (the vtable word is
//     stripped); the kernel body reads fields via `x.field`.
//   - user types declared `class X implements KernelArg` (the marker
//     interface from runtime/src/cajeta/xpu/core/KernelArg.cajeta) — still
//     admitted even when not a pure POD (the explicit opt-in)
//   - Texture2D / Sampler (Item 8) — matched by canonical name. A read-only
//     2-D float image and its filtering/addressing config; bound as image +
//     sampler descriptors (Vulkan) or texture-object handles (CUDA/HIP), and
//     read in the kernel via `tex.sample(sampler, u, v)`. Sampler is admitted
//     by name (NOT as a POD struct) so it takes the sampler-descriptor path.
//
// v1 does NOT yet admit:
//   - non-POD structs (non-primitive fields / inherited fields) without an
//     explicit `implements KernelArg`
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

    // Is `type` a plain POD struct admissible by value as a kernel arg —
    // a non-interface, non-Buffer class with no inherited fields and at
    // least one field, all of whose instance fields are primitives? Shared
    // by the admissibility check and the launch-site marshaller so both
    // agree on exactly which arguments take the by-value struct path.
    bool isPodStructType(const CajetaTypePtr& type);

    // Is `type` the cajeta.xpu.core.Texture2D resp. Sampler type (Item 8)?
    // Matched by canonical name. Shared by the admissibility check, the
    // launch-site marshaller, and the kernel-param classifier so all agree on
    // which arguments take the texture-image resp. sampler-descriptor path
    // (rather than the by-value POD-struct path Sampler would otherwise match).
    bool isTextureType(const CajetaTypePtr& type);
    // Is `type` the cajeta.xpu.core.Texture3D<T> type — the volumetric sibling of
    // Texture2D? Matched by canonical name; takes the same texture-image path as
    // Texture2D but with a 3-D image and 3-coord sample/fetch.
    bool isTexture3DType(const CajetaTypePtr& type);
    // Is `type` the cajeta.xpu.core.Texture1D<T> type — the linear sibling of
    // Texture2D? Matched by canonical name; takes the same texture-image path as
    // Texture2D but with a 1-D image and 1-coord sample/fetch (no lod).
    bool isTexture1DType(const CajetaTypePtr& type);
    // Is `type` the cajeta.xpu.core.Texture2DArray<T> type — the layered sibling
    // of Texture2D (N 2-D planes)? Matched by canonical name; takes the texture-
    // image path with an Arrayed image and a (u,v,layer)/(x,y,layer) coord.
    bool isTexture2DArrayType(const CajetaTypePtr& type);
    bool isSamplerType(const CajetaTypePtr& type);

    // Is `type` the cajeta.xpu.core.Image2D type (writable images)? Matched by
    // canonical name — the writable twin of Texture2D, bound as a STORAGE_IMAGE
    // descriptor and written in a kernel via `img.store(x, y, value)`. Shared by
    // the admissibility check, the launch-site marshaller, and the kernel-param
    // classifier so all agree it takes the storage-image path.
    bool isImageType(const CajetaTypePtr& type);

    // Is `type` the cajeta.xpu.core.AccelerationStructure resp. RayQuery type
    // (cajeta-gpu Part C ray query)? Matched by canonical name.
    // AccelerationStructure is an admissible kernel arg (a descriptor-bound BVH);
    // RayQuery is a device-only kernel-body local (never a kernel arg), recognized
    // by the device lowerer when it declares a RayQuery local.
    bool isAccelStructType(const CajetaTypePtr& type);
    bool isRayQueryType(const CajetaTypePtr& type);
    // CooperativeMatrix<T,Rows,Cols,Use> — also a device-only kernel-body local
    // (a subgroup matrix-core tile), never a kernel arg.
    bool isCooperativeMatrixType(const CajetaTypePtr& type);

    // Validate every parameter of `method`. Throws cajeta::Exception
    // (errorId "XPU-K01") with a clear diagnostic on the first non-
    // admissible parameter found. No-op when `method` is not a @Kernel
    // — call sites can invoke this unconditionally on any method
    // about to be codegen'd.
    void validateKernelParams(const MethodPtr& method);

} // namespace xpu
} // namespace cajeta
