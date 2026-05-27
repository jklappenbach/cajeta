//
// XpuMirBuilder — see header for shape and rationale.
//

#include "XpuMirBuilder.h"

#include "../core/XpuAttributes.h"
#include "../core/AddressSpace.h"

#include "../../method/Method.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaType.h"
#include "../../type/FormalParameter.h"
#include "../../compile/CajetaModule.h"

namespace cajeta {
namespace xpu {
namespace mir {

namespace {

// Address-space qualifier for a parameter type. Recognized when the
// type's canonical name maps to one of cajeta.xpu.core.{Global,
// Shared, Constant, Private, Generic}. Anything else defaults to
// Generic — including primitives and Buffer<T>, which carry their
// own backend lowering at codegen time.
AddressSpace addressSpaceForType(const CajetaTypePtr& type) {
    if (!type) return AddressSpace::Generic;
    AddressSpace as;
    if (isAddressSpaceCanonical(type->toCanonical(), as)) return as;
    return AddressSpace::Generic;
}

// Build the parameter list, skipping `this` for instance methods
// (defensive — @Kernel methods are required to be static, but the
// validator that enforces that lands later; v1 just drops `this`
// if it slipped in).
std::vector<XpuKernelParam>
buildParams(const MethodPtr& method) {
    std::vector<XpuKernelParam> out;
    out.reserve(method->getParameterList().size());
    for (auto& fp : method->getParameterList()) {
        if (!fp) continue;
        if (fp->getName() == "this") continue;
        XpuKernelParam p;
        p.name = fp->getName();
        p.type = XpuMirType(fp->getType(), addressSpaceForType(fp->getType()));
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace

XpuMirKernelPtr XpuMirBuilder::buildKernelForMethod(const MethodPtr& method) {
    if (!method) return nullptr;
    if (!isKernel(*method)) return nullptr;

    auto k = std::make_shared<XpuMirKernel>();
    k->method = method;
    k->params = buildParams(method);

    // Compose the canonical name as `pkg.Class.method`. The Method
    // doesn't directly carry the full canonical (it has a separate
    // signature-mangled form), so we synthesize it from the parent
    // class's canonical plus the method short name.
    std::string parentCanonical;
    if (method->getParent()) {
        parentCanonical = method->getParent()->toCanonical();
    }
    k->canonicalName = parentCanonical.empty()
        ? method->getName()
        : (parentCanonical + "." + method->getName());

    // Pull @Wave / @Backend values via the typed view from step 1.
    if (auto attr = XpuKernelAttr::from(*method)) {
        k->waveWidth = attr->waveWidth();
        k->backends  = attr->backends();
    }

    // bodyOps stays empty in step 4; step 5's leaf walker populates.
    return k;
}

XpuMirModulePtr XpuMirBuilder::buildForModule(const CajetaModulePtr& module) {
    auto m = std::make_shared<XpuMirModule>();
    if (!module) return m;
    for (auto& method : module->getAllMethods()) {
        if (auto k = buildKernelForMethod(method)) {
            m->kernels.push_back(std::move(k));
        }
    }
    return m;
}

} // namespace mir
} // namespace xpu
} // namespace cajeta
