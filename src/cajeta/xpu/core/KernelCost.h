// KernelCost - the work model a kernel's body states (xpu-tile-manifest spec §4).

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelBufferCost {
        std::string param;
        unsigned storedBits = 0;   // @Stored(bits = N), else the element width
        std::string bytesRead = "0";
        std::string bytesWritten = "0";
    };

    // Each cost is an expression over the kernel's parameters, or "measured".
    struct KernelCost {
        std::string costUnit = "item";   // group | item
        std::string matrixMacs = "0";
        std::string vectorMacs = "0";
        std::string intOps = "0";
        std::optional<std::string> workAxisParam;
        std::vector<KernelBufferCost> buffers;   // buffer parameters, declaration order
    };

    /** Derives `kernel`'s work model from its body. `groupWidth` is the target's folded
     *  Group width; without one, a field that depends on it is "measured". Never throws. */
    KernelCost deriveKernelCost(const MethodPtr& kernel, std::optional<unsigned> groupWidth);

    /** Evaluates a cost expression against launch arguments; `ok` is false for
     *  "measured", an unbound name or a malformed expression. */
    double evaluateCost(const std::string& expr, const std::map<std::string, double>& args,
                        bool* ok = nullptr);

    /** The buffer parameter a `@Stored` annotation is misplaced on or misstates, else empty. */
    std::string storedAnnotationError(const MethodPtr& kernel);

} // namespace xpu
} // namespace cajeta
