//
// XPU MIR type wrapper.
//
// Wraps a CajetaTypePtr with an explicit address-space qualifier.
// Per CajetaXPU.md §3.1.2, the address-space is part of the type;
// step 5's AddressSpaceLowerPass will inspect these to emit the
// right `addrspace(N)` decoration on the LLVM-IR side.
//
// v1 type is a value type — no allocation, fits in a couple of
// pointers. Builder code stores them in XpuMirKernel::signature
// (one per parameter) and in the lowering ops that need an
// address-space-qualified operand.
//

#pragma once

#include <memory>

#include "../core/AddressSpace.h"

namespace cajeta {
    class CajetaType;
    using CajetaTypePtr = std::shared_ptr<CajetaType>;
}

namespace cajeta {
namespace xpu {
namespace mir {

    struct XpuMirType {
        CajetaTypePtr underlying;
        AddressSpace addressSpace = AddressSpace::Generic;

        XpuMirType() = default;
        XpuMirType(CajetaTypePtr u, AddressSpace as = AddressSpace::Generic)
            : underlying(std::move(u)), addressSpace(as) { }
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
