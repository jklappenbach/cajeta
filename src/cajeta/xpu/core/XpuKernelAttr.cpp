// Implementation of XpuKernelAttr — see the header for shape and rationale.

#include "XpuKernelAttr.h"

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaConstantType.h"
#include "cajeta/type/ConstExpr.h"

#include <algorithm>
#include <cctype>

namespace cajeta {
namespace xpu {

namespace {

std::string toLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

std::optional<XpuBackend> parseBackend(const std::string& name) {
    const std::string n = toLowerAscii(name);
    if (n == "nvidia") return XpuBackend::Nvidia;
    if (n == "amd")    return XpuBackend::Amd;
    if (n == "vulkan") return XpuBackend::Vulkan;
    return std::nullopt;
}

const char* backendName(XpuBackend b) {
    switch (b) {
        case XpuBackend::Nvidia: return "nvidia";
        case XpuBackend::Amd:    return "amd";
        case XpuBackend::Vulkan: return "vulkan";
    }
    return "?";
}

std::optional<XpuKernelAttr> XpuKernelAttr::from(const Annotatable& a) {
    return fromWith(a, nullptr);
}

std::optional<XpuKernelAttr> XpuKernelAttr::from(const Method& m) {
    return fromWith(m, m.getParent().get());
}

std::optional<XpuKernelAttr> XpuKernelAttr::fromWith(const Annotatable& a,
                                                     const CajetaClass* cls) {
    if (!isKernel(a)) return std::nullopt;

    XpuKernelAttr out;

    // @Wave(width = N): an absent or wrong-kind arg leaves waveWidth_ unset, and
    // lowering then picks the target default.
    if (auto wave = a.findAnnotation(XpuAttr::Wave)) {
        if (auto* arg = wave->findArg("width")) {
            if (arg->kind == AnnotationArgKind::Int64) {
                out.waveWidth_ = static_cast<int>(arg->i64Val);
            }
        }
    }

    // @Backend("nvidia") and @Backend({"nvidia", "amd"}) both fill backends_.
    // Unrecognized names are dropped here and diagnosed at MIR-build time, which
    // is the first point with a user-facing location.
    if (auto backend = a.findAnnotation(XpuAttr::Backend)) {
        const auto& single = backend->getString();
        if (!single.empty()) {
            if (auto b = parseBackend(single)) {
                out.backends_.push_back(*b);
            }
        }
        for (const auto& s : backend->getStringList()) {
            if (auto b = parseBackend(s)) {
                out.backends_.push_back(*b);
            }
        }
    }

    // @Occupancy(maxThreads, maxWaves, minResident, maxRegisters): optional named Int64 args; non-positive values are ignored.
    if (auto occ = a.findAnnotation(XpuAttr::Occupancy)) {
        // An argument that is not a literal arrives as its source text, and
        // is a constant expression over the class template's non-type
        // parameters (`maxWaves = (TM / WM) * (TN / WN)`), evaluated for
        // this instantiation (xpu-kernel-independence §3.3).
        ConstLookup bound;
        if (cls) {
            bound = [cls](const std::string& n) -> std::optional<int64_t> {
                const auto& params = cls->getTypeParameters();
                const auto& args = cls->getTypeArguments();
                for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
                    if (!params[i].isNonType || params[i].name != n) continue;
                    if (auto c = std::dynamic_pointer_cast<CajetaConstantType>(args[i]))
                        return c->getValue();
                }
                return std::nullopt;
            };
        }
        auto readU = [&](const char* key) -> std::optional<unsigned> {
            if (auto* arg = occ->findArg(key)) {
                if (arg->kind == AnnotationArgKind::Int64 && arg->i64Val > 0) {
                    return static_cast<unsigned>(arg->i64Val);
                }
                if (arg->kind == AnnotationArgKind::String && !arg->strVal.empty()) {
                    ConstExprResult r = evalConstExpr(arg->strVal, bound);
                    if (!r.ok) {
                        if (out.occupancyError_.empty())
                            out.occupancyError_ = "@Occupancy(" + std::string(key) + " = "
                                + arg->strVal + ") is not a compile-time constant: "
                                + r.error;
                        return std::nullopt;
                    }
                    if (r.value > 0) return static_cast<unsigned>(r.value);
                }
            }
            return std::nullopt;
        };
        out.maxThreads_   = readU("maxThreads");
        out.minResident_  = readU("minResident");
        out.maxRegisters_ = readU("maxRegisters");
        out.maxWaves_     = readU("maxWaves");
    }

    return out;
}

bool XpuKernelAttr::emitsFor(XpuBackend b) const {
    if (backends_.empty()) return true;
    return std::find(backends_.begin(), backends_.end(), b)
        != backends_.end();
}

} // namespace xpu
} // namespace cajeta
