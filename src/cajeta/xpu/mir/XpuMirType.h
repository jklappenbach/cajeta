// XPU MIR type wrapper: a CajetaTypePtr plus an explicit address-space qualifier,
// which CajetaXPU.md §3.1.2 makes part of the type — AddressSpaceLowerPass reads it
// to emit the right `addrspace(N)`. A value type, cheap enough to copy freely.

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
