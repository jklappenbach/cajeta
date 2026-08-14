#pragma once
//
// COFF hosts need JITLink installed explicitly on EVERY LLJIT this process
// builds. LLJIT's auto-config deliberately avoids JITLink on COFF (LLJIT.cpp
// `UseJITLink = !TT.isOSBinFormatCOFF()`) and falls back to RuntimeDyld —
// whose COFF x86-64 handler cannot relocate IMAGE_REL_AMD64_ADDR32NB unless
// sections happen to land in image order, and report_fatal_error()s the WHOLE
// PROCESS when they don't (specs/windows-jit-coff-reloc-spec.md; the abort
// point wanders with the host module's section layout, so any Windows JIT
// workload can hit it). JITLink's COFF backend computes image-relative fixups
// correctly.
//
// This helper exists because the first fix covered only CajetaJitHost's
// builder, and the abort promptly resurfaced through KernelSession's bare
// `LLJITBuilder().create()` 48 tests later — one un-audited builder site is
// enough to keep the defect alive. Route every LLJITBuilder through here.
//
// Mirrors what LLJIT's own JITLink path sets up: Small code model + PIC
// relocation on the target machine, and the two responsibility-flag overrides
// its RTDyld-COFF path applies for comdat/weak symbols. ELF/MachO are
// untouched (the function is a no-op off COFF).
//

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/DebugUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace cajeta {
namespace jit {

// Diagnostic plugin: force every symbol live before pruning. The layer's own
// mark-live pass (LinkGraphLinkingLayer's markResponsibilitySymbolsLive) marks
// only the symbols the MaterializationResponsibility promised, and everything
// else is dead-stripped. Enabled by CAJETA_COFF_KEEPALIVE=1 to test whether
// dead-stripping is what leaves promised symbols unmaterialized on COFF.
class KeepAllSymbolsLivePlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
    void modifyPassConfig(llvm::orc::MaterializationResponsibility&,
                          llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override {
        config.PrePrunePasses.push_back(llvm::jitlink::markAllSymbolsLive);
    }
    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override {
        return llvm::Error::success();
    }
    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&,
                                        llvm::orc::ResourceKey) override {
        return llvm::Error::success();
    }
    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey,
                                     llvm::orc::ResourceKey) override {}
};

inline bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

inline void applyCoffJitLink(llvm::orc::LLJITBuilder& builder) {
    if (!llvm::Triple(llvm::sys::getProcessTriple()).isOSBinFormatCOFF())
        return;
    // Escape hatch for bisecting the COFF JIT path: CAJETA_COFF_JIT=off falls
    // back to LLVM's default (RuntimeDyld), which is what shipped before the
    // JITLink switch. Diagnostic only — RuntimeDyld aborts the process on
    // IMAGE_REL_AMD64_ADDR32NB, which is why this helper exists at all.
    if (const char* mode = std::getenv("CAJETA_COFF_JIT");
        mode && std::string(mode) == "off") {
        fprintf(stderr, "cajeta.jit: COFF host — JITLink DISABLED "
                        "(CAJETA_COFF_JIT=off), using RuntimeDyld\n");
        return;
    }
    // Once-per-process breadcrumb: the ADDR32NB abort resurfaced on a binary
    // where both builder sites carry this helper, so either a RuntimeDyld
    // user exists outside them or the runner built stale objects. This line
    // in a run log proves the fixed code executed; its absence proves the
    // build. Remove when windows-jit-coff-reloc closes for good.
    static bool noted = false;
    if (!noted) {
        noted = true;
        fprintf(stderr,
                "cajeta.jit: COFF host — JITLink object layer installed\n");
    }
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (jtmb) {
        if (!jtmb->getCodeModel())
            jtmb->setCodeModel(llvm::CodeModel::Small);
        jtmb->setRelocationModel(llvm::Reloc::PIC_);
        builder.setJITTargetMachineBuilder(std::move(*jtmb));
    }
    builder.setObjectLinkingLayerCreator(
        [](llvm::orc::ExecutionSession& es,
           llvm::jitlink::JITLinkMemoryManager& memMgr)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
            auto layer =
                std::make_unique<llvm::orc::ObjectLinkingLayer>(es, memMgr);
            // CAJETA_COFF_CLAIM=off drops the two responsibility overrides.
            // They mirror what LLJIT's RTDyld-COFF path applies for
            // comdat/weak symbols, but they are also the only COFF-specific
            // config we set — so they are the first suspect when promised
            // symbols come back unmaterialized.
            if (!envOn("CAJETA_COFF_NOCLAIM")) {
                layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
                layer->setAutoClaimResponsibilityForObjectSymbols(true);
            } else {
                fprintf(stderr, "cajeta.jit: COFF host — responsibility "
                                "overrides DISABLED (CAJETA_COFF_NOCLAIM)\n");
            }
            if (envOn("CAJETA_COFF_KEEPALIVE")) {
                fprintf(stderr, "cajeta.jit: COFF host — markAllSymbolsLive "
                                "plugin installed (CAJETA_COFF_KEEPALIVE)\n");
                layer->addPlugin(std::make_shared<KeepAllSymbolsLivePlugin>());
            }
            return layer;
        });
}

// Write every object the JIT is about to link to disk, when CAJETA_DUMP_OBJ is
// set (to a directory, or to "1" for the working directory). Call right after
// LLJITBuilder::create() on any JIT whose input you need to inspect.
//
// Why this exists: JITLink's COFF reader rejects an object we emit with
// "Could not find symbol at given index, did you add it to JITSymbolTable?"
// (COFF_x86_64.cpp addSingleRelocation — a relocation naming a symbol the
// LinkGraph builder never created). Deciding whether that is an LLVM gap or
// a malformed object of ours needs the object itself: dump it, read it with
// llvm-readobj, and hand the same bytes to a real linker. Every platform, not
// just COFF — an ELF dump of the same module is the control.
inline void installObjectDump(llvm::orc::LLJIT& jit) {
    const char* dir = std::getenv("CAJETA_DUMP_OBJ");
    if (!dir || !*dir) return;
    std::string dumpDir = (std::string(dir) == "1") ? std::string() : dir;
    jit.getObjTransformLayer().setTransform(
        llvm::orc::DumpObjects(std::move(dumpDir)));
}

} // namespace jit
} // namespace cajeta
