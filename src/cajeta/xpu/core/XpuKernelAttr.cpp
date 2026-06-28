//
// Implementation of XpuKernelAttr — see header for shape and rationale.
//

#include "XpuKernelAttr.h"
#include "KernelTuner.h"

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
    if (!isKernel(a)) return std::nullopt;

    XpuKernelAttr out;

    // @Wave(width = N) — captured by parseAnnotationInstance as a
    // named Int64 arg. Absent annotation or missing/wrong-kind arg
    // both leave waveWidth_ unset (lowering picks a target default).
    if (auto wave = a.findAnnotation(XpuAttr::Wave)) {
        if (auto* arg = wave->findArg("width")) {
            if (arg->kind == AnnotationArgKind::Int64) {
                out.waveWidth_ = static_cast<int>(arg->i64Val);
            }
        }
    }

    // @Backend("nvidia") — unnamed string arg, single form.
    // @Backend({"nvidia", "amd"}) — StringList form. Both shapes
    // produce the same backends_ vector; unrecognized names are
    // dropped silently (a future validation pass surfaces them as
    // diagnostics at MIR-build time, where the user-facing location
    // is available).
    if (auto backend = a.findAnnotation(XpuAttr::Backend)) {
        // Single-string form: @Backend("nvidia"). Arg captured with
        // empty name; findArg("value") routes through.
        const auto& single = backend->getString();
        if (!single.empty()) {
            if (auto b = parseBackend(single)) {
                out.backends_.push_back(*b);
            }
        }
        // Multi-string form: @Backend({"nvidia", "amd"}). Captured as
        // StringList on the unnamed arg; the typed getStringList
        // accessor returns an empty vector when absent or wrong-kind.
        for (const auto& s : backend->getStringList()) {
            if (auto b = parseBackend(s)) {
                out.backends_.push_back(*b);
            }
        }
    }

    // @Occupancy(maxThreads = N, minResident = N, maxRegisters = N) — each a
    // named Int64 arg; all optional. Non-positive values are ignored.
    if (auto occ = a.findAnnotation(XpuAttr::Occupancy)) {
        auto readU = [&](const char* key) -> std::optional<unsigned> {
            if (auto* arg = occ->findArg(key)) {
                if (arg->kind == AnnotationArgKind::Int64 && arg->i64Val > 0) {
                    return static_cast<unsigned>(arg->i64Val);
                }
            }
            return std::nullopt;
        };
        out.maxThreads_   = readU("maxThreads");
        out.minResident_  = readU("minResident");
        out.maxRegisters_ = readU("maxRegisters");
    }

    // @Autotune — bare marker, or @Autotune(blocks = {64, 128, 256}) candidate set.
    if (auto autotune = a.findAnnotation(XpuAttr::Autotune)) {
        out.autotune_ = true;
        if (auto* arg = autotune->findArg("blocks")) {
            if (arg->kind == AnnotationArgKind::Int64List) {
                for (int64_t v : arg->i64List)
                    if (v > 0) out.autotuneBlocks_.push_back(
                        static_cast<unsigned>(v));
            }
        }
    }

    return out;
}

unsigned XpuKernelAttr::autotuneMaxThreads() const {
    if (!autotune_) return 0;
    const std::vector<unsigned>& blocks =
        autotuneBlocks_.empty() ? KernelTuner::defaultCandidateBlocks()
                                : autotuneBlocks_;
    unsigned max = 0;
    for (unsigned b : blocks) max = std::max(max, b);
    return max;
}

bool XpuKernelAttr::emitsFor(XpuBackend b) const {
    if (backends_.empty()) return true;
    return std::find(backends_.begin(), backends_.end(), b)
        != backends_.end();
}

} // namespace xpu
} // namespace cajeta
