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

    // Class/interface references — admit Buffer<T> by name prefix, and
    // user-defined classes/interfaces that implement the KernelArg
    // marker interface.
    auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
    if (klass) {
        if (isBufferInstantiation(type->toCanonical())) return true;
        if (implementsKernelArg(klass)) return true;
    }

    return false;
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
                << "cajeta.xpu.core.Buffer<T>, or any type that "
                << "implements cajeta.xpu.core.KernelArg.";
            throw cajeta::Exception(msg.str(), "XPU-K01");
        }
    }
}

} // namespace xpu
} // namespace cajeta
