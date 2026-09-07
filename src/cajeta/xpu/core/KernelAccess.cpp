//
// KernelAccess — pointer-provenance classification of a lowered kernel's
// buffer accesses. See the header.
//

#include "KernelAccess.h"
#include "XpuAttributes.h"

#include "cajeta/error/Exception.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaType.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"

#include <functional>
#include <map>

namespace cajeta {
namespace xpu {

namespace {

    // The buffer-like parameters of `method`, in declaration order, with their
    // index among ALL declared parameters (the IR argument / descriptor binding
    // index) and the manifest kind. Scalars, samplers, acceleration structures
    // are not access entries.
    struct BufferParam {
        std::string name;
        unsigned declIndex = 0;   // position among declared params (no `this`)
        std::string kind;         // kernelBuffer | image
        bool readOnlyKind = false;   // sampled textures: never written
        const FormalParameter* field = nullptr;
    };

    std::vector<BufferParam> bufferParams(const MethodPtr& method) {
        std::vector<BufferParam> out;
        if (!method) return out;
        unsigned idx = 0;
        for (auto& p : method->getParameterList()) {
            if (!p) continue;
            if (p->getName() == "this") continue;
            std::string canonical = p->getType() ? p->getType()->toCanonical() : std::string();
            BufferParam bp;
            bp.name = p->getName();
            bp.declIndex = idx++;
            bp.field = p.get();
            auto starts = [&](const char* prefix) {
                return canonical.rfind(prefix, 0) == 0;
            };
            if (starts("cajeta.xpu.KernelBuffer<") || starts("cajeta.xpu.KernelBuffer")) {
                bp.kind = "kernelBuffer";
            } else if (starts("cajeta.xpu.Image")) {
                bp.kind = "image";
            } else if (starts("cajeta.xpu.Texture")) {
                bp.kind = "image";
                bp.readOnlyKind = true;
            } else {
                continue;
            }
            out.push_back(std::move(bp));
        }
        return out;
    }

    // Walk `v` back to its roots: kernel arguments and SPIR-V
    // resource-handle calls. Bounded, cycle-safe.
    void collectRoots(const llvm::Value* v,
                      llvm::SmallPtrSetImpl<const llvm::Value*>& roots,
                      llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                      unsigned depth) {
        if (!v || depth > 96 || !seen.insert(v).second) return;
        if (llvm::isa<llvm::Argument>(v)) { roots.insert(v); return; }
        if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(v)) {
            collectRoots(gep->getPointerOperand(), roots, seen, depth + 1);
            return;
        }
        if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) {
            collectRoots(ci->getOperand(0), roots, seen, depth + 1);
            return;
        }
        if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
            if (ce->isCast() || ce->getOpcode() == llvm::Instruction::GetElementPtr)
                collectRoots(ce->getOperand(0), roots, seen, depth + 1);
            return;
        }
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            for (const llvm::Value* in : phi->incoming_values())
                collectRoots(in, roots, seen, depth + 1);
            return;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v)) {
            collectRoots(sel->getTrueValue(), roots, seen, depth + 1);
            collectRoots(sel->getFalseValue(), roots, seen, depth + 1);
            return;
        }
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
            // A pointer reloaded from a slot: the slot's stored values are the
            // provenance (the lowering spills params to allocas). A pointer
            // loaded THROUGH a parameter (a bindless handle array) is an access
            // to that parameter too, so the pointer operand's roots count.
            const llvm::Value* slot = ld->getPointerOperand()->stripPointerCasts();
            if (auto* al = llvm::dyn_cast<llvm::AllocaInst>(slot)) {
                for (const llvm::User* u : al->users())
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                        if (st->getPointerOperand()->stripPointerCasts() == al)
                            collectRoots(st->getValueOperand(), roots, seen, depth + 1);
                return;
            }
            collectRoots(ld->getPointerOperand(), roots, seen, depth + 1);
            return;
        }
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(v)) {
            if (const llvm::Function* f = call->getCalledFunction()) {
                llvm::StringRef n = f->getName();
                if (n.contains("spv.resource.getpointer")) {
                    collectRoots(call->getArgOperand(0), roots, seen, depth + 1);
                    return;
                }
                if (n.contains("spv.resource.handlefrombinding")) {
                    roots.insert(call);
                    return;
                }
            }
            return;
        }
    }

    // Does the element address `ptr` name a compile-time-constant element?
    // (A GEP with constant indices off the parameter, the parameter itself, or
    // a SPIR-V getpointer with a constant index.)
    bool constantElement(const llvm::Value* ptr) {
        const llvm::Value* v = ptr;
        for (unsigned depth = 0; v && depth < 32; ++depth) {
            if (llvm::isa<llvm::Argument>(v)) return true;
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(v)) {
                for (const llvm::Value* idx : gep->indices())
                    if (!llvm::isa<llvm::ConstantInt>(idx)) return false;
                v = gep->getPointerOperand();
                continue;
            }
            if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) { v = ci->getOperand(0); continue; }
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(v)) {
                if (const llvm::Function* f = call->getCalledFunction()) {
                    if (f->getName().contains("spv.resource.getpointer"))
                        return call->arg_size() > 1
                            && llvm::isa<llvm::ConstantInt>(call->getArgOperand(1));
                    if (f->getName().contains("spv.resource.handlefrombinding")) return true;
                }
                return false;
            }
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
                // A pointer reloaded from a slot names the same element the
                // stored pointer did; take the single-store case.
                const llvm::Value* slot = ld->getPointerOperand()->stripPointerCasts();
                if (auto* al = llvm::dyn_cast<llvm::AllocaInst>(slot)) {
                    const llvm::Value* stored = nullptr;
                    for (const llvm::User* u : al->users())
                        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                            if (st->getPointerOperand()->stripPointerCasts() == al) {
                                if (stored) return false;
                                stored = st->getValueOperand();
                            }
                    if (!stored) return false;
                    v = stored;
                    continue;
                }
                return false;
            }
            return false;
        }
        return false;
    }

    enum class Op { Load, Store, Atomic };

    // Which buffer parameter (index into `params`) a root belongs to, or -1.
    int paramOfRoot(const llvm::Value* root, const std::vector<BufferParam>& params) {
        if (auto* arg = llvm::dyn_cast<llvm::Argument>(root)) {
            for (size_t i = 0; i < params.size(); ++i)
                if (arg->getName() == params[i].name) return (int) i;
            return -1;
        }
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(root)) {
            // llvm.spv.resource.handlefrombinding(set, binding, ...): the lowering
            // binds parameter i at binding i (SpirvKernelLowering::materializeParam).
            if (call->arg_size() > 1)
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1)))
                    for (size_t i = 0; i < params.size(); ++i)
                        if (params[i].declIndex == c->getZExtValue()) return (int) i;
            // Or by the value's name, which the lowering sets to the parameter's.
            for (size_t i = 0; i < params.size(); ++i)
                if (call->getName() == params[i].name) return (int) i;
        }
        return -1;
    }

    // Visit every memory operation in `kfn` that addresses one of `params`.
    void forEachAccess(llvm::Function& kfn, const std::vector<BufferParam>& params,
                       const std::function<void(llvm::Instruction&, Op, int paramIdx,
                                                const llvm::Value* ptr)>& visit) {
        auto rootsOf = [&](const llvm::Value* ptr, llvm::SmallVectorImpl<int>& out) {
            llvm::SmallPtrSet<const llvm::Value*, 8> roots, seen;
            collectRoots(ptr, roots, seen, 0);
            for (const llvm::Value* r : roots) {
                int p = paramOfRoot(r, params);
                if (p >= 0) out.push_back(p);
            }
        };
        for (llvm::BasicBlock& bb : kfn) {
            for (llvm::Instruction& inst : bb) {
                llvm::SmallVector<int, 4> ps;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                    rootsOf(ld->getPointerOperand(), ps);
                    for (int p : ps) visit(inst, Op::Load, p, ld->getPointerOperand());
                } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                    rootsOf(st->getPointerOperand(), ps);
                    for (int p : ps) visit(inst, Op::Store, p, st->getPointerOperand());
                } else if (auto* rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst)) {
                    rootsOf(rmw->getPointerOperand(), ps);
                    for (int p : ps) visit(inst, Op::Atomic, p, rmw->getPointerOperand());
                } else if (auto* cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst)) {
                    rootsOf(cx->getPointerOperand(), ps);
                    for (int p : ps) visit(inst, Op::Atomic, p, cx->getPointerOperand());
                } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    const llvm::Function* f = call->getCalledFunction();
                    if (!f) continue;
                    llvm::StringRef n = f->getName();
                    // Address computations are not accesses.
                    if (n.contains("spv.resource.getpointer")
                            || n.contains("spv.resource.handlefrombinding")) continue;
                    if (auto* mt = llvm::dyn_cast<llvm::MemTransferInst>(call)) {
                        rootsOf(mt->getRawDest(), ps);
                        for (int p : ps) visit(inst, Op::Store, p, mt->getRawDest());
                        ps.clear();
                        rootsOf(mt->getRawSource(), ps);
                        for (int p : ps) visit(inst, Op::Load, p, mt->getRawSource());
                        continue;
                    }
                    if (auto* ms = llvm::dyn_cast<llvm::MemSetInst>(call)) {
                        rootsOf(ms->getRawDest(), ps);
                        for (int p : ps) visit(inst, Op::Store, p, ms->getRawDest());
                        continue;
                    }
                    if (!f->isIntrinsic()) continue;
                    // Image / masked / vector-predicated memory intrinsics: the
                    // name says which way the data moves.
                    std::string lower = n.lower();
                    bool isStore = lower.find("store") != std::string::npos
                                || lower.find("write") != std::string::npos;
                    bool isLoad = lower.find("load") != std::string::npos
                               || lower.find("read") != std::string::npos
                               || lower.find("sample") != std::string::npos
                               || lower.find("fetch") != std::string::npos
                               || lower.find("gather") != std::string::npos;
                    bool isAtomic = lower.find("atomic") != std::string::npos;
                    if (!isStore && !isLoad && !isAtomic) continue;
                    for (unsigned a = 0; a < call->arg_size(); ++a) {
                        ps.clear();
                        rootsOf(call->getArgOperand(a), ps);
                        for (int p : ps)
                            visit(inst, isAtomic ? Op::Atomic : isStore ? Op::Store : Op::Load,
                                  p, call->getArgOperand(a));
                    }
                }
            }
        }
    }

    struct Tally {
        unsigned loads = 0, stores = 0, atomics = 0;
        unsigned nontemporal = 0;        // loads + stores carrying !nontemporal
        bool everyWriteConstant = true;  // stores + atomics on constant elements
    };

    std::vector<Tally> tally(llvm::Function& kfn, const std::vector<BufferParam>& params) {
        std::vector<Tally> t(params.size());
        forEachAccess(kfn, params,
            [&](llvm::Instruction& inst, Op op, int p, const llvm::Value* ptr) {
                Tally& x = t[(size_t) p];
                switch (op) {
                    case Op::Load:   x.loads++; break;
                    case Op::Store:  x.stores++; break;
                    case Op::Atomic: x.atomics++; break;
                }
                if (op != Op::Atomic && inst.getMetadata(llvm::LLVMContext::MD_nontemporal))
                    x.nontemporal++;
                if (op != Op::Load && !constantElement(ptr)) x.everyWriteConstant = false;
            });
        return t;
    }

    std::string derivedMode(const Tally& t, bool readOnlyKind) {
        if (readOnlyKind) return "read";
        if (t.atomics) return "accumulate";
        if (t.loads && t.stores) return "readwrite";
        if (t.stores) return "write";
        return "read";   // loads, or untouched: the conservative membership
    }

    // `@Access(mode)` on the parameter, or "".
    std::string declaredMode(const BufferParam& bp) {
        if (!bp.field) return {};
        auto ann = bp.field->findAnnotation(XpuAttr::Access);
        if (!ann) return {};
        std::string v = ann->getString();
        // `@Access("write")` and `@Access(write)` both arrive as the text.
        while (!v.empty() && (v.front() == '"' || v.front() == ' ')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '"' || v.back() == ' ')) v.pop_back();
        return v;
    }

} // namespace

KernelAccessSummary classifyKernelAccess(llvm::Function& kfn, const MethodPtr& method) {
    KernelAccessSummary out;
    std::vector<BufferParam> params = bufferParams(method);
    std::vector<Tally> t = tally(kfn, params);
    bool anyWrite = false;
    bool allWritesConstant = true;
    out.restartable = true;
    for (size_t i = 0; i < params.size(); ++i) {
        KernelAccessEntry e;
        e.param = params[i].name;
        e.kind = params[i].kind;
        std::string declared = declaredMode(params[i]);
        e.mode = declared.empty() ? derivedMode(t[i], params[i].readOnlyKind) : declared;
        e.origin = declared.empty() ? "derived" : "declared";
        unsigned plain = t[i].loads + t[i].stores;
        e.streaming = plain > 0 && t[i].nontemporal == plain;
        if (e.mode == "readwrite" || e.mode == "accumulate") out.restartable = false;
        if (t[i].stores || t[i].atomics) {
            anyWrite = true;
            if (!t[i].everyWriteConstant) allWritesConstant = false;
        }
        out.entries.push_back(std::move(e));
    }
    out.drainsDevice = anyWrite && allWritesConstant;
    return out;
}

void applyAccessDeclarations(llvm::Function& kfn, const MethodPtr& method,
                             bool nontemporalSupported) {
    std::vector<BufferParam> params = bufferParams(method);
    if (params.empty()) return;

    // @Streaming: tag the parameter's loads and stores non-temporal where the
    // backend lowers it. The manifest reads the tag back off the IR.
    if (nontemporalSupported) {
        llvm::LLVMContext& ctx = kfn.getContext();
        llvm::MDNode* nt = llvm::MDNode::get(
            ctx, llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)));
        std::vector<bool> streaming(params.size(), false);
        bool any = false;
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].field && params[i].field->findAnnotation(XpuAttr::Streaming)) {
                streaming[i] = true;
                any = true;
            }
        if (any) {
            forEachAccess(kfn, params,
                [&](llvm::Instruction& inst, Op op, int p, const llvm::Value*) {
                    if (op == Op::Atomic || !streaming[(size_t) p]) return;
                    if (llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::StoreInst>(inst))
                        inst.setMetadata(llvm::LLVMContext::MD_nontemporal, nt);
                });
        }
    }

    // @Access: the body must not contradict the declaration (§6.2).
    std::vector<Tally> t = tally(kfn, params);
    for (size_t i = 0; i < params.size(); ++i) {
        std::string declared = declaredMode(params[i]);
        if (declared.empty()) continue;
        const Tally& x = t[i];
        std::string contradiction;
        if (declared == "read") {
            if (x.stores) contradiction = "stores to it";
            else if (x.atomics) contradiction = "updates it atomically";
        } else if (declared == "write") {
            if (x.loads) contradiction = "loads from it";
            else if (x.atomics) contradiction = "updates it atomically";
        } else if (declared == "readwrite") {
            if (x.atomics) contradiction = "updates it atomically (declare `accumulate`)";
        } else if (declared == "indirect") {
            if (x.stores || x.atomics) contradiction = "writes it";
        } else if (declared != "accumulate") {
            throw cajeta::Exception(
                "`@Access(" + declared + ")` on parameter `" + params[i].name + "` of `"
                + method->getName() + "`: unknown access mode; one of read, write, "
                "readwrite, accumulate, indirect",
                "CAJETA_ERROR_XPU_ACCESS_UNKNOWN");
        }
        if (!contradiction.empty()) {
            throw cajeta::Exception(
                "`@Access(" + declared + ")` on parameter `" + params[i].name + "` of `"
                + method->getName() + "`, but the body " + contradiction + ". A declared "
                "mode narrows what the body does; it cannot contradict it (xpu-tile-manifest "
                "§6.2). Drop the annotation to record the derived mode, or fix the body.",
                "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED");
        }
    }
}

} // namespace xpu
} // namespace cajeta
