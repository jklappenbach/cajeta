//
// KernelArg admissibility — see header for rationale.
//

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

// Buffer<T> instantiations have canonical names of the form
// "cajeta.xpu.core.Buffer<...>". The plain template (uninstantiated)
// is just "cajeta.xpu.core.Buffer". Match the prefix to admit both.
bool isBufferInstantiation(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.core.Buffer";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    // Exact match or `Buffer<...>` follow-on
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}

// Texture2D is now templated on its texel scalar — `Texture2D<T = float32>` —
// so an instance's canonical is either the bare `cajeta.xpu.core.Texture2D`
// (default-filled to `<float32>`) or `cajeta.xpu.core.Texture2D<...>`. Match the
// prefix like Buffer. Sampler is NOT a template (exact match).
bool isTextureCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.core.Texture2D";
    if (canonical.size() < kPrefix.size()) return false;
    if (canonical.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (canonical.size() == kPrefix.size()) return true;
    return canonical[kPrefix.size()] == '<';
}
bool isSamplerCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.core.Sampler";
}
// Image2D (writable images) — the writable twin of Texture2D, matched by exact
// canonical name (not a template). A 2-D float storage image, bound as a
// STORAGE_IMAGE descriptor and written via `img.store(x, y, value)`.
bool isImageCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.core.Image2D";
}
// AccelerationStructure (cajeta-gpu Part C) — a descriptor-bound BVH handle,
// admissible as a kernel argument (it lowers to an OpTypeAccelerationStructureKHR
// descriptor on Vulkan). RayQuery is NOT a kernel arg — it is a device-only
// function-local — so it has no admissibility entry; it is recognized only by
// the device lowerer (isRayQueryType) when it appears as a kernel-body local.
bool isAccelStructCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.core.AccelerationStructure";
}
bool isRayQueryCanonical(const std::string& canonical) {
    return canonical == "cajeta.xpu.core.RayQuery";
}
// CooperativeMatrix (cajeta-gpu Part C) — like RayQuery, a device-only
// kernel-local (a subgroup-cooperative matrix-core tile), NOT a kernel arg. It
// is parameterized (`CooperativeMatrix<T, Rows, Cols, Use>`), so an instance's
// canonical carries a `<...>` suffix; match the prefix. Recognized only by the
// device lowerer when it appears as a kernel-body local.
bool isCooperativeMatrixCanonical(const std::string& canonical) {
    static const std::string kPrefix = "cajeta.xpu.core.CooperativeMatrix";
    return canonical.compare(0, kPrefix.size(), kPrefix) == 0;
}

// A class implements the KernelArg marker interface if its
// implemented-interfaces list contains cajeta.xpu.core.KernelArg.
// `getImplementedInterfaces()` returns the concrete CajetaClass
// pointers for interfaces (CajetaInterface is just a CajetaClass with
// isInterface()=true, so we walk those).
bool implementsKernelArg(const std::shared_ptr<CajetaClass>& klass) {
    if (!klass) return false;
    for (auto& iface : klass->getImplementedInterfaces()) {
        if (!iface) continue;
        if (iface->toCanonical() == "cajeta.xpu.core.KernelArg") {
            return true;
        }
    }
    return false;
}

// A plain POD struct admissible by value (Item 7): a non-interface,
// non-Buffer class with no inherited fields (so every instance field is
// in its own propertyList) and at least one field, all of whose
// non-static instance fields are primitives (scalars / Vector — both carry
// PRIMITIVE_FLAG) OR nested @ValueType PODs (S5, recursive). A @ValueType
// class is itself such a struct: vtable-free, by-value, all-POD fields — so
// it AND a struct that contains a value-type field marshal by value to a
// field-reading kernel. Inheritance stays out of scope for v1. The vtable
// word ordinary classes carry is stripped during marshalling, so it does
// not affect admissibility (value types carry none — see CajetaClass
// hasVtablePointerAtSlotZero).
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
        // Scalar primitive or Vector (both PRIMITIVE_FLAG): admissible directly.
        if (ft->getTypeFlags() & PRIMITIVE_FLAG) continue;
        // Nested @ValueType field: recurse — a value-type-containing POD is
        // still flat by-value POD all the way down.
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

    // Primitives — int*, uint*, float*, bool, etc. Identified by the
    // PRIMITIVE_FLAG bit on the type's flag word.
    if (type->getTypeFlags() & PRIMITIVE_FLAG) return true;

    // Primitive arrays — `float32[]`, `int32[]`, etc. Used by the
    // CPU-emulation kernels in step 7 (and by users who write
    // device code against raw host-shaped arrays). Admissibility is
    // recursive on the element type: `Buffer<float32>[]` is fine,
    // `SomeRandomClass[]` isn't.
    auto array = std::dynamic_pointer_cast<CajetaArray>(type);
    if (array) {
        return isKernelArgAdmissible(array->getElementType());
    }

    // Class/interface references — admit Buffer<T> by name prefix,
    // plain POD structs by value (Item 7), and user-defined
    // classes/interfaces that implement the KernelArg marker interface.
    auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
    if (klass) {
        const std::string canonical = type->toCanonical();
        if (isBufferInstantiation(canonical)) return true;
        // Texture2D / Sampler matched by name BEFORE the POD-struct check —
        // Sampler is a plain primitive-field class (so isPodStruct would also
        // admit it), but it must take the sampler-descriptor path, not the
        // by-value POD path.
        if (isTextureCanonical(canonical)) return true;
        if (isImageCanonical(canonical)) return true;
        if (isSamplerCanonical(canonical)) return true;
        if (isAccelStructCanonical(canonical)) return true;
        if (isPodStruct(klass)) return true;
        // A user type marked `implements KernelArg` is admissible only if it is
        // ALSO a by-value POD (all-primitive fields) — that is the only shape the
        // launch-site marshaller and the device deviceStructInfo can lower. A
        // non-POD KernelArg would be admitted here but then silently dropped
        // (the device lowerer throws XPU-N01, which the registration swallows).
        // Rejecting it surfaces a clean diagnostic instead. (M11)
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

void validateKernelParams(const MethodPtr& method) {
    if (!method) return;
    if (!isKernel(*method)) return;

    for (auto& param : method->getParameterList()) {
        if (!param) continue;
        // Skip the implicit `this` parameter on instance methods —
        // @Kernel methods are required to be static, but the check
        // for that is independent of arg admissibility, and the
        // parameter list may already carry `this` from method-shape
        // post-processing.
        if (param->getName() == "this") continue;

        auto t = param->getType();
        if (!isKernelArgAdmissible(t)) {
            std::ostringstream msg;
            msg << "@Kernel parameter '" << param->getName()
                << "' has type '"
                << (t ? t->toCanonical() : std::string("<unknown>"))
                << "' which is not admissible as a kernel argument. "
                << "Admissible types: primitives, "
                << "cajeta.xpu.core.Buffer<T>, cajeta.xpu.core.Texture2D, "
                << "cajeta.xpu.core.Image2D, cajeta.xpu.core.Sampler, "
                << "cajeta.xpu.core.AccelerationStructure, POD structs (a class with "
                << "only primitive fields and no inheritance), or any type "
                << "that implements cajeta.xpu.core.KernelArg.";
            throw cajeta::Exception(msg.str(), "XPU-K01");
        }
    }
}

} // namespace xpu
} // namespace cajeta
