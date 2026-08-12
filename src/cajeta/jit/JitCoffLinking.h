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

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>

namespace cajeta {
namespace jit {

inline void applyCoffJitLink(llvm::orc::LLJITBuilder& builder) {
    if (!llvm::Triple(llvm::sys::getProcessTriple()).isOSBinFormatCOFF())
        return;
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
            layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
            layer->setAutoClaimResponsibilityForObjectSymbols(true);
            return layer;
        });
}

} // namespace jit
} // namespace cajeta
