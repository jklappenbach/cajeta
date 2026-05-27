//
// XpuMirPrinter — text dump of an XpuMirModule / XpuMirKernel.
//
// Backs the `--xpu-emit=mir` debug mode. Format is one kernel per
// block:
//
//   kernel <canonical>
//     wave 32
//     backend nvidia, amd
//     param <name> : <type> [addrspace <as>]
//     ...
//     body-op <kind> <payload>
//
// Stable enough for tests to grep for known substrings; not
// committed as a wire-format yet (step 9+ may add per-backend
// metadata that lands here).
//

#pragma once

#include <string>

#include "XpuMir.h"

namespace cajeta {
namespace xpu {
namespace mir {

    class XpuMirPrinter {
    public:
        static std::string print(const XpuMirModule& module);
        static std::string print(const XpuMirKernel& kernel);
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
