// XpuMirPrinter — text dump of an XpuMirModule / XpuMirKernel, backing the
// `--xpu-emit=mir` debug mode: one block per kernel, carrying wave, backend,
// param and body-op lines. Greppable by tests, but not a committed wire format.

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
