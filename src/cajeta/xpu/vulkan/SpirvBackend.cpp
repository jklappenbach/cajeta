//
// SPIR-V backend — see header.
//

#include "SpirvBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
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

// Enable the SPIR-V extensions cajeta emits. The in-tree SPIR-V backend gates
// extension opcodes/capabilities behind the `spirv-ext` cl::opt (the same flag
// llc takes as `--spirv-ext=...`); for in-process emission there is no command
// line, so we set it programmatically. The SPIRV target's own
// SPIRVSubtarget::addExtensionsToClOpt would be cleaner, but its header is not
// shipped in the LLVM artifact, so we drive the registered option directly.
//
// `SPV_KHR_ray_query` (cajeta-gpu Part C) is the first; "all of the cutting-edge
// GPU calls" (cooperative matrix, etc.) extend this list as they land. The
// parser also accepts "khr" (all KHR) / "all" — kept to an explicit allowlist so
// emission stays deterministic and only the extensions we test are enabled.
void enableSpirvExtensions() {
    // Cooperative matrix (CM2) adds +SPV_KHR_cooperative_matrix and
    // +SPV_KHR_vulkan_memory_model — the latter because a Shader module that
    // declares CooperativeMatrixKHR must also declare VulkanMemoryModel
    // (spirv-val), which the backend emits as OpMemoryModel Logical VulkanKHR.
    // SPV_KHR_bfloat16 gives the bfloat *type* + conversions (portable KHR); we
    // never emit native bfloat *arithmetic* (that is Intel-only) — the device
    // lowerer computes bfloat in f32 (widen / op / narrow), the standard GPU
    // "bf16 is a storage format" model.
    // SPV_KHR_integer_dot_product gives the DP4a op (OpSDot/OpUDot
    // PackedVectorFormat4x8Bit) for Vector<int8,4>.dot — without it the backend
    // falls back to the portable bit-field expansion (correct, but not the
    // hardware dot-product unit).
    // SPV_EXT_shader_atomic_float_add / _min_max give the float atomic-RMW ops
    // (OpAtomicFAddEXT/FMinEXT/FMaxEXT) for Buffer<float32>.atomic{Add,Min,Max}.
    static const char* kExtensions =
        "+SPV_KHR_ray_query,+SPV_KHR_cooperative_matrix,"
        "+SPV_KHR_vulkan_memory_model,+SPV_KHR_bfloat16,"
        "+SPV_KHR_integer_dot_product,+SPV_EXT_shader_atomic_float_add,"
        "+SPV_EXT_shader_atomic_float_min_max";
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

// Run the SPIR-V codegen pipeline to a file type (Assembly = SPIR-V text,
// Object = SPIR-V binary) into an in-memory buffer. Unlike the AMDGPU backend
// there is no pre-emit mem2reg: the in-tree SPIR-V backend lowers Function-
// storage allocas itself, and PromotePass is unnecessary for the descriptor-
// set kernels we emit. Returns false (and logs) on failure.
bool emitToBuffer(llvm::Module& m, llvm::TargetMachine& tm,
                  llvm::CodeGenFileType type, llvm::SmallVectorImpl<char>& out) {
    // Inline every @Device helper (alwaysinline, internal) into its kernel
    // before codegen. On NVPTX/AMDGPU a buffer arg is an addrspace(1) pointer
    // that survives a call, but on SPIR-V a Buffer<T> is a descriptor HANDLE
    // (spirv.VulkanBuffer) — and the in-tree SPIR-V instruction selector traces
    // a load/store's handle back to its handlefrombinding WITHIN one function;
    // a handle passed across a call boundary crashes selectStore. Folding the
    // helper in (its whole reason for being alwaysinline) keeps every buffer
    // access in the kernel where the handle is bound. Idempotent + no-op for
    // helper-free kernels.
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
    mpm.run(m, mam);

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
// value. LLVM 23's only barrier intrinsic
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

// Make the compute workgroup size a SPECIALIZATION CONSTANT so the launch's
// `block` dims set it at pipeline creation, instead of the fixed `OpExecutionMode
// LocalSize 64,1,1` baked by hlsl.numthreads (LLVM 23 has no IR path to a
// spec-constant LocalSizeId). We add the classic `WorkgroupSize` builtin pattern:
// three `OpSpecConstant uint` (SpecId 0/1/2, defaulting to the baked dims) + an
// `OpSpecConstantComposite v3uint` decorated `BuiltIn WorkgroupSize` — which
// overrides the (retained) LocalSize default. The runtime supplies the three
// spec constants via VkSpecializationInfo per (bx,by,bz). Raw word-stream surgery
// like fixupControlBarriers; no-op on a malformed/odd module. SPIR-V op/enum
// numbers: OpExecutionMode=16, OpTypeInt=21, OpTypeVector=23, OpFunction=54,
// OpSpecConstant=50, OpSpecConstantComposite=51, OpDecorate=71; LocalSize mode=17,
// SpecId deco=1, BuiltIn deco=11, WorkgroupSize builtin=25.
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

    // Reserve ids: specX/Y/Z (+ the composite), and a v3uint type if absent.
    uint32_t idX = w[3], idY = idX + 1, idZ = idX + 2, idWg = idX + 3;
    uint32_t nextId = idX + 4;
    std::vector<uint32_t> mkType;     // an OpTypeVector to insert if v3uint absent
    if (!v3uintTy) {
        v3uintTy = nextId++;
        mkType = {(4u << 16) | kOpTypeVector, v3uintTy, uintTy, 3u};
    }
    w[3] = nextId;

    // Constants (inserted before the first function): spec constants + composite.
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

    // Decorations: SpecId 0/1/2 on the scalars + BuiltIn WorkgroupSize on composite.
    std::vector<uint32_t> decos = {
        (4u << 16) | kOpDecorate, idX, kSpecId, 0u,
        (4u << 16) | kOpDecorate, idY, kSpecId, 1u,
        (4u << 16) | kOpDecorate, idZ, kSpecId, 2u,
        (4u << 16) | kOpDecorate, idWg, kBuiltIn, kWorkgroupSize};

    // Insert the LATER block first (constants, at firstFn) so decoEnd stays valid.
    w.insert(w.begin() + firstFn, consts.begin(), consts.end());
    w.insert(w.begin() + decoEnd, decos.begin(), decos.end());

    bytes.resize(w.size() * 4);
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return true;
}

// Make a dynamic shared array's LENGTH a specialization constant set from the
// launch's sharedBytes. The shared lowerer emits a concrete `[256 x T]` internal
// Workgroup array named `cajeta_dynsh_…` (a 1-elem array would decay to a scalar,
// and an external/unsized one needs the forbidden Linkage capability). This finds
// that array via its OpName, adds an OpSpecConstant (SpecId 3, default = the baked
// length) and repoints the OpTypeArray's length operand to it — the array is then
// sized at pipeline creation. Op/enum: OpName=5, OpDecorate=71, OpTypeArray=28,
// OpTypePointer=32, OpConstant=43, OpVariable=59, OpSpecConstant=50; SpecId deco=1.
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
    // The spec constant must precede the OpTypeArray that uses it (types/constants
    // can't forward-reference). Insert it just before the array; decorations
    // (earlier in the module) may forward-reference, so insert that at decoEnd.
    size_t arrStart = al->second - 3;               // OpTypeArray instruction start
    w.insert(w.begin() + arrStart, spec, spec + 4); // before the array (later pos)
    w.insert(w.begin() + decoEnd, deco, deco + 4);  // decoEnd < arrStart, unshifted

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
    // Make any workgroup barrier Vulkan-spec-valid (LLVM 23 emits forbidden
    // SequentiallyConsistent semantics). No-op for barrier-free kernels.
    fixupControlBarriers(spirv);
    // Make the workgroup size a spec constant so the launch's block dims set it
    // at pipeline creation (default = the baked LocalSize). No-op if absent.
    injectWorkgroupSizeSpecConstant(spirv);
    // Make a dynamic shared array's length a spec constant (SpecId 3) set from
    // the launch's sharedBytes. No-op for kernels without dynamic shared.
    injectDynamicSharedSpecConstant(spirv);
    return spirv;
}

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
