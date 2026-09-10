// KernelArg admissibility for @Kernel parameter lists: primitives, POD
// structs, Buffer<T>, the texture and image types, Sampler, and any class
// declaring `implements KernelArg`. See CajetaXPU.md §3.1.1 for the trait.

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

    /// Is `type` admissible as a parameter to an @Kernel method?
    bool isKernelArgAdmissible(const CajetaTypePtr& type);

    /// A non-interface, non-Buffer class with no inherited fields and at least
    /// one, all primitive: the by-value struct path, shared with the marshaller.
    bool isPodStructType(const CajetaTypePtr& type);

    /// cajeta.gfx.Texture2D, by canonical name. Sampler is matched by name too
    /// rather than falling into the by-value POD path it would otherwise take.
    bool isTextureType(const CajetaTypePtr& type);
    /// cajeta.gfx.Texture3D<T>: a 3-D image with 3-coord sample and fetch.
    bool isTexture3DType(const CajetaTypePtr& type);
    /// cajeta.gfx.Texture1D<T>: a 1-D image with 1-coord sample and fetch, no lod.
    bool isTexture1DType(const CajetaTypePtr& type);
    /// cajeta.gfx.Texture2DArray<T>: an Arrayed image, coords carrying a layer.
    bool isTexture2DArrayType(const CajetaTypePtr& type);
    /// cajeta.gfx.TextureCube<T>: 6 faces sampled by direction, never fetched.
    bool isTextureCubeType(const CajetaTypePtr& type);
    /// cajeta.gfx.Sampler: the filtering and addressing configuration.
    bool isSamplerType(const CajetaTypePtr& type);

    /// cajeta.xpu.Image2D: the writable twin of Texture2D, bound as a
    /// STORAGE_IMAGE descriptor and written via `img.store(x, y, value)`.
    bool isImageType(const CajetaTypePtr& type);

    /// cajeta.xpu.AccelerationStructure: a descriptor-bound BVH, admissible.
    bool isAccelStructType(const CajetaTypePtr& type);
    /// RayQuery, CooperativeMatrix<T,Rows,Cols,Use> and Tile<T,Rows,Cols> are
    /// device-only kernel-body LOCALS, never kernel args. Tile is the same
    /// fragment as CooperativeMatrix with its Use inferred from Group.mac.
    bool isRayQueryType(const CajetaTypePtr& type);
    bool isCooperativeMatrixType(const CajetaTypePtr& type);
    bool isTileType(const CajetaTypePtr& type);

    /// Validates every parameter of `method`, throwing cajeta::Exception
    /// "XPU-K01" on the first inadmissible one. A no-op for a non-@Kernel
    /// method, so a call site may invoke it on anything about to be codegen'd.
    void validateKernelParams(const MethodPtr& method);

} // namespace xpu
} // namespace cajeta
