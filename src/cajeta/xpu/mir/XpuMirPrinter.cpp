//
// XpuMirPrinter — see header.
//

#include "XpuMirPrinter.h"

#include "../core/AddressSpace.h"
#include "../core/XpuKernelAttr.h"

#include "../../type/CajetaType.h"

#include <sstream>

namespace cajeta {
namespace xpu {
namespace mir {

namespace {

void printType(std::ostringstream& out, const XpuMirType& t) {
    out << (t.underlying ? t.underlying->toCanonical() : std::string("<?>"));
    if (t.addressSpace != AddressSpace::Generic) {
        out << " addrspace " << addressSpaceName(t.addressSpace);
    }
}

void printOp(std::ostringstream& out, const XpuMirOp& op) {
    out << "  body-op " << opKindName(op.kind);
    switch (op.kind) {
        case OpKind::ThreadId:
        case OpKind::WorkgroupId:
        case OpKind::WorkgroupDim:
            out << '.' << axisLetter(op.axis);
            break;
        case OpKind::LaunchKernel:
            out << ' ' << op.kernelName;
            break;
        case OpKind::StatusBufWrite:
            out << ' ' << op.statusCode;
            break;
        default:
            break;
    }
    out << '\n';
}

} // namespace

std::string XpuMirPrinter::print(const XpuMirKernel& kernel) {
    std::ostringstream out;
    out << "kernel " << kernel.canonicalName << '\n';
    if (kernel.waveWidth.has_value()) {
        out << "  wave " << *kernel.waveWidth << '\n';
    }
    if (!kernel.backends.empty()) {
        out << "  backend ";
        for (size_t i = 0; i < kernel.backends.size(); ++i) {
            if (i) out << ", ";
            out << backendName(kernel.backends[i]);
        }
        out << '\n';
    }
    for (auto& p : kernel.params) {
        out << "  param " << p.name << " : ";
        printType(out, p.type);
        out << '\n';
    }
    for (auto& op : kernel.bodyOps) {
        if (op) printOp(out, *op);
    }
    return out.str();
}

std::string XpuMirPrinter::print(const XpuMirModule& module) {
    std::ostringstream out;
    for (auto& k : module.kernels) {
        if (k) out << print(*k) << '\n';
    }
    return out.str();
}

} // namespace mir
} // namespace xpu
} // namespace cajeta
