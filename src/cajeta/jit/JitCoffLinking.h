#pragma once
// JITLink installation for COFF hosts: LLJIT's auto-config falls back to
// RuntimeDyld there, whose IMAGE_REL_AMD64_ADDR32NB handling aborts the whole
// process. Every LLJITBuilder in this process must be routed through here.

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
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

// Diagnostic plugin that forces every symbol live before pruning, defeating the
// layer's dead-stripping of anything the MaterializationResponsibility did not
// promise. Installed only under CAJETA_COFF_KEEPALIVE=1.
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

// True when the named environment variable is set to anything but "" or "0".
inline bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

// Drops the .pdata SEH unwind tables from a JIT'd COFF graph: their 32-bit RVAs
// cannot reach the host image base, and nothing registers them. Inbound
// KeepAlive edges go first — removeSection() frees Symbols prune() still reads.
inline llvm::Error dropSehFrames(llvm::jitlink::LinkGraph& g) {
    // .xdata is left to dead-stripping: .pdata is its only referrer.
    llvm::SmallPtrSet<llvm::jitlink::Section*, 4> doomed;
    for (auto& sec : g.sections())
        if (sec.getName().starts_with(".pdata"))
            doomed.insert(&sec);
    if (doomed.empty())
        return llvm::Error::success();

    for (auto* b : g.blocks()) {
        if (doomed.contains(&b->getSection()))
            continue;
        for (auto it = b->edges().begin(); it != b->edges().end();) {
            auto& target = it->getTarget();
            if (target.isDefined() &&
                doomed.contains(&target.getBlock().getSection()))
                it = b->removeEdge(it);
            else
                ++it;
        }
    }

    for (auto* sec : doomed)
        g.removeSection(*sec);
    return llvm::Error::success();
}

class DropSehFramesPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
    void modifyPassConfig(llvm::orc::MaterializationResponsibility&,
                          llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override {
        config.PrePrunePasses.push_back(dropSehFrames);
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

// Installs the JITLink object linking layer, the SEH-frame drop and the COFF
// responsibility overrides on the builder; a no-op off COFF. Must be called on
// every LLJITBuilder before create(): one un-audited site revives the abort.
inline void applyCoffJitLink(llvm::orc::LLJITBuilder& builder) {
    if (!llvm::Triple(llvm::sys::getProcessTriple()).isOSBinFormatCOFF())
        return;
    if (const char* mode = std::getenv("CAJETA_COFF_JIT");
        mode && std::string(mode) == "off") {
        fprintf(stderr, "cajeta.jit: COFF host — JITLink DISABLED "
                        "(CAJETA_COFF_JIT=off), using RuntimeDyld\n");
        return;
    }
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
            if (!envOn("CAJETA_COFF_NOCLAIM")) {
                layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
                layer->setAutoClaimResponsibilityForObjectSymbols(true);
            } else {
                fprintf(stderr, "cajeta.jit: COFF host — responsibility "
                                "overrides DISABLED (CAJETA_COFF_NOCLAIM)\n");
            }
            // On by default: without the drop, no COFF link succeeds at all.
            if (!envOn("CAJETA_COFF_KEEP_SEH"))
                layer->addPlugin(std::make_shared<DropSehFramesPlugin>());
            if (envOn("CAJETA_COFF_KEEPALIVE")) {
                fprintf(stderr, "cajeta.jit: COFF host — markAllSymbolsLive "
                                "plugin installed (CAJETA_COFF_KEEPALIVE)\n");
                layer->addPlugin(std::make_shared<KeepAllSymbolsLivePlugin>());
            }
            return layer;
        });
}

// Writes every object the JIT is about to link into $CAJETA_DUMP_OBJ (a
// directory, or "1" for the working directory); does nothing when unset. Call
// right after LLJITBuilder::create(), before anything is materialized.
inline void installObjectDump(llvm::orc::LLJIT& jit) {
    const char* dir = std::getenv("CAJETA_DUMP_OBJ");
    if (!dir || !*dir) return;
    std::string dumpDir = (std::string(dir) == "1") ? std::string() : dir;
    jit.getObjTransformLayer().setTransform(
        llvm::orc::DumpObjects(std::move(dumpDir)));
}

} // namespace jit
} // namespace cajeta
