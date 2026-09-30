// NVPTX backend — see header.

#include "NvptxBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <sstream>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

/// Target registry init; this may run before any Compiler has done it.
void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
    });
}

/// NVIDIA's libdevice bitcode, or empty if no install has it.
std::string findLibdevice() {
    auto has = [](const std::string& p) { return llvm::sys::fs::exists(p); };
    auto probe = [&](const std::string& root) -> std::string {
        std::string p = root + "/nvvm/libdevice/libdevice.10.bc";
        return has(p) ? p : std::string{};
    };
    if (const char* cp = std::getenv("CUDA_PATH")) {
        if (std::string p = probe(cp); !p.empty()) return p;
    }
    if (std::string p = probe("/usr/local/cuda"); !p.empty()) return p;
    // Compared NUMERICALLY: "cuda-9.0" sorts above "cuda-13.3" as text.
    std::error_code ec;
    std::string best;
    long bestMajor = -1, bestMinor = -1;
    for (llvm::sys::fs::directory_iterator it("/usr/local", ec), end;
         it != end && !ec; it.increment(ec)) {
        llvm::StringRef d = llvm::StringRef(it->path());
        size_t at = d.rfind("/cuda-");
        if (at == llvm::StringRef::npos) continue;
        llvm::StringRef ver = d.substr(at + 6);          // "13.3"
        long major = 0, minor = 0;
        auto [majStr, rest] = ver.split('.');
        if (majStr.getAsInteger(10, major)) continue;    // non-numeric -> skip
        if (!rest.empty()) rest.getAsInteger(10, minor); // absent minor -> 0
        if (probe(d.str()).empty()) continue;
        if (major > bestMajor || (major == bestMajor && minor > bestMinor)) {
            bestMajor = major; bestMinor = minor; best = d.str();
        }
    }
    return best.empty() ? std::string{} : probe(best);
}

/// True if `m` references any DECLARATION whose name starts with `prefix`.
bool referencesDeviceLib(llvm::Module& m, const char* prefix) {
    for (llvm::Function& fn : m)
        if (fn.isDeclaration() && fn.getName().starts_with(prefix))
            return true;
    return false;
}

/// Links libdevice into `m` ONLY when a __nv_* declaration is outstanding: most
/// kernels must not pay to parse ~500 KB of bitcode. Needed symbols only.
void linkCudaDeviceLibsIfNeeded(llvm::Module& m) {
    if (!referencesDeviceLib(m, "__nv_")) return;

    std::string path = findLibdevice();
    if (path.empty()) {
        llvm::errs() << "cajeta.xpu.nvidia: libdevice.10.bc not found (set "
                        "CUDA_PATH); device transcendentals (Math.exp/cos/...) "
                        "cannot be assembled — ptxas will report an unresolved "
                        "__nv_* extern and the kernel is skipped\n";
        return;
    }
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> lib =
        llvm::parseIRFile(path, err, m.getContext());
    if (!lib) {
        llvm::errs() << "cajeta.xpu.nvidia: cannot parse " << path << ": "
                     << err.getMessage() << "\n";
        return;
    }
    // Aligning libdevice's older datalayout and triple avoids a benign mismatch.
    lib->setDataLayout(m.getDataLayout());
    lib->setTargetTriple(m.getTargetTriple());
    if (llvm::Linker::linkModules(m, std::move(lib),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        llvm::errs() << "cajeta.xpu.nvidia: failed to link " << path << "\n";
        return;
    }

    // NVVMReflect folds libdevice's guards using these flags; without them the
    // reflect calls survive as externs. 0 preserves IEEE denormals.
    if (!m.getModuleFlag("nvvm-reflect-ftz"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-ftz", (uint32_t) 0);
    if (!m.getModuleFlag("nvvm-reflect-prec-sqrt"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-sqrt", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-prec-div"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-div", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-approx-func"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-approx-func", (uint32_t) 0);
}

/// Reads and writes of a kernel buffer at a PROVABLY word-aligned offset become
/// word-aligned accesses.
///
/// `KernelBuffer<int8>.vload<N>` is a `load <N x i8>` at alignment 1, and PTX
/// may not touch a word at an unaligned address, so the backend legalizes it
/// into N byte loads and rebuilds the words with `prmt` (260 `ld.global.b8` in
/// the hot Q4_K mat-vec). The index is nearly always a multiple of four by the
/// layout's own arithmetic (`row * blocks * 144 + block * 144 + 16 + 32 * g`),
/// and after the pipeline has promoted the locals that is a fact the known
/// bits of the address carry. Where they do, the access takes the alignment
/// and the backend emits words.
///
/// The proof is about the OFFSET; the base is the launch's. Each buffer
/// parameter an access was raised on is given `align` and listed in the
/// function attribute `cajeta-word-aligned-params`, which the registration
/// hands to the runtime, and the launch refuses a base that is not aligned,
/// by name, instead of faulting the context with a misaligned address.
/// An index nothing proves is left exactly as it was.
/// The number of low bits of an address that are provably zero, given that the
/// buffer base `base` is aligned to `1 << baseBits`.
///
/// LLVM's computeKnownBits answers the same question and gives up six
/// operators deep. The offset of a quantized row is deeper than that by
/// construction (`zext(row * (blocks * 144) + block * 144 + 32 * g) + 16` is
/// nine), so the hot loads were exactly the ones it could not see. This solves
/// the same algebra over the whole expression, without the limit: a product
/// adds its operands' zeros, a sum keeps the smaller, a loop-carried value is
/// the smaller of where it starts and what it steps by. It is a descending
/// fixpoint (every value starts at "all zeros" and is only ever lowered), so a
/// value reached through a loop is never credited with zeros its own phi
/// does not end up having. Anything outside the algebra is a leaf answered by
/// computeKnownBits.
class TrailingZeroProof {
public:
    TrailingZeroProof(const llvm::Argument* base, unsigned baseBits,
                      const llvm::DataLayout& dl)
        : base(base), baseBits(baseBits), dl(dl) {}

    unsigned of(llvm::Value* v) {
        collect(v);
        bool changed = true;
        while (changed) {
            changed = false;
            for (llvm::Value* n : order) {
                const unsigned now = transfer(n);
                unsigned& cur = val[n];
                if (now < cur) { cur = now; changed = true; }
            }
        }
        return get(v);
    }

private:
    static constexpr unsigned kAll = 64;
    const llvm::Argument* base;
    unsigned baseBits;
    const llvm::DataLayout& dl;
    llvm::DenseMap<llvm::Value*, unsigned> val;   // interior nodes and leaves
    std::vector<llvm::Value*> order;              // interior nodes only

    static bool interior(llvm::Value* v) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
            switch (bo->getOpcode()) {
                case llvm::Instruction::Add: case llvm::Instruction::Sub:
                case llvm::Instruction::Or:  case llvm::Instruction::Xor:
                case llvm::Instruction::And: case llvm::Instruction::Mul:
                case llvm::Instruction::Shl:
                    return true;
                default:
                    return false;
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) {
            return ci->getOpcode() == llvm::Instruction::ZExt
                || ci->getOpcode() == llvm::Instruction::SExt
                || ci->getOpcode() == llvm::Instruction::Trunc;
        }
        if (llvm::isa<llvm::SelectInst>(v) || llvm::isa<llvm::PHINode>(v)) return true;
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
            for (auto gti = llvm::gep_type_begin(gep), e = llvm::gep_type_end(gep);
                 gti != e; ++gti)
                if (gti.isStruct()) return false;
            return true;
        }
        if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
            switch (ii->getIntrinsicID()) {
                case llvm::Intrinsic::umin: case llvm::Intrinsic::umax:
                case llvm::Intrinsic::smin: case llvm::Intrinsic::smax:
                    return true;
                default:
                    return false;
            }
        }
        return false;
    }

    void collect(llvm::Value* root) {
        std::vector<llvm::Value*> work{root};
        while (!work.empty()) {
            llvm::Value* v = work.back();
            work.pop_back();
            if (llvm::isa<llvm::ConstantInt>(v) || v == base || val.count(v)) continue;
            if (!interior(v)) {
                val[v] = v->getType()->isIntOrPtrTy()
                    ? std::min<unsigned>(kAll,
                          llvm::computeKnownBits(v, dl).countMinTrailingZeros())
                    : 0;
                continue;
            }
            val[v] = kAll;
            order.push_back(v);
            auto* u = llvm::cast<llvm::User>(v);
            if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v)) {
                work.push_back(sel->getTrueValue());
                work.push_back(sel->getFalseValue());
            } else if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
                work.push_back(ii->getArgOperand(0));
                work.push_back(ii->getArgOperand(1));
            } else {
                for (llvm::Value* op : u->operands()) work.push_back(op);
            }
        }
    }

    unsigned get(llvm::Value* v) const {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
            return c->isZero() ? kAll
                               : std::min<unsigned>(kAll, c->getValue().countr_zero());
        if (v == base) return baseBits;
        auto it = val.find(v);
        return it == val.end() ? 0 : it->second;
    }

    unsigned transfer(llvm::Value* v) const {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
            const unsigned l = get(bo->getOperand(0));
            const unsigned r = get(bo->getOperand(1));
            switch (bo->getOpcode()) {
                case llvm::Instruction::And: return std::max(l, r);
                case llvm::Instruction::Mul: return std::min(kAll, l + r);
                case llvm::Instruction::Shl:
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(bo->getOperand(1)))
                        return (unsigned) std::min<uint64_t>(
                            kAll, l + c->getLimitedValue(kAll));
                    return l;
                default: return std::min(l, r);           // add, sub, or, xor
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) return get(ci->getOperand(0));
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v))
            return std::min(get(sel->getTrueValue()), get(sel->getFalseValue()));
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            unsigned tz = kAll;
            for (llvm::Value* in : phi->incoming_values()) tz = std::min(tz, get(in));
            return tz;
        }
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
            unsigned tz = get(gep->getPointerOperand());
            for (auto gti = llvm::gep_type_begin(gep), e = llvm::gep_type_end(gep);
                 gti != e; ++gti) {
                const uint64_t scale =
                    gti.getSequentialElementStride(dl).getFixedValue();
                const unsigned scaleTz =
                    scale ? (unsigned) llvm::countr_zero(scale) : kAll;
                tz = std::min(tz, std::min(kAll, get(gti.getOperand()) + scaleTz));
            }
            return tz;
        }
        if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v))
            return std::min(get(ii->getArgOperand(0)), get(ii->getArgOperand(1)));
        return 0;
    }
};

void raiseProvableBufferAlignment(llvm::Module& m) {
    const llvm::DataLayout& dl = m.getDataLayout();
    for (llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        std::set<unsigned> raised;
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst);
                auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst);
                if (!ld && !st) continue;
                if ((ld && !ld->isSimple()) || (st && !st->isSimple())) continue;
                llvm::Type* ty = ld ? ld->getType() : st->getValueOperand()->getType();
                llvm::Value* ptr = ld ? ld->getPointerOperand() : st->getPointerOperand();
                const uint64_t have = (ld ? ld->getAlign() : st->getAlign()).value();
                // What the access wants: a byte vector is read in words, anything
                // else at its own natural alignment, and never past a word (the
                // runtime's contract on a base is four bytes).
                uint64_t want = 0;
                if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ty)) {
                    // Whatever the element: the pipeline rewrites a byte
                    // vector it sees dotted into `<N/4 x i32>` at alignment 1,
                    // and the backend splits either into word pieces.
                    llvm::Type* et = vt->getElementType();
                    if (et->isIntegerTy() || et->isFloatingPointTy()) {
                        const uint64_t bytes = dl.getTypeStoreSize(ty).getFixedValue();
                        want = bytes % 4 == 0 ? 4 : bytes % 2 == 0 ? 2 : 0;
                    }
                } else if (ty->isIntegerTy() || ty->isFloatingPointTy()) {
                    want = std::min<uint64_t>(dl.getABITypeAlign(ty).value(), 4);
                }
                if (want <= have) continue;
                auto* arg = llvm::dyn_cast<llvm::Argument>(
                    llvm::getUnderlyingObject(ptr));
                if (!arg || !arg->getType()->isPointerTy()) continue;
                // The zeros of the address under the contract this pass would
                // be signing the launch up to: the base aligned to a word.
                const unsigned tz = TrailingZeroProof(arg, 2, dl).of(ptr);
                const uint64_t proven = std::min<uint64_t>(
                    want, uint64_t(1) << std::min<unsigned>(tz, 2));
                if (std::getenv("CAJETA_XPU_DEBUG_ALIGN"))
                    llvm::errs() << "align? " << f.getName() << " arg " << arg->getArgNo()
                                 << " have " << have << " want " << want << " tz "
                                 << tz << " : " << inst << "\n";
                if (proven > have) {
                    const llvm::Align al(proven);
                    if (ld) ld->setAlignment(al); else st->setAlignment(al);
                    raised.insert(arg->getArgNo());
                }
            }
        }
        // The contract, stated on the arguments it was taken on.
        for (unsigned i : raised) {
            llvm::Argument* arg = f.getArg(i);
            const llvm::MaybeAlign before = arg->getParamAlign();
            if (before && before->value() >= 4) continue;
            arg->removeAttr(llvm::Attribute::Alignment);
            arg->addAttr(llvm::Attribute::getWithAlignment(f.getContext(), llvm::Align(4)));
        }
        if (raised.empty()) continue;
        std::string list;
        for (unsigned i : raised) {
            if (!list.empty()) list += ",";
            list += std::to_string(i);
        }
        f.addFnAttr("cajeta-word-aligned-params", list);
    }
}

/// Runs the IR pipeline before PTX emission, libdevice linked first so the
/// merged bodies optimize with the kernel. Nothing else optimizes device IR.
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    linkCudaDeviceLibsIfNeeded(m);
    llvm::PassBuilder pb(&tm);
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Default O3; CAJETA_XPU_DEVICE_OPT=0|1|2|3 overrides.
    llvm::ModulePassManager mpm;
    int lvl = 3;
    if (const char* e = std::getenv("CAJETA_XPU_DEVICE_OPT")) lvl = std::atoi(e);
    if (lvl <= 0) {
        // The inliner is NOT optional here: lowerDeviceFn passes a @Device
        // helper the buffer BASE by value and relies on inlining to splice it
        // in. Left out-of-line the call reads a bogus base and faults at launch.
        mpm.addPass(llvm::AlwaysInlinerPass());
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());  // mem2reg
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    } else {
        llvm::OptimizationLevel ol = lvl == 1 ? llvm::OptimizationLevel::O1
                                   : lvl == 2 ? llvm::OptimizationLevel::O2
                                              : llvm::OptimizationLevel::O3;
        mpm = pb.buildPerModuleDefaultPipeline(ol);
    }
    mpm.run(m, mam);
    // After the pipeline: the proof needs the promoted, folded index.
    // CAJETA_XPU_NO_WORD_ALIGN=1 is the A/B arm.
    const char* noAlign = std::getenv("CAJETA_XPU_NO_WORD_ALIGN");
    if (lvl > 0 && !(noAlign && *noAlign && *noAlign != '0'))
        raiseProvableBufferAlignment(m);
}

} // namespace

std::unique_ptr<llvm::TargetMachine>
createNvptxTargetMachine(const std::string& arch) {
    ensureTargetsInitialized();

    llvm::Triple triple(kNvptxTriple);
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.nvidia: nvptx64 target not available: "
                     << error << "\n";
        return nullptr;
    }

    llvm::TargetOptions opt;
    // PTX is position-independent and ptxas does final placement.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/arch, /*Features=*/"", opt, /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kNvptxTriple));
    m.setDataLayout(tm.createDataLayout());
}

std::string emitPtx(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    // addPassesToEmitFile needs a raw_pwrite_stream; PTX is textual, so AssemblyFile.
    optimizeDeviceModule(deviceModule, tm);

    llvm::SmallString<0> buf;
    llvm::raw_svector_ostream os(buf);

    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr,
                               llvm::CodeGenFileType::AssemblyFile)) {
        llvm::errs() << "cajeta.xpu.nvidia: NVPTX TargetMachine cannot emit "
                        "assembly\n";
        return {};
    }
    pm.run(deviceModule);
    std::string ptx(buf.begin(), buf.end());
    // Same debugging seam as AMDGPU's CAJETA_XPU_DUMP_BC: a directory to drop
    // the emitted PTX into, so a kernel that misbehaves only inside a full test
    // run can be diffed against the same kernel compiled in isolation.
    if (const char* dumpDir = std::getenv("CAJETA_XPU_DUMP_PTX")) {
        std::error_code ec;
        llvm::raw_fd_ostream out(
            std::string(dumpDir) + "/" + deviceModule.getName().str() + ".ptx",
            ec);
        if (!ec) out << ptx;
    }
    return ptx;
}

std::string findPtxas() {
    if (const char* cudaPath = std::getenv("CUDA_PATH")) {
        for (const char* exe : {"/bin/ptxas.exe", "/bin/ptxas"}) {
            std::string p = std::string(cudaPath) + exe;
            if (llvm::sys::fs::exists(p)) return p;
        }
    }
    if (auto found = llvm::sys::findProgramByName("ptxas")) return *found;
    return {};
}

PtxasVersion parsePtxasVersion(const std::string& versionText) {
    // "Cuda compilation tools, release 12.0, V12.0.140" — the release pair is
    // what NVIDIA versions the assembler by; the build number after V adds
    // nothing this gate needs.
    size_t at = versionText.find("release ");
    if (at == std::string::npos) return {};
    llvm::StringRef rest(versionText);
    rest = rest.substr(at + std::strlen("release "));
    llvm::StringRef majStr = rest.take_while(llvm::isDigit);
    if (majStr.empty()) return {};
    rest = rest.substr(majStr.size());
    if (!rest.consume_front(".")) return {};
    llvm::StringRef minStr = rest.take_while(llvm::isDigit);
    if (minStr.empty()) return {};
    PtxasVersion v;
    if (majStr.getAsInteger(10, v.major)) return {};
    if (minStr.getAsInteger(10, v.minor)) return {};
    // A genuine release is never 0.0, so the unknown sentinel stays unambiguous.
    if (v.unknown()) return {};
    return v;
}

bool ptxasVersionSupported(const PtxasVersion& v) {
    if (v.unknown()) return true;
    return !(v < kMinPtxasVersion);
}

PtxasVersion queryPtxasVersion(const std::string& ptxasPath) {
    // Cached per path: assembleCubin runs once per kernel, and spawning ptxas
    // just to re-read a constant would be paid on every one of them.
    static std::map<std::string, PtxasVersion> cache;
    static std::mutex cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(ptxasPath);
        if (it != cache.end()) return it->second;
    }

    PtxasVersion v;
    llvm::SmallString<128> logPath;
    if (!llvm::sys::fs::createTemporaryFile("cajeta_ptxas_ver", "txt", logPath)) {
        std::string flag = "--version";
        llvm::SmallVector<llvm::StringRef, 2> args = {ptxasPath, flag};
        // ptxas prints --version on stdout; capture only that.
        std::optional<llvm::StringRef> redirects[3] = {
            std::nullopt, llvm::StringRef(logPath), std::nullopt};
        if (llvm::sys::ExecuteAndWait(ptxasPath, args, /*Env=*/std::nullopt,
                                      redirects) == 0) {
            if (auto buf = llvm::MemoryBuffer::getFile(logPath, /*IsText=*/true))
                v = parsePtxasVersion((*buf)->getBuffer().str());
        }
        llvm::sys::fs::remove(logPath);
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[ptxasPath] = v;
    return v;
}

std::vector<PtxasKernelStats> parsePtxasVerbose(const std::string& text) {
    std::vector<PtxasKernelStats> out;
    // The unsigned integer immediately before `suffix` on `line`; 0 if absent.
    auto numBefore = [](const std::string& line, const char* suffix) -> unsigned {
        size_t p = line.find(suffix);
        if (p == std::string::npos) return 0;
        size_t e = p;
        while (e > 0 && line[e - 1] == ' ') --e;
        size_t s = e;
        while (s > 0 && std::isdigit((unsigned char) line[s - 1])) --s;
        if (s == e) return 0;
        return (unsigned) std::strtoul(line.substr(s, e - s).c_str(), nullptr, 10);
    };
    std::istringstream ss(text);
    std::string line;
    PtxasKernelStats* cur = nullptr;
    while (std::getline(ss, line)) {
        if (size_t p = line.find("Function properties for "); p != std::string::npos) {
            std::string name = line.substr(p + std::strlen("Function properties for "));
            while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == ':'))
                name.pop_back();
            out.push_back(PtxasKernelStats{});
            cur = &out.back();
            cur->name = name;
            continue;
        }
        if (!cur) continue;
        if (line.find("bytes stack frame") != std::string::npos) {
            cur->stackBytes = numBefore(line, " bytes stack frame");
            cur->spillStoreBytes = numBefore(line, " bytes spill stores");
            cur->spillLoadBytes = numBefore(line, " bytes spill loads");
        }
        if (line.find("Used ") != std::string::npos
                && line.find(" registers") != std::string::npos) {
            cur->registers = numBefore(line, " registers");
            cur->smemBytes = numBefore(line, " bytes smem");
        }
    }
    return out;
}

std::vector<uint8_t> assembleCubin(const std::string& ptx,
                                   const std::string& arch,
                                   std::string* verboseLog) {
    std::string ptxas = findPtxas();
    if (ptxas.empty()) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas not found (set CUDA_PATH or "
                        "put ptxas on PATH)\n";
        return {};
    }

    // Which assembler was picked is invisible until it matters, and when it
    // matters it is a WRONG ANSWER rather than an error — so name it once, and
    // refuse outright below the floor. Reported per path so a run that somehow
    // switches assemblers says so.
    PtxasVersion ver = queryPtxasVersion(ptxas);
    if (!ptxasVersionSupported(ver)) {
        static std::set<std::string> refused;
        static std::mutex refusedMutex;
        bool first;
        {
            std::lock_guard<std::mutex> lock(refusedMutex);
            first = refused.insert(ptxas).second;
        }
        if (first) {
            llvm::errs()
                << "cajeta.xpu.nvidia: refusing to assemble with " << ptxas
                << " (CUDA " << ver.major << "." << ver.minor
                << "); cajeta requires " << kMinPtxasVersion.major << "."
                << kMinPtxasVersion.minor << " or newer.\n"
                << "  CUDA 12.0's ptxas miscompiles cooperative-matrix kernels "
                   "that spill a tile to the local frame — silently, with no "
                   "diagnostic and wrong device results.\n"
                << "  Point CUDA_PATH at a newer toolkit "
                   "(e.g. CUDA_PATH=/usr/local/cuda).\n";
        }
        return {};
    }

    llvm::SmallString<128> ptxPath, cubinPath;
    // Constructed BEFORE the temp files, so a later failure still removes them.
    struct Cleanup {
        llvm::SmallString<128> &a, &b;
        ~Cleanup() { llvm::sys::fs::remove(a); llvm::sys::fs::remove(b); }
    } cleanup{ptxPath, cubinPath};
    if (llvm::sys::fs::createTemporaryFile("cajeta_xpu", "ptx", ptxPath) ||
        llvm::sys::fs::createTemporaryFile("cajeta_xpu", "cubin", cubinPath)) {
        llvm::errs() << "cajeta.xpu.nvidia: could not create temp files\n";
        return {};
    }

    {
        std::error_code ec;
        llvm::raw_fd_ostream out(ptxPath, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            llvm::errs() << "cajeta.xpu.nvidia: could not write PTX: "
                         << ec.message() << "\n";
            return {};
        }
        out << ptx;
    }

    // ExecuteAndWait passes argv directly, so paths with spaces are safe.
    std::string archArg = "-arch=" + arch;
    std::string oFlag = "-o";
    std::string vFlag = "-v";
    llvm::SmallVector<llvm::StringRef, 8> args = {
        ptxas, archArg, ptxPath.str(), oFlag, cubinPath.str()};
    // `-v` prints the per-kernel resource report on stderr; the cubin is unaffected.
    llvm::SmallString<128> logPath;
    std::optional<llvm::StringRef> redirects[3] = {std::nullopt, std::nullopt, std::nullopt};
    bool capture = false;
    if (verboseLog) {
        verboseLog->clear();
        if (!llvm::sys::fs::createTemporaryFile("cajeta_xpu", "ptxas.log", logPath)) {
            args.push_back(vFlag);
            redirects[2] = llvm::StringRef(logPath);
            capture = true;
        }
    }
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(
        ptxas, args, /*Env=*/std::nullopt,
        /*Redirects=*/capture ? llvm::ArrayRef<std::optional<llvm::StringRef>>(redirects)
                              : llvm::ArrayRef<std::optional<llvm::StringRef>>(),
        /*SecondsToWait=*/0, /*MemoryLimit=*/0, &errMsg);
    if (capture) {
        if (auto log = llvm::MemoryBuffer::getFile(logPath, /*IsText=*/true))
            *verboseLog = (*log)->getBuffer().str();
        llvm::sys::fs::remove(logPath);
    }
    if (rc != 0) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas failed (rc=" << rc << ") "
                     << errMsg << "\n";
        if (capture && verboseLog && !verboseLog->empty())
            llvm::errs() << *verboseLog << "\n";
        return {};
    }

    auto buf = llvm::MemoryBuffer::getFile(cubinPath, /*IsText=*/false);
    if (!buf) {
        llvm::errs() << "cajeta.xpu.nvidia: could not read cubin: "
                     << buf.getError().message() << "\n";
        return {};
    }
    llvm::StringRef bytes = (*buf)->getBuffer();
    return std::vector<uint8_t>(bytes.bytes_begin(), bytes.bytes_end());
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
