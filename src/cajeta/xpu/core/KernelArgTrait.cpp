// KernelArg admissibility — see header for the trait.

#include "KernelArgTrait.h"
#include "XpuAttributes.h"

#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaArray.h"
#include "../../method/Method.h"
#include "../../error/Exception.h"

#include <sstream>

namespace cajeta {
namespace xpu {

namespace {

/// Prefix match, so both the plain template and its instantiations admit.
bool isBufferInstantiation(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.KernelBuffer";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}

/// The texture types are templated on their texel scalar, so a canonical is
/// either bare (default-filled) or `<...>`; Sampler and Image2D are not.
bool isTextureCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.gfx.Texture2D";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isTexture3DCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.gfx.Texture3D";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isTexture1DCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.gfx.Texture1D";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isTexture2DArrayCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.gfx.Texture2DArray";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isTextureCubeCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.gfx.TextureCube";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isSamplerCanonical(const std::string& canonical) {
    return canonical == "cajeta.gfx.Sampler";
}
bool isImageCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.Image2D";
}
/// A descriptor-bound BVH handle, and an admissible kernel argument.
bool isAccelStructCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.AccelerationStructure";
}
bool isRayQueryCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.RayQuery";
}
/// A device-only kernel-local, never an argument; parameterized, so prefix-matched.
bool isCooperativeMatrixCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.CooperativeMatrix";
    return canonical.compare(0, kPrefix.size(), kPrefix) == 0;
}
/// The author-facing cooperative fragment; also a device-only kernel-local.
bool isTileCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.Tile";
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    // Exactly the class or its `<...>`, never a longer name starting "Tile".
    return canonical.size() == kPrefix.size()
        || canonical[kPrefix.size()] == '<';
}

/// Does `klass`'s implemented-interfaces list contain cajeta.xpu.KernelArg?
bool implementsKernelArg(const std::shared_ptr<CajetaClass>& klass) {
    if (!klass) return false;
    for (auto& iface : klass->getImplementedInterfaces()) {
        if (!iface) continue;
        if (iface->toCanonical() == "cajeta.xpu.KernelArg") {
            return true;
        }
    }
    return false;
}

/// A non-interface, non-Buffer class with no inherited fields and at least one,
/// every instance field a primitive or a nested @ValueType POD. Marshalling
/// strips an ordinary class's vtable word, so it does not affect admissibility.
bool isPodStruct(const std::shared_ptr<CajetaClass>& klass) {
    if (!klass) return false;
    if (klass->isInterface()) return false;
    if (isBufferInstantiation(klass->toCanonical())) return false;
    if (klass->countInheritedFields() != 0) return false;  // no inheritance v1
    bool sawField = false;
    for (auto& prop : klass->getPropertyList()) {
        if (!prop || prop->isStatic()) continue;
        sawField = true;
        auto ft = prop->getType();
        if (!ft) return false;
        if (ft->getTypeFlags() & PRIMITIVE_FLAG) continue;
        // A value-type field is still flat by-value POD all the way down.
        if (ft->getTypeFlags() & VALUE_TYPE_FLAG) {
            if (isPodStruct(std::dynamic_pointer_cast<CajetaClass>(ft))) continue;
        }
        return false;
    }
    return sawField;
}

} // namespace

bool isKernelArgAdmissible(const CajetaTypePtr& type) {
    if (!type) return false;

    if (type->getTypeFlags() & PRIMITIVE_FLAG) return true;

    // An array is admissible exactly when its element type is.
    auto array = std::dynamic_pointer_cast<CajetaArray>(type);
    if (array) {
        return isKernelArgAdmissible(array->getElementType());
    }

    auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
    if (klass) {
        const std::string canonical = type->toCanonical();
        if (isBufferInstantiation(canonical)) return true;
        // Matched by name BEFORE the POD-struct check: Sampler is structurally
        // a POD, but must take the sampler-descriptor path instead.
        if (isTextureCanonical(canonical)) return true;
        if (isTexture3DCanonical(canonical)) return true;
        if (isTexture1DCanonical(canonical)) return true;
        if (isTexture2DArrayCanonical(canonical)) return true;
        if (isTextureCubeCanonical(canonical)) return true;
        if (isImageCanonical(canonical)) return true;
        if (isSamplerCanonical(canonical)) return true;
        if (isAccelStructCanonical(canonical)) return true;
        if (isPodStruct(klass)) return true;
        // `implements KernelArg` still requires a by-value POD: it is the only
        // shape the marshaller lowers, and a non-POD would be silently dropped.
        if (implementsKernelArg(klass) && isPodStruct(klass)) return true;
    }

    return false;
}

bool isPodStructType(const CajetaTypePtr& type) {
    if (!type) return false;
    // Sampler matches isPodStruct structurally (primitive fields, no
    // inheritance) but is NOT a by-value POD arg — it is a sampler descriptor.
    if (isSamplerCanonical(type->toCanonical())) return false;
    return isPodStruct(std::dynamic_pointer_cast<CajetaClass>(type));
}

bool isTextureType(const CajetaTypePtr& type) {
    return type && isTextureCanonical(type->toCanonical());
}

bool isTexture3DType(const CajetaTypePtr& type) {
    return type && isTexture3DCanonical(type->toCanonical());
}

bool isTexture1DType(const CajetaTypePtr& type) {
    return type && isTexture1DCanonical(type->toCanonical());
}

bool isTexture2DArrayType(const CajetaTypePtr& type) {
    return type && isTexture2DArrayCanonical(type->toCanonical());
}

bool isTextureCubeType(const CajetaTypePtr& type) {
    return type && isTextureCubeCanonical(type->toCanonical());
}

bool isSamplerType(const CajetaTypePtr& type) {
    return type && isSamplerCanonical(type->toCanonical());
}

bool isImageType(const CajetaTypePtr& type) {
    return type && isImageCanonical(type->toCanonical());
}

bool isAccelStructType(const CajetaTypePtr& type) {
    return type && isAccelStructCanonical(type->toCanonical());
}

bool isRayQueryType(const CajetaTypePtr& type) {
    return type && isRayQueryCanonical(type->toCanonical());
}

bool isCooperativeMatrixType(const CajetaTypePtr& type) {
    return type && isCooperativeMatrixCanonical(type->toCanonical());
}

bool isTileType(const CajetaTypePtr& type) {
    return type && isTileCanonical(type->toCanonical());
}

void validateKernelParams(const MethodPtr& method) {
    if (!method) return;
    if (!isKernel(*method)) return;

    for (auto& param : method->getParameterList()) {
        if (!param) continue;
        // @Kernel methods must be static, but that check is separate and the
        // list may already carry `this` from method-shape post-processing.
        if (param->getName() == "this") continue;

        auto t = param->getType();
        if (!isKernelArgAdmissible(t)) {
            std::ostringstream msg;
            msg << "@Kernel parameter '" << param->getName()
                << "' has type '"
                << (t ? t->toCanonical() : std::string("<unknown>"))
                << "' which is not admissible as a kernel argument. "
                << "Admissible types: primitives, "
                << "cajeta.xpu.KernelBuffer<T>, cajeta.gfx.Texture2D, "
                << "cajeta.xpu.Image2D, cajeta.gfx.Sampler, "
                << "cajeta.xpu.AccelerationStructure, POD structs (a class with "
                << "only primitive fields and no inheritance), or any type "
                << "that implements cajeta.xpu.KernelArg.";
            throw cajeta::Exception(msg.str(), "XPU-K01");
        }
    }
}

} // namespace xpu
} // namespace cajeta
