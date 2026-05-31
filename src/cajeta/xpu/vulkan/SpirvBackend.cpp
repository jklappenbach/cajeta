//
// SPIR-V backend — see header.
//

#include "SpirvBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace cajeta {
namespace xpu {
namespace vulkan {

namespace {

void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
    });
}

// Run the SPIR-V codegen pipeline to a file type (Assembly = SPIR-V text,
// Object = SPIR-V binary) into an in-memory buffer. Unlike the AMDGPU backend
// there is no pre-emit mem2reg: the in-tree SPIR-V backend lowers Function-
// storage allocas itself, and PromotePass is unnecessary for the descriptor-
// set kernels we emit. Returns false (and logs) on failure.
bool emitToBuffer(llvm::Module& m, llvm::TargetMachine& tm,
                  llvm::CodeGenFileType type, llvm::SmallVectorImpl<char>& out) {
    llvm::raw_svector_ostream os(out);
    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr, type)) {
        llvm::errs() << "cajeta.xpu.vulkan: SPIR-V TargetMachine cannot emit "
                     << (type == llvm::CodeGenFileType::AssemblyFile
                             ? "assembly" : "object") << "\n";
        return false;
    }
    pm.run(m);
    return true;
}

// Rewrite every OpControlBarrier's memory-semantics operand to a Vulkan-valid
// value. LLVM 22's only barrier intrinsic
// (llvm.spv.group.memory.barrier.with.group.sync) lowers to OpControlBarrier
// with SequentiallyConsistent (0x10) semantics, which the Vulkan spec forbids
// (VUID-StandaloneSpirv-MemorySemantics-10866) — so the module fails strict
// spirv-val and may be rejected by drivers stricter than RADV. We add ONE new
// uint constant of value WorkgroupMemory|AcquireRelease (0x108) and repoint
// every barrier to it (rather than mutating the existing constant, which a user
// literal could share). Operates on the raw SPIR-V word stream; no-op on a
// non-little-endian / malformed module. Returns true if it changed anything.
//
// SPIR-V layout: header is 5 words (magic, version, generator, bound, schema);
// then a stream of instructions, each starting with (wordCount<<16 | opcode).
// OpConstant = 43 [type, result, literal]; OpControlBarrier = 224
// [execScope, memScope, semantics] — all <id> operands referencing constants.
bool fixupControlBarriers(std::vector<uint8_t>& bytes) {
    if (bytes.size() < 20 || (bytes.size() % 4) != 0) return false;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    if (w[0] != 0x07230203u) return false;  // SPIR-V magic (little-endian)

    constexpr uint32_t kOpConstant = 43, kOpControlBarrier = 224;
    constexpr uint32_t kVulkanSemantics = 0x108;  // WorkgroupMemory|AcquireRelease

    std::vector<size_t> barrierSemOperand;          // word idx of each semantics operand
    std::map<uint32_t, size_t> constInstStart;      // const result id -> its instr word idx
    std::map<uint32_t, uint32_t> constTypeId;       // const result id -> its type id

    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) return false;  // malformed — leave alone
        if (op == kOpConstant && wc >= 4) {
            constInstStart[w[i + 2]] = i;
            constTypeId[w[i + 2]] = w[i + 1];
        } else if (op == kOpControlBarrier && wc == 4) {
            barrierSemOperand.push_back(i + 3);
        }
        i += wc;
    }
    if (barrierSemOperand.empty()) return false;

    // Take the uint type + an in-section insertion point from the first
    // barrier's current semantics constant (guaranteed to be a module-level
    // OpConstant since a control barrier in a function references it).
    uint32_t curSemId = w[barrierSemOperand[0]];
    auto itType = constTypeId.find(curSemId);
    auto itPos = constInstStart.find(curSemId);
    if (itType == constTypeId.end() || itPos == constInstStart.end()) return false;

    uint32_t newId = w[3];   // current bound is the next free id
    w[3] = newId + 1;        // reserve it
    for (size_t idx : barrierSemOperand) w[idx] = newId;  // repoint (before insert)

    uint32_t newConst[4] = {(4u << 16) | kOpConstant, itType->second, newId,
                            kVulkanSemantics};
    w.insert(w.begin() + (itPos->second + 4), newConst, newConst + 4);

    bytes.resize(w.size() * 4);
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return true;
}

} // namespace

std::unique_ptr<llvm::TargetMachine>
createSpirvTargetMachine(const std::string& /*arch*/) {
    ensureTargetsInitialized();

    llvm::Triple triple(kSpirvTriple);
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.vulkan: spirv target not available: "
                     << error << "\n";
        return nullptr;
    }

    llvm::TargetOptions opt;
    // SPIR-V is not position-independent like AMDGPU; the default reloc model
    // is correct.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/"", /*Features=*/"", opt, /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kSpirvTriple));
    m.setDataLayout(tm.createDataLayout());
}

std::string emitSpirvText(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    llvm::SmallString<0> buf;
    if (!emitToBuffer(deviceModule, tm, llvm::CodeGenFileType::AssemblyFile,
                      buf)) {
        return {};
    }
    return std::string(buf.begin(), buf.end());
}

std::vector<uint8_t> emitSpirv(llvm::Module& deviceModule,
                               llvm::TargetMachine& tm) {
    llvm::SmallString<0> buf;
    if (!emitToBuffer(deviceModule, tm, llvm::CodeGenFileType::ObjectFile,
                      buf)) {
        return {};
    }
    std::vector<uint8_t> spirv(buf.begin(), buf.end());
    // Make any workgroup barrier Vulkan-spec-valid (LLVM 22 emits forbidden
    // SequentiallyConsistent semantics). No-op for barrier-free kernels.
    fixupControlBarriers(spirv);
    return spirv;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
