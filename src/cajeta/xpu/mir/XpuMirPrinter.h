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
        // Every kernel in `module`, in declaration order, each rendered by the kernel
        // overload and separated by a blank line.
        static std::string print(const XpuMirModule& module);
        // One kernel block: a `kernel <canonicalName>` header, then the wave, backend,
        // param and body-op lines, each indented two spaces.
        static std::string print(const XpuMirKernel& kernel);
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
