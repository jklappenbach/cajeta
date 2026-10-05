#include "cajeta/xpu/core/DeclarationCheck.h"

#include "cajeta/error/Exception.h"

#include <cstdlib>
#include <cstring>

namespace cajeta {
namespace xpu {

namespace {

const char* kErrorId = "CAJETA_ERROR_XPU_DECLARATION_DROPPED";

// The text between `.entry <name>(` and the `{` that opens its body: the
// directives a kernel's bounds are printed as live there, after the
// parameter list.
std::string ptxEntryHeader(const std::string& ptx, const std::string& entryName) {
    const std::string key = ".entry " + entryName + "(";
    size_t at = ptx.find(key);
    if (at == std::string::npos) return "";
    size_t open = ptx.find('{', at);
    return ptx.substr(at, open == std::string::npos ? std::string::npos : open - at);
}

} // namespace

bool xpuFault(const char* fault) {
    const char* f = std::getenv("CAJETA_XPU_FAULT");
    return f && std::strcmp(f, fault) == 0;
}

void checkOccupancyInPtx(const XpuKernelAttr& attr, const std::string& entryName,
                         const std::string& ptx) {
    if (!attr.hasOccupancy()) return;
    const std::string header = ptxEntryHeader(ptx, entryName);
    auto missing = [&](const char* declared, const char* directive) {
        throw cajeta::Exception(
            "kernel " + entryName + " declares " + declared + " and its nvptx PTX does not carry "
            "it: no " + directive + " in the entry's header. The bound was dropped between the IR "
            "and the PTX, so the launch would run unbounded while the manifest says otherwise.",
            kErrorId);
    };
    if (header.empty())
        throw cajeta::Exception("kernel " + entryName + " declares @Occupancy and no .entry of that "
                                "name is in its nvptx PTX", kErrorId);
    if (attr.hasThreadBound() && header.find(".maxntid") == std::string::npos)
        missing(attr.maxThreads() ? "@Occupancy(maxThreads)" : "@Occupancy(maxWaves)", ".maxntid");
    if (attr.minResident() && header.find(".minnctapersm") == std::string::npos)
        missing("@Occupancy(minResident)", ".minnctapersm");
    if (attr.maxRegisters() && header.find(".maxnreg") == std::string::npos)
        missing("@Occupancy(maxRegisters)", ".maxnreg");
}

void checkWaveWidthBuilt(const XpuKernelAttr& attr, const std::string& entryName,
                         const std::string& backend, unsigned builtWidth) {
    if (!attr.waveWidth() || builtWidth < 2) return;
    const int declared = *attr.waveWidth();
    if ((unsigned) declared == builtWidth) return;
    throw cajeta::Exception(
        "kernel " + entryName + " declares @Wave(width = " + std::to_string(declared) + ") and "
        "the " + backend + " backend built it at " + std::to_string(builtWidth) + " lanes. "
        "The declaration did not pin the wave, so a reduce across the declared lanes would "
        "read another width's partial sums.",
        kErrorId);
}

} // namespace xpu
} // namespace cajeta
