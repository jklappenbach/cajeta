//
// Shared diagnostics for XPU lowering tests.
//
// Two things live here. The first is the boilerplate every lowering test
// re-types: compile a source string, find a kernel, lower it for a backend,
// hand back its IR and assembly. 73 files in this suite define their own
// `compileForInspection`, differing only in a temp-directory prefix and a
// class file name. Nothing below is a new idea — it is that function, once.
//
// The second is the part worth having: a way to ask WHY a kernel carries a
// stack frame. That question cost a day on 2026-09-19. The spill gate reports
// one number and gives one piece of advice ("cut live registers or pin a
// smaller block"), and for 31 of the 31 kernels then spilling in cajeta-llm
// that advice was wrong. `q4kMatVecKernelIL` paid 64 bytes at vgpr=48, with
// 207 registers of headroom. Three different causes wear the same number:
//
//   * a CONSTRUCT LEGALIZED THROUGH MEMORY — the backend had no instruction
//     for something and spilled it. Writes through %SP, no `ld.local`.
//     Remedy: a lowering override. (lut4's table; a dynamic lane read.)
//   * a SCRATCH TILE the kernel genuinely declares — the portable software
//     CooperativeMatrix, which IS the scratch. Heavy `st.local`/`ld.local`,
//     kilobytes. Remedy: a dtype the backend has a native config for.
//   * REGISTER PRESSURE, the only case the gate's advice fits. Often NO PTX
//     frame at all, because ptxas produced it during register allocation.
//
// `classifyFrame` separates the first two off the PTX text; `ptxasFrame`
// settles all three by asking ptxas, which already reports "bytes stack
// frame" and "bytes spill stores" as separate numbers on the same line. A
// nonzero frame with ZERO spill stores is never register pressure.
//
// One trap this encodes, paid for once: a PTX-level depot is not by itself a
// cost. `Prim.ropeF32` carries a 28-byte depot and 20 local ops, and ptxas
// promotes all of it — 0 bytes stack frame. Read `classifyFrame` as a lead,
// and `ptxasFrame` as the verdict.
//
// A probe that lies is worse than no probe, so the classifier has its own
// test with cases that make it fire and cases that make it stay quiet:
// test/xpu/KernelLoweringProbeTests.cpp.
//

#pragma once

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace cajeta::xpu::probe {

// --- compile + lower -------------------------------------------------------

// Compile `source` to a module for inspection. `fqClassName` names the class
// it declares, which fixes the file path the compiler expects; `tag` only
// keeps concurrent tests out of each other's temp directories.
inline CajetaModulePtr compileForInspection(Compiler& compiler,
                                            const std::string& source,
                                            const std::string& fqClassName
                                                = "test.M",
                                            const std::string& tag = "probe") {
    static std::mt19937_64 rng(std::random_device{}());
    const std::string salt = std::to_string(rng());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_" + tag + "_" + salt);
    std::filesystem::path rel;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= fqClassName.size(); ++i)
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    rel += ".cajeta";
    std::filesystem::create_directories((base / rel).parent_path());
    std::ofstream(base / rel) << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_" + tag + "_arch_" + salt);
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / rel).string(), base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

inline MethodPtr findMethod(const CajetaClassPtr& klass,
                            const std::string& name) {
    if (!klass) return nullptr;
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// What one kernel lowered to. `ok` false means it refused, with `why` set —
// a refusal is a result, not an error, since half these tests are about which
// kernels lower at all.
struct Lowered {
    bool ok = false;
    std::string why;
    std::string ir;     // device IR, before the backend's own passes
    std::string ptx;    // assembly, which is where a frame becomes visible
};

inline Lowered lowerForNvptx(const std::string& source,
                             const std::string& kernelName,
                             const std::string& fqClassName = "test.M",
                             const std::string& arch = "sm_89",
                             const std::string& tag = "probe") {
    Lowered out;
    Compiler compiler;
    auto module = compileForInspection(compiler, source, fqClassName, tag);
    auto klass = module->getStructures()[fqClassName];
    if (!klass) { out.why = fqClassName + " did not compile"; return out; }
    auto k = findMethod(klass, kernelName);
    if (!k) { out.why = "no kernel " + kernelName; return out; }
    auto tm = nvidia::createNvptxTargetMachine(arch);
    if (!tm) { out.why = "no " + arch + " target machine"; return out; }
    llvm::LLVMContext ctx;
    llvm::Module dev("probe_dev", ctx);
    nvidia::configureDeviceModule(dev, *tm);
    try {
        nvidia::lowerKernel(k, dev);
    } catch (const std::exception& e) { out.why = e.what(); return out; }
    { llvm::raw_string_ostream os(out.ir); dev.print(os, nullptr); }
    out.ptx = nvidia::emitPtx(dev, *tm);
    if (out.ptx.empty()) { out.why = "no ptx"; return out; }
    out.ok = true;
    return out;
}

inline std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1))
        ++n;
    return n;
}

// --- why does this kernel carry a frame? -----------------------------------

enum class FrameShape {
    None,                 // no depot in the PTX
    LegalizedConstruct,   // written through %SP, no ld.local: a lowering gap
    ScratchTile,          // real local traffic: the kernel declares an array
};

struct FrameReport {
    FrameShape shape = FrameShape::None;
    unsigned depotBytes = 0;
    std::size_t spRefs = 0;      // `[%SP` — generic stores into the frame
    std::size_t localOps = 0;    // st.local / ld.local
};

// Read the frame off the PTX. A LEAD, not a verdict — ptxas may promote the
// whole depot away (see the header comment). Use ptxasFrame to settle it.
inline FrameReport classifyFrame(const std::string& ptx) {
    FrameReport r;
    const std::string key = "__local_depot";
    auto at = ptx.find(key);
    if (at != std::string::npos) {
        auto open = ptx.find('[', at);
        auto close = ptx.find(']', open);
        if (open != std::string::npos && close != std::string::npos)
            r.depotBytes = (unsigned) std::strtoul(
                ptx.substr(open + 1, close - open - 1).c_str(), nullptr, 10);
    }
    r.spRefs = countOf(ptx, "[%SP");
    r.localOps = countOf(ptx, "st.local") + countOf(ptx, "ld.local");
    if (at == std::string::npos && r.spRefs == 0) r.shape = FrameShape::None;
    else if (r.localOps == 0 && r.spRefs > 0)
        r.shape = FrameShape::LegalizedConstruct;
    else r.shape = FrameShape::ScratchTile;
    return r;
}

// What ptxas itself reports, through the production parser rather than a
// second one. `ok` false means ptxas was unreachable or refused the PTX,
// which callers should SKIP on rather than fail — the toolchain is not
// always present, and `assembleCubin` also refuses below CUDA 12.1 on
// purpose.
//
// Note `spillStores`: the compiler ALREADY parses it
// (`NvptxBackend.cpp:329`) and then drops it — `NvptxRegistration.cpp:128`
// sets `manifest.spillBytes = st.stackBytes` and nothing keeps the rest. So
// the manifest, and therefore the spill gate, cannot tell a legalized
// construct from register pressure even though ptxas told it.
struct PtxasFrame {
    bool ok = false;
    unsigned stackFrame = 0;
    unsigned spillStores = 0;
    unsigned spillLoads = 0;
    unsigned registers = 0;
};

inline PtxasFrame ptxasFrame(const std::string& ptx,
                             const std::string& kernelName,
                             const std::string& arch = "sm_89") {
    PtxasFrame out;
    if (ptx.empty()) return out;
    std::string log;
    if (nvidia::assembleCubin(ptx, arch, &log).empty()) return out;
    for (const auto& st : nvidia::parsePtxasVerbose(log)) {
        if (st.name != kernelName) continue;
        out.stackFrame  = st.stackBytes;
        out.spillStores = st.spillStoreBytes;
        out.spillLoads  = st.spillLoadBytes;
        out.registers   = st.registers;
        out.ok = true;
        return out;
    }
    return out;
}

} // namespace cajeta::xpu::probe
