// The xpu kernel gate -- see the header for the rule and its provenance.

#include "XpuKernelGate.h"
#include "XpuAttributes.h"

#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/type/CajetaClass.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cajeta {
namespace xpu {

namespace {

int g_override = -1;   // -1 = ask the environment, 0 = error, 1 = warn
unsigned g_hostWave = 0;   // the cpu backend's host wave for this build, 0 = unknown
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
    bool named = false;
    if (auto* arg = ann->findArg("backend")) {
        if (arg->kind == AnnotationArgKind::String && canonicalBackend(arg->strVal) == be)
            named = true;
        if (arg->kind == AnnotationArgKind::StringList)
            for (const auto& s : arg->strList)
                if (canonicalBackend(s) == be) named = true;
    }
    if (!named) return "";
    // hostWaveBelow = N: the hold is conditional on the cpu host's wave. On a
    // host at or above N the kernel is expected to lower, and the hold is
    // neither applied nor stale. Below N it holds a kernel that does not
    // lower and permits one that does (noteKernelLowered). Unknown host wave
    // (0): the hold applies.
    if (be == "cpu") {
        if (auto* below = ann->findArg("hostWaveBelow");
                below && below->kind == AnnotationArgKind::Int64) {
            if (g_hostWave != 0 && (int64_t) g_hostWave >= below->i64Val) return "";
        }
    }
    return tracked;
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

const char* const kGateCode = "CAJETA_ERROR_XPU_KERNEL_GATE";

// `backend` alone, or `backend/arch` when the arch is known.
std::string targetOf(const std::string& backend, const std::string& arch) {
    return arch.empty() ? backend : backend + "/" + arch;
}

// One gate line as text, or under --diag-format=json a diagnostic at the kernel naming it.
void gateRecord(const Method& kernel, const char* severity, const std::string& code,
                const std::string& kernelName, const std::string& target,
                const std::string& text) {
    if (!jsonProgressEnabled()) {
        fprintf(stderr, "cajeta: %s: %s\n", severity, text.c_str());
        return;
    }
    SourceSite at = kernel.declarationSite();
    emitJsonDiagnostic(severity, code, text, at.file, at.line, at.column, "project",
                       GeneratedOrigin(), DiagnosticArtifact{"kernel", kernelName, target});
}

} // namespace

bool kernelGateWarns() {
    if (g_override >= 0) return g_override == 1;
    const char* v = std::getenv("CAJETA_XPU_KERNEL_GATE");
    return v && std::strcmp(v, "warn") == 0;
}

void setKernelGateHostWave(unsigned lanes) { g_hostWave = lanes; }
unsigned kernelGateHostWave() { return g_hostWave; }

void setKernelGateWarns(bool on) { g_override = on ? 1 : 0; }
void clearKernelGateWarnsOverride() { g_override = -1; }

void resetKernelGate() {
    g_errors = 0;
    g_failures.clear();
}

unsigned kernelGateErrors() { return g_errors; }
const std::vector<std::string>& kernelGateFailures() { return g_failures; }

void reportUnloweredKernel(const Method& kernel,
                           const std::string& kernelName,
                           const std::string& backend,
                           const std::string& reason,
                           const std::string& arch) {
    const std::string target = targetOf(backend, arch);
    const std::string head = "[xpu-kernel-skipped] " + kernelName + ": no " + backend
        + " device code — " + reason;
    const std::string held = unloweredHeldBy(kernel, backend);
    if (!held.empty()) {
        gateRecord(kernel, "note", "xpu-kernel-skipped", kernelName, target,
                   head + " [tracked: " + held + "]");
        return;
    }
    if (kernelGateWarns()) {
        gateRecord(kernel, "warning", "xpu-kernel-skipped", kernelName, target, head);
        return;
    }
    gateRecord(kernel, "error", kGateCode, kernelName, target,
               head + ". A kernel with no device code for a declared backend fails the build "
               "(CAJETA_ERROR_XPU_KERNEL_GATE, xpu-kernel-adaptor 4.2.2); hold it "
               "with @Unlowered(backend = \"" + backend + "\", tracked = \"<plan item>\") on "
               "the kernel, or CAJETA_XPU_KERNEL_GATE=warn for a sweep");
    fail(kernelName, "no " + backend + " device code");
}

void noteKernelLowered(const Method& kernel,
                       const std::string& kernelName,
                       const std::string& backend,
                       const std::string& arch) {
    const std::string held = unloweredHeldBy(kernel, backend);
    if (held.empty()) return;
    // A CONDITIONAL hold (hostWaveBelow = N) is never stale. Below N it
    // permits lowering rather than demanding a failure: the host's wave is
    // the width LLVM prefers, and an AVX-512 part tuned to prefer 256 bits
    // reads 8 while its 32 registers still let a 32-lane kernel lower
    // (2026-10-03, cajeta-cabra CI 37129095762). At or above N the hold
    // does not apply at all.
    if (canonicalBackend(backend) == "cpu") {
        if (auto ann = kernel.findAnnotation(XpuAttr::Unlowered))
            if (auto* below = ann->findArg("hostWaveBelow");
                    below && below->kind == AnnotationArgKind::Int64)
                return;
    }
    const std::string target = targetOf(backend, arch);
    const std::string head = "[xpu-kernel-skipped] " + kernelName + ": STALE: it has " + backend
        + " device code now; remove @Unlowered(backend = \"" + backend + "\", tracked = \""
        + held + "\")";
    if (kernelGateWarns()) {
        gateRecord(kernel, "warning", "xpu-kernel-skipped", kernelName, target, head);
        return;
    }
    gateRecord(kernel, "error", kGateCode, kernelName, target,
               head + " (CAJETA_ERROR_XPU_KERNEL_GATE)");
    fail(kernelName, "stale @Unlowered for " + backend);
}

void reportUnboundedKernel(const Method& kernel,
                           const std::string& kernelName,
                           unsigned sites) {
    const std::string head = "[xpu-kernel-unbounded] " + kernelName
        + ": launched with a non-constant block at " + std::to_string(sites)
        + " site(s) and no @Occupancy(maxThreads): the compiler cannot bound the block, so "
          "amdgpu budgets registers for the part's full 1024-thread ceiling and nvptx "
          "applies no bound. Declare the kernel's structural ceiling, "
          "@Occupancy(maxThreads = N), in the same change that derives the block "
          "(xpu-kernel-adaptor 7.0.1)";
    const std::string held = unboundedHeldBy(kernel);
    if (!held.empty()) {
        gateRecord(kernel, "note", "xpu-kernel-unbounded", kernelName, "",
                   head + " [tracked: " + held + "]");
        return;
    }
    if (kernelGateWarns()) {
        gateRecord(kernel, "warning", "xpu-kernel-unbounded", kernelName, "", head);
        return;
    }
    gateRecord(kernel, "error", kGateCode, kernelName, "",
               head + ", or hold it with @Unbounded(tracked = \"<plan item>\") "
               "(CAJETA_ERROR_XPU_KERNEL_GATE)");
    fail(kernelName, "unbounded block, no ceiling");
}

void noteKernelBounded(const Method& kernel,
                       const std::string& kernelName,
                       const std::string& why) {
    const std::string held = unboundedHeldBy(kernel);
    if (held.empty()) return;
    const std::string head = "[xpu-kernel-unbounded] " + kernelName + ": STALE: " + why
        + "; remove @Unbounded(tracked = \"" + held + "\")";
    if (kernelGateWarns()) {
        gateRecord(kernel, "warning", "xpu-kernel-unbounded", kernelName, "", head);
        return;
    }
    gateRecord(kernel, "error", kGateCode, kernelName, "",
               head + " (CAJETA_ERROR_XPU_KERNEL_GATE)");
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
