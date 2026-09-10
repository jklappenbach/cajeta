// SPIR-V backend — see header.

#include "SpirvBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/StructurizeCFG.h"
#include "llvm/Transforms/Utils/FixIrreducible.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
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

// Enables the SPIR-V extensions cajeta emits. The in-tree backend gates extension
// opcodes behind the `spirv-ext` cl::opt, and in-process emission has no command line,
// so the registered option is driven directly.
void enableSpirvExtensions() {
    // An explicit allowlist, not "khr" or "all", so only tested extensions are enabled;
    // vulkan_memory_model rides with cooperative_matrix because spirv-val demands it.
    static const char* kExtensions =
        "+SPV_KHR_ray_query,+SPV_KHR_cooperative_matrix,"
        "+SPV_KHR_vulkan_memory_model,+SPV_KHR_bfloat16,"
        "+SPV_KHR_integer_dot_product,+SPV_EXT_shader_atomic_float_add,"
        "+SPV_EXT_shader_atomic_float_min_max,+SPV_KHR_shader_clock,"
        "+SPV_KHR_maximal_reconvergence,+SPV_KHR_subgroup_rotate,"
        "+SPV_KHR_quad_control";
    auto& opts = llvm::cl::getRegisteredOptions();
    auto it = opts.find("spirv-ext");
    if (it != opts.end())
        it->second->addOccurrence(0, "spirv-ext", kExtensions);
}

void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
        enableSpirvExtensions();
    });
}

// Runs the codegen pipeline into an in-memory buffer, as SPIR-V text for Assembly or
// binary for Object. False, with a log line, on failure.
bool emitToBuffer(llvm::Module& m, llvm::TargetMachine& tm,
                  llvm::CodeGenFileType type, llvm::SmallVectorImpl<char>& out) {
    // A SPIR-V Buffer<T> is a descriptor HANDLE, and the instruction selector traces a
    // load's handle back to its handlefrombinding WITHIN one function: a handle crossing
    // a call boundary crashes selectStore, so every @Device helper is inlined first.
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::AlwaysInlinerPass());
    // The backend's structurizer rejects the CFG that inlining the software ray-query
    // walk produces, so the passes below hand it single-exit structured regions instead.
    {
        llvm::FunctionPassManager fpm;
        // Mem2reg must run BEFORE codegen: a wide vector local left in memory reaches
        // the backend as spv_load/spv_store, which bypass the legalizer's wide-vector
        // splitting entirely, and on a shader target that is a hard failure.
        fpm.addPass(llvm::PromotePass());
        // Without this the builder's literal instruction stream reaches the backend,
        // where every LDS index add is a fresh i64 chain.
        fpm.addPass(llvm::EarlyCSEPass());
        // NOT InstCombine: its FP narrowing and GEP reshaping crash
        // SPIRVLegalizePointerCast and trip the bfloat16-arithmetic guard.
        fpm.addPass(llvm::FixIrreduciblePass());
        fpm.addPass(llvm::UnifyFunctionExitNodesPass());
        fpm.addPass(llvm::StructurizeCFGPass());
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    }
    mpm.run(m, mam);

    // FP narrowing canonicalizes fptrunc(op(fpext a, fpext b)) into direct bfloat
    // arithmetic, which needs SPV_INTEL_bfloat16_arithmetic, not a Vulkan extension.
    // Re-expand it: a bf16 op is DEFINED here as a round-trip through f32.
    {
        llvm::SmallVector<llvm::Instruction*, 8> bf16Ops;
        for (auto& f : m)
            for (auto& bb : f)
                for (auto& inst : bb) {
                    if (!inst.getType()->isBFloatTy()
                        && !(inst.getType()->isVectorTy()
                             && inst.getType()->getScalarType()->isBFloatTy()))
                        continue;
                    if (inst.isBinaryOp() || inst.getOpcode() == llvm::Instruction::FNeg)
                        bf16Ops.push_back(&inst);
                }
        for (llvm::Instruction* inst : bf16Ops) {
            llvm::IRBuilder<> b(inst);
            llvm::Type* fTy = inst->getType()->isVectorTy()
                ? (llvm::Type*) llvm::FixedVectorType::get(
                      b.getFloatTy(),
                      llvm::cast<llvm::FixedVectorType>(inst->getType())
                          ->getNumElements())
                : (llvm::Type*) b.getFloatTy();
            llvm::Value* wide;
            if (inst->getOpcode() == llvm::Instruction::FNeg) {
                wide = b.CreateFNeg(
                    b.CreateFPExt(inst->getOperand(0), fTy, "bf16.x"));
            } else {
                wide = b.CreateBinOp(
                    (llvm::Instruction::BinaryOps) inst->getOpcode(),
                    b.CreateFPExt(inst->getOperand(0), fTy, "bf16.x"),
                    b.CreateFPExt(inst->getOperand(1), fTy, "bf16.y"),
                    "bf16.w");
            }
            llvm::Value* back = b.CreateFPTrunc(wide, inst->getType(),
                                                "bf16.t");
            inst->replaceAllUsesWith(back);
            inst->eraseFromParent();
        }
    }

    // CAJETA_XPU_DUMP_BC=<dir>: the post-structurize, pre-codegen bitcode, so a legalizer
    // failure can be reproduced under a standalone `llc -global-isel`.
    if (const char* dumpDir = std::getenv("CAJETA_XPU_DUMP_BC")) {
        std::string name = "module";
        for (auto& f : m) {
            if (!f.isDeclaration()) { name = f.getName().str(); break; }
        }
        std::error_code ec;
        llvm::raw_fd_ostream bcOut(
            std::string(dumpDir) + "/" + name + ".vulkan.bc", ec);
        if (!ec) llvm::WriteBitcodeToFile(m, bcOut);
    }

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

// Repoints every OpControlBarrier at one new WorkgroupMemory|AcquireRelease (0x108) uint
// constant: LLVM 23 emits the SequentiallyConsistent semantics Vulkan forbids. A new
// constant, since a user literal could share the existing one. True if anything changed.
// Word stream: a 5-word header, then instructions headed by (wordCount<<16 | opcode);
// OpConstant = 43 [type, result, literal], OpControlBarrier = 224 [exec, mem, semantics].
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

    // The uint type and an insertion point both come from the first barrier's current
    // semantics constant, which is module-level because a function references it.
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

// Makes the workgroup size a spec constant the launch's `block` dims set at pipeline
// creation, LLVM 23 having no IR path to a spec-constant LocalSizeId: three
// OpSpecConstant uint plus a composite decorated BuiltIn WorkgroupSize override it.
bool injectWorkgroupSizeSpecConstant(std::vector<uint8_t>& bytes) {
    if (bytes.size() < 20 || (bytes.size() % 4) != 0) return false;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    if (w[0] != 0x07230203u) return false;

    constexpr uint32_t kOpExecMode = 16, kOpTypeInt = 21, kOpTypeVector = 23,
                       kOpFunction = 54, kOpSpecConstant = 50,
                       kOpSpecConstComposite = 51, kOpDecorate = 71;
    constexpr uint32_t kLocalSize = 17, kSpecId = 1, kBuiltIn = 11,
                       kWorkgroupSize = 25;

    uint32_t uintTy = 0, v3uintTy = 0;
    uint32_t defX = 64, defY = 1, defZ = 1;
    size_t decoEnd = 0;      // word idx just past the last OpDecorate/OpMemberDecorate
    size_t firstFn = 0;      // word idx of the first OpFunction
    bool sawLocalSize = false;

    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) return false;
        if (op == kOpExecMode && wc >= 6 && w[i + 2] == kLocalSize) {
            defX = w[i + 3]; defY = w[i + 4]; defZ = w[i + 5];
            sawLocalSize = true;
        } else if (op == kOpTypeInt && wc >= 4 && w[i + 2] == 32 && w[i + 3] == 0) {
            if (!uintTy) uintTy = w[i + 1];
        } else if (op == kOpTypeVector && wc == 4 && w[i + 3] == 3 &&
                   w[i + 2] == uintTy) {
            if (!v3uintTy) v3uintTy = w[i + 1];
        } else if (op >= 71 && op <= 76) {        // any OpDecorate* / OpMemberDecorate
            decoEnd = i + wc;
        } else if (op == kOpFunction) {
            firstFn = i;
            break;                                 // types/constants/decos all precede
        }
        i += wc;
    }
    if (!sawLocalSize || !uintTy || decoEnd == 0 || firstFn == 0) return false;

    uint32_t idX = w[3], idY = idX + 1, idZ = idX + 2, idWg = idX + 3;
    uint32_t nextId = idX + 4;
    std::vector<uint32_t> mkType;     // an OpTypeVector to insert if v3uint absent
    if (!v3uintTy) {
        v3uintTy = nextId++;
        mkType = {(4u << 16) | kOpTypeVector, v3uintTy, uintTy, 3u};
    }
    w[3] = nextId;

    std::vector<uint32_t> consts;
    auto specConst = [&](uint32_t id, uint32_t def) {
        consts.insert(consts.end(),
                      {(4u << 16) | kOpSpecConstant, uintTy, id, def});
    };
    consts.insert(consts.end(), mkType.begin(), mkType.end());
    specConst(idX, defX); specConst(idY, defY); specConst(idZ, defZ);
    consts.insert(consts.end(),
                  {(6u << 16) | kOpSpecConstComposite, v3uintTy, idWg,
                   idX, idY, idZ});

    std::vector<uint32_t> decos = {
        (4u << 16) | kOpDecorate, idX, kSpecId, 0u,
        (4u << 16) | kOpDecorate, idY, kSpecId, 1u,
        (4u << 16) | kOpDecorate, idZ, kSpecId, 2u,
        (4u << 16) | kOpDecorate, idWg, kBuiltIn, kWorkgroupSize};

    // The later block (constants, at firstFn) goes in first so decoEnd stays valid.
    w.insert(w.begin() + firstFn, consts.begin(), consts.end());
    w.insert(w.begin() + decoEnd, decos.begin(), decos.end());

    bytes.resize(w.size() * 4);
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return true;
}

// Makes a dynamic shared array's LENGTH a spec constant (SpecId 3) set from the launch's
// sharedBytes: the array is found by its `cajeta_dynsh_…` OpName and its OpTypeArray
// length operand repointed, so it is sized at pipeline creation.
constexpr uint32_t kSpecIdDynShared = 3;   // 0/1/2 are the workgroup-size dims
bool injectDynamicSharedSpecConstant(std::vector<uint8_t>& bytes) {
    if (bytes.size() < 20 || (bytes.size() % 4) != 0) return false;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    if (w[0] != 0x07230203u) return false;

    constexpr uint32_t kOpName = 5, kOpDecorate = 71, kOpTypeArray = 28,
                       kOpTypePointer = 32, kOpConstant = 43, kOpVariable = 59,
                       kOpSpecConstant = 50, kSpecId = 1;

    uint32_t dynVar = 0;
    std::map<uint32_t, uint32_t> varType;       // OpVariable result -> result type
    std::map<uint32_t, uint32_t> ptrPointee;    // OpTypePointer result -> pointee
    std::map<uint32_t, size_t> arrLenWord;      // OpTypeArray result -> length-operand word idx
    std::map<uint32_t, uint32_t> constType;     // OpConstant result -> type
    std::map<uint32_t, uint32_t> constVal;      // OpConstant result -> literal
    size_t firstFn = 0, decoEnd = 0;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) return false;
        if (op == kOpName && wc >= 3) {
            const char* s = reinterpret_cast<const char*>(&w[i + 2]);
            size_t maxLen = (wc - 2) * 4;
            if (strnlen(s, maxLen) < maxLen &&
                std::string(s).find("cajeta_dynsh_") != std::string::npos)
                dynVar = w[i + 1];
        } else if (op == kOpVariable && wc >= 3) {
            varType[w[i + 2]] = w[i + 1];
        } else if (op == kOpTypePointer && wc == 4) {
            ptrPointee[w[i + 1]] = w[i + 3];
        } else if (op == kOpTypeArray && wc == 4) {
            arrLenWord[w[i + 1]] = i + 3;
        } else if (op == kOpConstant && wc >= 4) {
            constType[w[i + 2]] = w[i + 1];
            constVal[w[i + 2]] = w[i + 3];
        } else if (op >= 71 && op <= 76) {
            decoEnd = i + wc;
        } else if ((w[i] & 0xFFFFu) == 54 /*OpFunction*/) {   // section 9 ends
            firstFn = i; break;
        }
        i += wc;
    }
    if (!dynVar || decoEnd == 0 || firstFn == 0) return false;
    auto vt = varType.find(dynVar);
    if (vt == varType.end()) return false;
    auto pp = ptrPointee.find(vt->second);
    if (pp == ptrPointee.end()) return false;
    auto al = arrLenWord.find(pp->second);
    if (al == arrLenWord.end()) return false;
    uint32_t lenConst = w[al->second];
    auto ct = constType.find(lenConst);
    auto cv = constVal.find(lenConst);
    if (ct == constType.end() || cv == constVal.end()) return false;

    uint32_t newId = w[3];
    w[3] = newId + 1;
    w[al->second] = newId;                          // repoint the array length

    uint32_t spec[4] = {(4u << 16) | kOpSpecConstant, ct->second, newId,
                        cv->second};                // default = the baked length
    uint32_t deco[4] = {(4u << 16) | kOpDecorate, newId, kSpecId,
                        kSpecIdDynShared};
    // Types and constants cannot forward-reference, so the spec constant goes just before
    // the array; a decoration may, so it goes at decoEnd.
    size_t arrStart = al->second - 3;               // OpTypeArray instruction start
    w.insert(w.begin() + arrStart, spec, spec + 4); // before the array (later pos)
    w.insert(w.begin() + decoEnd, deco, deco + 4);  // decoEnd < arrStart, unshifted

    bytes.resize(w.size() * 4);
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return true;
}

// Turns ONE user spec-constant witness — a Private OpVariable named `cajeta_spec_<id>`
// seeded with the compile-time default — into a real OpSpecConstant, repointing the
// variable's initializer at it. Patches the FIRST unpatched witness.
bool injectOneUserSpecConstant(std::vector<uint8_t>& bytes) {
    if (bytes.size() < 20 || (bytes.size() % 4) != 0) return false;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    if (w[0] != 0x07230203u) return false;

    constexpr uint32_t kOpName = 5, kOpDecorate = 71, kOpConstant = 43,
                       kOpVariable = 59, kOpSpecConstant = 50, kOpFunction = 54,
                       kSpecId = 1;

    std::map<uint32_t, uint32_t> nameSpecId;   // OpVariable result -> specId (from name)
    std::map<uint32_t, size_t> varInitWord;    // OpVariable result -> initializer-operand word
    std::map<uint32_t, uint32_t> constType;    // OpConstant result -> type id
    std::map<uint32_t, uint32_t> constVal;     // OpConstant result -> literal
    size_t firstFn = 0, decoEnd = 0;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) return false;
        if (op == kOpName && wc >= 3) {
            const char* s = reinterpret_cast<const char*>(&w[i + 2]);
            size_t maxLen = (wc - 2) * 4;
            if (strnlen(s, maxLen) < maxLen) {
                std::string nm(s);
                const std::string pfx = "cajeta_spec_";
                if (nm.size() > pfx.size() &&
                    nm.compare(0, pfx.size(), pfx) == 0) {
                    uint32_t sid = 0;
                    bool ok = true;
                    for (size_t k = pfx.size(); k < nm.size(); ++k) {
                        if (nm[k] < '0' || nm[k] > '9') { ok = false; break; }
                        sid = sid * 10 + (uint32_t) (nm[k] - '0');
                    }
                    if (ok) nameSpecId[w[i + 1]] = sid;
                }
            }
        } else if (op == kOpVariable && wc >= 5) {   // OpVariable WITH initializer
            // OpVariable <result-type> <result> <storage-class> <initializer>
            varInitWord[w[i + 2]] = i + 4;
        } else if (op == kOpConstant && wc >= 4) {
            constType[w[i + 2]] = w[i + 1];
            constVal[w[i + 2]] = w[i + 3];
        } else if (op >= 71 && op <= 76) {           // any OpDecorate* / member
            decoEnd = i + wc;
        } else if (op == kOpFunction) {
            firstFn = i; break;
        }
        i += wc;
    }
    if (decoEnd == 0 || firstFn == 0) return false;

    // A patched variable's initializer is an OpSpecConstant, absent from constVal, so
    // this lookup skips it and finds the first unpatched witness.
    for (const auto& kv : nameSpecId) {
        auto iw = varInitWord.find(kv.first);
        if (iw == varInitWord.end()) continue;
        uint32_t initId = w[iw->second];
        auto ct = constType.find(initId);
        auto cv = constVal.find(initId);
        if (ct == constType.end() || cv == constVal.end()) continue;

        uint32_t newId = w[3];
        w[3] = newId + 1;
        w[iw->second] = newId;                       // repoint the initializer

        uint32_t spec[4] = {(4u << 16) | kOpSpecConstant, ct->second, newId,
                            cv->second};             // default = the witness seed
        uint32_t deco[4] = {(4u << 16) | kOpDecorate, newId, kSpecId, kv.second};
        // The spec constant goes just before the OpVariable that references it; the
        // decoration goes at decoEnd, which is earlier and so unshifted by that insert.
        size_t varStart = iw->second - 4;            // OpVariable instruction start
        w.insert(w.begin() + varStart, spec, spec + 4);
        w.insert(w.begin() + decoEnd, deco, deco + 4);

        bytes.resize(w.size() * 4);
        std::memcpy(bytes.data(), w.data(), bytes.size());
        return true;
    }
    return false;
}

// Patch every user spec-constant witness (Spec.geti). No-op without any.
bool injectUserSpecConstants(std::vector<uint8_t>& bytes) {
    bool any = false;
    while (injectOneUserSpecConstant(bytes)) any = true;
    return any;
}

// A SPIR-V TargetMachine for an explicit triple, initializing the target registry: the
// per-stage and compute entry points both funnel through here.
std::unique_ptr<llvm::TargetMachine>
createTargetMachineForTriple(const std::string& tripleStr) {
    ensureTargetsInitialized();

    llvm::Triple triple(tripleStr);
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.vulkan: spirv target not available for "
                     << tripleStr << ": " << error << "\n";
        return nullptr;
    }

    llvm::TargetOptions opt;
    // The default reloc model is right: SPIR-V is not position-independent.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/"", /*Features=*/"", opt, /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

} // namespace

std::unique_ptr<llvm::TargetMachine>
createSpirvTargetMachine(const std::string& /*arch*/) {
    return createTargetMachineForTriple(kSpirvTriple);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kSpirvTriple));
    m.setDataLayout(tm.createDataLayout());
}

const char* spirvStageEnv(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Compute:     return "compute";
        case ShaderStage::Vertex:      return "vertex";
        case ShaderStage::Fragment:    return "pixel";
        case ShaderStage::Geometry:    return "geometry";
        case ShaderStage::TessControl: return "hull";
        case ShaderStage::TessEval:    return "domain";
        case ShaderStage::Mesh:        return "mesh";
        case ShaderStage::Task:        return "amplification";
    }
    return "compute";
}

std::string spirvStageTriple(ShaderStage stage, const std::string& arch) {
    return std::string("spirv-unknown-") + arch + "-" + spirvStageEnv(stage);
}

const char* hlslShaderAttr(ShaderStage stage) {
    // The attribute spelling coincides with the triple env token: both are stage names.
    return spirvStageEnv(stage);
}

std::unique_ptr<llvm::TargetMachine>
createSpirvTargetMachineForStage(ShaderStage stage, const std::string& arch) {
    return createTargetMachineForTriple(spirvStageTriple(stage, arch));
}

void configureDeviceModuleForStage(llvm::Module& m, llvm::TargetMachine& tm,
                                   ShaderStage stage, const std::string& arch) {
    m.setTargetTriple(llvm::Triple(spirvStageTriple(stage, arch)));
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
    // Each of these word-stream fixups is a no-op on a kernel that lacks its feature.
    fixupControlBarriers(spirv);
    injectWorkgroupSizeSpecConstant(spirv);
    injectDynamicSharedSpecConstant(spirv);
    injectUserSpecConstants(spirv);
    // CAJETA_XPU_DUMP_SPV=<dir>: the FINAL binary, exactly what vkCreateShaderModule
    // receives, so spirv-val triage sees the driver's input and not an approximation.
    if (const char* dumpDir = std::getenv("CAJETA_XPU_DUMP_SPV")) {
        std::string name = "module";
        for (auto& f : deviceModule) {
            if (!f.isDeclaration()) { name = f.getName().str(); break; }
        }
        std::error_code ec;
        llvm::raw_fd_ostream spvOut(
            std::string(dumpDir) + "/" + name + ".spv", ec);
        if (!ec) spvOut.write(reinterpret_cast<const char*>(spirv.data()),
                              spirv.size());
    }
    return spirv;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
