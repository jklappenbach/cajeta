// The xpu kernel gate -- see the header for the rule and its provenance.

#include "XpuKernelGate.h"
#include "XpuAttributes.h"

#include "cajeta/error/Exception.h"
#include "cajeta/type/Annotatable.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cajeta {
namespace xpu {

namespace {

int g_override = -1;   // -1 = ask the environment, 0 = error, 1 = warn
unsigned g_errors = 0;
std::vector<std::string> g_failures;

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// The backend names a kernel may write in @Unlowered, folded onto the four
// the registrations report under.
std::string canonicalBackend(const std::string& name) {
    const std::string n = lower(name);
    if (n == "nvidia" || n == "cuda") return "nvptx";
    if (n == "amd" || n == "hip" || n == "rocm") return "amdgpu";
    if (n == "spirv" || n == "spir-v") return "vulkan";
    return n;
}

// The tracked item @Unlowered holds this kernel with for `backend`, or empty.
// An @Unlowered that names no tracked item holds nothing: the point of the
// declaration is the item.
std::string unloweredHeldBy(const Annotatable& kernel, const std::string& backend) {
    auto ann = kernel.findAnnotation(XpuAttr::Unlowered);
    if (!ann) return "";
    const std::string tracked = ann->getString("tracked");
    if (tracked.empty()) return "";
    const std::string be = canonicalBackend(backend);
    if (auto* arg = ann->findArg("backend")) {
        if (arg->kind == AnnotationArgKind::String && canonicalBackend(arg->strVal) == be)
            return tracked;
        if (arg->kind == AnnotationArgKind::StringList)
            for (const auto& s : arg->strList)
                if (canonicalBackend(s) == be) return tracked;
    }
    return "";
}

bool hasUnloweredFor(const Annotatable& kernel, const std::string& backend) {
    return !unloweredHeldBy(kernel, backend).empty();
}

std::string unboundedHeldBy(const Annotatable& kernel) {
    auto ann = kernel.findAnnotation(XpuAttr::Unbounded);
    if (!ann) return "";
    return ann->getString("tracked");
}

void fail(const std::string& kernelName, const std::string& what) {
    ++g_errors;
    g_failures.push_back(kernelName + " (" + what + ")");
}

} // namespace

bool kernelGateWarns() {
    if (g_override >= 0) return g_override == 1;
    const char* v = std::getenv("CAJETA_XPU_KERNEL_GATE");
    return v && std::strcmp(v, "warn") == 0;
}

void setKernelGateWarns(bool on) { g_override = on ? 1 : 0; }
void clearKernelGateWarnsOverride() { g_override = -1; }

void resetKernelGate() {
    g_errors = 0;
    g_failures.clear();
}

unsigned kernelGateErrors() { return g_errors; }
const std::vector<std::string>& kernelGateFailures() { return g_failures; }

void reportUnloweredKernel(const Annotatable& kernel,
                           const std::string& kernelName,
                           const std::string& backend,
                           const std::string& reason) {
    const std::string held = unloweredHeldBy(kernel, backend);
    if (!held.empty()) {
        fprintf(stderr,
                "cajeta: note: [xpu-kernel-skipped] %s: no %s device code — %s "
                "[tracked: %s]\n",
                kernelName.c_str(), backend.c_str(), reason.c_str(), held.c_str());
        return;
    }
    if (kernelGateWarns()) {
        fprintf(stderr,
                "cajeta: warning: [xpu-kernel-skipped] %s: no %s device code — %s\n",
                kernelName.c_str(), backend.c_str(), reason.c_str());
        return;
    }
    fprintf(stderr,
            "cajeta: error: [xpu-kernel-skipped] %s: no %s device code — %s. "
            "A kernel with no device code for a declared backend fails the build "
            "(CAJETA_ERROR_XPU_KERNEL_GATE, xpu-kernel-adaptor 4.2.2); hold it "
            "with @Unlowered(backend = \"%s\", tracked = \"<plan item>\") on the "
            "kernel, or CAJETA_XPU_KERNEL_GATE=warn for a sweep\n",
            kernelName.c_str(), backend.c_str(), reason.c_str(), backend.c_str());
    fail(kernelName, "no " + backend + " device code");
}

void noteKernelLowered(const Annotatable& kernel,
                       const std::string& kernelName,
                       const std::string& backend) {
    const std::string held = unloweredHeldBy(kernel, backend);
    if (held.empty()) return;
    if (kernelGateWarns()) {
        fprintf(stderr,
                "cajeta: warning: [xpu-kernel-skipped] %s: STALE: it has %s device code "
                "now; remove @Unlowered(backend = \"%s\", tracked = \"%s\")\n",
                kernelName.c_str(), backend.c_str(), backend.c_str(), held.c_str());
        return;
    }
    fprintf(stderr,
            "cajeta: error: [xpu-kernel-skipped] %s: STALE: it has %s device code "
            "now; remove @Unlowered(backend = \"%s\", tracked = \"%s\") "
            "(CAJETA_ERROR_XPU_KERNEL_GATE)\n",
            kernelName.c_str(), backend.c_str(), backend.c_str(), held.c_str());
    fail(kernelName, "stale @Unlowered for " + backend);
}

void reportUnboundedKernel(const Annotatable& kernel,
                           const std::string& kernelName,
                           unsigned sites) {
    static const char* kBody =
        "launched with a non-constant block at %u site(s) and no "
        "@Occupancy(maxThreads): the compiler cannot bound the block, so amdgpu "
        "budgets registers for the part's full 1024-thread ceiling and nvptx "
        "applies no bound. Declare the kernel's structural ceiling, "
        "@Occupancy(maxThreads = N), in the same change that derives the block "
        "(xpu-kernel-adaptor 7.0.1)";
    const std::string held = unboundedHeldBy(kernel);
    if (!held.empty()) {
        fprintf(stderr, "cajeta: note: [xpu-kernel-unbounded] %s: ", kernelName.c_str());
        fprintf(stderr, kBody, sites);
        fprintf(stderr, " [tracked: %s]\n", held.c_str());
        return;
    }
    if (kernelGateWarns()) {
        fprintf(stderr, "cajeta: warning: [xpu-kernel-unbounded] %s: ", kernelName.c_str());
        fprintf(stderr, kBody, sites);
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "cajeta: error: [xpu-kernel-unbounded] %s: ", kernelName.c_str());
    fprintf(stderr, kBody, sites);
    fprintf(stderr,
            ", or hold it with @Unbounded(tracked = \"<plan item>\") "
            "(CAJETA_ERROR_XPU_KERNEL_GATE)\n");
    fail(kernelName, "unbounded block, no ceiling");
}

void noteKernelBounded(const Annotatable& kernel,
                       const std::string& kernelName,
                       const std::string& why) {
    const std::string held = unboundedHeldBy(kernel);
    if (held.empty()) return;
    if (kernelGateWarns()) {
        fprintf(stderr,
                "cajeta: warning: [xpu-kernel-unbounded] %s: STALE: %s; remove "
                "@Unbounded(tracked = \"%s\")\n",
                kernelName.c_str(), why.c_str(), held.c_str());
        return;
    }
    fprintf(stderr,
            "cajeta: error: [xpu-kernel-unbounded] %s: STALE: %s; remove "
            "@Unbounded(tracked = \"%s\") (CAJETA_ERROR_XPU_KERNEL_GATE)\n",
            kernelName.c_str(), why.c_str(), held.c_str());
    fail(kernelName, "stale @Unbounded");
}

void throwIfKernelGateFailed() {
    if (g_errors == 0) return;
    std::string names;
    for (const auto& f : g_failures) {
        if (!names.empty()) names += ", ";
        names += f;
    }
    const unsigned n = g_errors;
    resetKernelGate();
    throw cajeta::Exception(
        std::to_string(n) + " kernel(s) failed the xpu kernel gate: " + names
        + ". Each is named above with its backend and reason "
          "(xpu-kernel-adaptor 4.2.2)",
        "CAJETA_ERROR_XPU_KERNEL_GATE");
}

} // namespace xpu
} // namespace cajeta
