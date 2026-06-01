//
// Shared @Kernel AST → device llvm::Function lowering — see header.
//

#include "KernelLowering.h"
#include "LoweringTarget.h"

#include "../../method/Method.h"
#include "../../type/FormalParameter.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
#include "../core/XpuAttributes.h"
#include "../../error/Exception.h"

#include "../../asn/AbstractSyntaxNode.h"
#include "../../asn/Block.h"
#include "../../asn/Statement.h"
#include "../../asn/LocalVariableDeclaration.h"
#include "../../asn/VariableDeclarator.h"
#include "../../asn/expression/Expression.h"
#include "../../asn/expression/Identifier.h"
#include "../../asn/expression/MethodCallExpression.h"
#include "../../asn/expression/BinaryOpExpression.h"
#include "../../asn/expression/LiteralExpression.h"
#include "../../asn/expression/NewExpression.h"
#include "../../asn/expression/CreatorRest.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {

// Global thread index = workgroupId * workgroupDim + threadId. This identity
// holds on both NVPTX (ctaid*ntid+tid) and AMDGPU (workgroup.id*wgsize+
// workitem.id), so it lives in the shared base; a backend with a native
// global-id intrinsic can override.
llvm::Value* LoweringTarget::globalId(llvm::IRBuilderBase& b, llvm::Module& m,
                                      unsigned dim) {
    llvm::Value* wid = workgroupId(b, m, dim);
    llvm::Value* wdim = workgroupDim(b, m, dim);
    llvm::Value* tid = threadId(b, m, dim);
    return b.CreateAdd(b.CreateMul(wid, wdim), tid, "gid");
}

// Defined far below (after the anonymous-namespace block); forward-declared in
// cajeta::xpu so DeviceLowerer (in that block) can lower @Device helpers.
static std::vector<LoweringTarget::KernelParam> collectParams(
        const MethodPtr& method, llvm::LLVMContext& ctx);

namespace {

[[noreturn]] void unsupported(const std::string& what) {
    throw cajeta::Exception(
        "XPU kernel lowering: unsupported construct — " + what,
        "XPU-N01");
}

// addrspace(1) == device global memory, addrspace(3) == workgroup / shared
// memory. NVPTX and AMDGPU agree on both (AddressSpace.h: Global->1,
// Shared->3), so these are shared constants, not a fork point.
constexpr unsigned kGlobalAS = 1;
constexpr unsigned kSharedAS = 3;

// Map a primitive CajetaType to a device LLVM scalar type, built fresh
// in the device context (NOT via CajetaType::getLlvmType(), whose cache
// is bound to the host context). Returns nullptr for non-primitives.
llvm::Type* deviceScalarType(const CajetaTypePtr& t, llvm::LLVMContext& ctx) {
    if (!t) return nullptr;
    unsigned long f = t->getTypeFlags();
    if (!(f & PRIMITIVE_FLAG)) return nullptr;
    if (f & FLOAT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getDoubleTy(ctx);
        if (f & BIT_16_FLAG) return llvm::Type::getHalfTy(ctx);
        return llvm::Type::getFloatTy(ctx);            // BIT_32 default
    }
    if (f & INT_FLAG) {
        if (f & BIT_64_FLAG) return llvm::Type::getInt64Ty(ctx);
        if (f & BIT_32_FLAG) return llvm::Type::getInt32Ty(ctx);
        if (f & BIT_16_FLAG) return llvm::Type::getInt16Ty(ctx);
        if (f & BIT_8_FLAG)  return llvm::Type::getInt8Ty(ctx);
        return llvm::Type::getInt1Ty(ctx);             // boolean (no BIT flag)
    }
    return nullptr;
}

bool isBufferType(const CajetaTypePtr& t) {
    if (!t) return false;
    const std::string& c = t->toCanonical();
    static const std::string kPrefix = "cajeta.xpu.core.Buffer";
    return c.compare(0, kPrefix.size(), kPrefix) == 0;
}

bool typeIsSigned(const CajetaTypePtr& t) {
    return t && (t->getTypeFlags() & SIGNED_FLAG);
}

// One device kernel's worth of lowering state.
class DeviceLowerer {
public:
    using DeviceFnCache = std::map<const Method*, llvm::Function*>;

    DeviceLowerer(llvm::Module& m, llvm::Function* fn, LoweringTarget& target)
        : mod(m), ctx(m.getContext()), builder(ctx), fn(fn), target(target) {}

    // @Device helper-call context: `cls` resolves a bare helper name to its
    // sibling method; `deviceFns` is a cache of already-lowered @Device functions
    // shared across the kernel and all helpers (a nullptr entry = currently being
    // lowered → a recursive call, which is rejected).
    void setDeviceContext(std::shared_ptr<CajetaClass> c, DeviceFnCache* cache) {
        cls = std::move(c);
        deviceFns = cache;
    }

    // The admitted kernel params (computed by lowerKernel); materialized into
    // the entry block by lowerBody via target.materializeParam (allocas /
    // descriptor binds need the entry block to exist first).
    void setParams(std::vector<LoweringTarget::KernelParam> p) {
        kparams = std::move(p);
    }

    void lowerBody(const MethodPtr& method) {
        if (!cls) cls = method->getParent();   // for @Device helper resolution
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        // Materialize params now that the entry block exists. Scalars get a
        // mutable alloca slot (so they can be reassigned / serve as loop
        // counters); buffers keep their base/handle. HOW a param's runtime
        // value is obtained is the backend's call (target.materializeParam):
        // NVPTX/AMDGPU read fn->getArg(idx); Vulkan binds a descriptor here.
        unsigned idx = 0;
        for (auto& p : kparams) {
            llvm::Value* v = target.materializeParam(builder, mod, fn, idx++, p);
            if (p.isBuffer) {
                bufferBases[p.name] = v;
                bufferElems[p.name] = p.type;
                bufferElemSigned[p.name] = p.isSigned;
            } else {
                llvm::Value* slot = entryAlloca(p.type, p.name);
                builder.CreateStore(v, slot);
                values[p.name] = slot;
                slotTypes[p.name] = p.type;
                signedness[p.name] = p.isSigned;
            }
        }
        lowerStatement(method->getBlock());
        // Kernels return void; close any open block.
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateRetVoid();
        }
    }

private:
    llvm::Module& mod;
    llvm::LLVMContext& ctx;
    llvm::IRBuilder<> builder;
    llvm::Function* fn;
    LoweringTarget& target;
    // Scalar locals/params live in entry-block allocas (mutable: load on
    // read, store on write) so loops and reassignment work. `values` maps a
    // scalar name to its alloca slot; `slotTypes` to the slot element type.
    // Buffer parameters are never reassigned, so their addrspace(1) base
    // pointer is kept directly in `bufferBases` rather than behind an alloca.
    std::map<std::string, llvm::Value*> values;       // scalar name -> alloca slot
    std::map<std::string, llvm::Type*> slotTypes;     // scalar name -> slot elem type
    std::map<std::string, bool> signedness;
    std::map<std::string, llvm::Value*> bufferBases;  // buffer name -> addrspace(1) ptr
    std::map<std::string, llvm::Type*> bufferElems;   // buffer name -> element type

    std::vector<LoweringTarget::KernelParam> kparams;  // admitted params
    std::map<std::string, bool> bufferElemSigned;  // buffer name -> elem signed?
    std::shared_ptr<CajetaClass> cls;              // declaring class (helper resolution)
    DeviceFnCache* deviceFns = nullptr;            // shared @Device function cache
    // A dynamic shared array kept TYPED (Vulkan): name -> {array global, array
    // type}. Indexed as gep(arrTy, gv, {0, i}) so the SPIR-V OpTypeArray survives
    // (a spec-constant length needs it), vs the decayed-to-T* base for others.
    std::map<std::string, std::pair<llvm::Value*, llvm::Type*>> arrayShared;
    // At most one dynamic (runtime-sized) shared array per kernel — the
    // extern unsized addrspace(3) region is a single base; multiple would
    // alias (CUDA extern __shared__ / HIP HIP_DYNAMIC_SHARED both single).
    bool emittedDynamicShared = false;

    // continue/break targets for the innermost enclosing loop.
    struct LoopTarget { llvm::BasicBlock* continueBB; llvm::BasicBlock* breakBB; };
    std::vector<LoopTarget> loopTargets;

    // Allocate a slot in the function entry block (so it dominates every use
    // regardless of which loop/branch block is current). The alloca address
    // space is the backend's (0 on NVPTX, 5/private on AMDGPU) — a fork point
    // (cajeta-amd.md §2). Mirrors the host's entry-positioned-IRBuilder idiom.
    llvm::AllocaInst* entryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::BasicBlock& entry = fn->getEntryBlock();
        llvm::IRBuilder<> eb(&entry, entry.begin());
        return eb.CreateAlloca(ty, target.allocaAddressSpace(), nullptr,
                               name + ".slot");
    }

    // ---- statements -----------------------------------------------------

    void lowerStatement(const AbstractSyntaxNodePtr& node) {
        if (!node) return;
        if (auto blk = std::dynamic_pointer_cast<Block>(node)) {
            for (auto& s : blk->getChildren()) lowerStatement(s);
            return;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            lowerStatement(ls->getBlock());
            return;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            lowerLocalDecl(lvd);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            lowerIf(ifs);
            return;
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            lowerFor(fs);
            return;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            lowerWhile(ws);
            return;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            lowerDo(ds);
            return;
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            lowerEnhancedFor(efs);
            return;
        }
        if (auto bs = std::dynamic_pointer_cast<BreakStatement>(node)) {
            if (!bs->getLabel().empty()) unsupported("labeled break");
            if (loopTargets.empty()) unsupported("break outside loop");
            builder.CreateBr(loopTargets.back().breakBB);
            // Trailing (dead) statements need somewhere to land; the
            // enclosing loop's tail-branch fixup terminates this block.
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.break", fn));
            return;
        }
        if (auto cs = std::dynamic_pointer_cast<ContinueStatement>(node)) {
            if (!cs->getLabel().empty()) unsupported("labeled continue");
            if (loopTargets.empty()) unsupported("continue outside loop");
            builder.CreateBr(loopTargets.back().continueBB);
            builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "after.continue", fn));
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            lowerExprStatement(es->getExpression());
            return;
        }
        if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            // @Kernel returns void; a @Device helper returns its value.
            if (fn->getReturnType()->isVoidTy() || !rs->getExpression()) {
                builder.CreateRetVoid();
            } else {
                llvm::Value* v = lowerExpr(rs->getExpression());
                builder.CreateRet(coerceTo(v, fn->getReturnType()));
            }
            return;
        }
        unsupported("statement form in kernel body");
    }

    void lowerLocalDecl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
        CajetaTypePtr declType = lvd->getType();
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            const std::string& nm = vd->getIdentifier();
            llvm::Type* slotTy = deviceScalarType(declType, ctx);
            auto init = vd->getInitializer();
            if (!init || init->getChildren().empty()) {
                // No initializer — reserve the slot; a later assignment fills it.
                if (!slotTy) unsupported("uninitialized local of non-scalar type");
                values[nm] = entryAlloca(slotTy, nm);
                slotTypes[nm] = slotTy;
                signedness[nm] = typeIsSigned(declType);
                continue;
            }
            auto initExpr = std::dynamic_pointer_cast<Expression>(
                init->getChildren()[0]);
            // `Shared<T> tile = shared T[N];` — workgroup-shared memory. The
            // `shared` placement keyword flags the array creation; lower it to a
            // per-block addrspace(3) global instead of a scalar slot.
            if (auto ne = std::dynamic_pointer_cast<NewExpression>(initExpr)) {
                if (ne->getSharedAlloc()) {
                    lowerSharedDecl(nm, declType, ne);
                    continue;
                }
            }
            llvm::Value* v = lowerExpr(initExpr);
            if (!slotTy) slotTy = v->getType();  // infer slot type from initializer
            llvm::Value* slot = entryAlloca(slotTy, nm);
            builder.CreateStore(coerceTo(v, slotTy), slot);
            values[nm] = slot;
            slotTypes[nm] = slotTy;
            signedness[nm] = typeIsSigned(declType);
        }
    }

    // `Shared<T> name = shared T[size];` — workgroup-shared memory. Shared
    // memory is reserved per block (every thread sees the same region), NOT a
    // per-thread alloca, so it lowers to ONE module-level addrspace(3) global;
    // we then register its decayed element pointer in the buffer maps, after
    // which indexing (tile[i]), assignment, and compound-assignment all reuse
    // the existing addrspace-agnostic buffer path (LLVM tracks the address
    // space on the pointer). Two flavors, by whether `size` folds:
    //   - constant size N -> STATIC: an internal [N x T] global (per-block,
    //     reserved by the assembler).
    //   - runtime size     -> DYNAMIC: an external, unsized [0 x T] global.
    //     The byte count comes from the launch config (sharedMemBytes /
    //     groupMemBytes); both runtimes allow one such region/kernel.
    void lowerSharedDecl(const std::string& nm, const CajetaTypePtr& declType,
                         const std::shared_ptr<NewExpression>& ne) {
        // Element type comes from the Shared<T> LHS type argument (T).
        llvm::Type* elemTy = nullptr;
        bool elemSigned = true;
        if (auto cls = std::dynamic_pointer_cast<CajetaClass>(declType)) {
            if (!cls->getTypeArguments().empty()) {
                elemTy = deviceScalarType(cls->getTypeArguments()[0], ctx);
                elemSigned = typeIsSigned(cls->getTypeArguments()[0]);
            }
        }
        if (!elemTy) unsupported("shared local '" + nm +
                                 "' needs a scalar element type (Shared<T>)");

        // Size from the array creator's single size operand.
        auto acr = std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest());
        if (!acr) unsupported("shared local '" + nm +
                              "' must be an array creation: `shared T[size]`");
        if (acr->getChildren().size() != 1) {
            unsupported("shared array '" + nm +
                        "' must have exactly one dimension");
        }
        auto sizeExpr = std::dynamic_pointer_cast<Expression>(
            acr->getChildren()[0]);
        llvm::Value* sizeV = lowerExpr(sizeExpr);  // constant => static path

        // Static [N x T] (internal, undef) vs dynamic [0 x T] (external). The
        // dynamic size value is unused on the device — the launch sizes it —
        // and is left for DCE.
        llvm::GlobalValue::LinkageTypes linkage;
        llvm::Constant* init;
        uint64_t n;
        bool isDynamic = false;
        if (auto* sizeCI = llvm::dyn_cast<llvm::ConstantInt>(sizeV)) {
            n = sizeCI->getZExtValue();
            if (n == 0) unsupported("shared array '" + nm + "' size must be > 0");
            linkage = llvm::GlobalValue::InternalLinkage;
        } else {
            if (emittedDynamicShared) {
                unsupported("at most one dynamic (runtime-sized) shared array "
                            "per kernel; '" + nm + "' is a second");
            }
            emittedDynamicShared = true;
            isDynamic = true;
            if (target.dynamicSharedNeedsConcreteSize()) {
                // Vulkan: a concrete INTERNAL [N x T] placeholder (N>1 so it
                // stays an OpTypeArray, not a decayed scalar); the SPIR-V
                // post-emit pass rewrites its length to a spec constant the
                // launch's sharedBytes sets at pipeline creation. N is just the
                // spec constant's default.
                n = 256;
                linkage = llvm::GlobalValue::InternalLinkage;
            } else {
                n = 0;                                  // native extern shared
                linkage = llvm::GlobalValue::ExternalLinkage;
            }
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(elemTy, n);
        init = (linkage == llvm::GlobalValue::ExternalLinkage)
                   ? nullptr                            // external: no initializer
                   : (llvm::Constant*) llvm::UndefValue::get(arrTy);
        // '_' not '.': the device global's name becomes a target symbol, and '.'
        // is a directive separator in PTX/AMDGCN asm. A concrete dynamic-shared
        // array (Vulkan) is prefixed so the SPIR-V pass finds it by OpName.
        std::string gname = fn->getName().str() + "_" + nm;
        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage)
            gname = "cajeta_dynsh_" + gname;
        auto* gv = new llvm::GlobalVariable(
            mod, arrTy, /*isConstant=*/false, linkage, init,
            gname, /*InsertBefore=*/nullptr,
            llvm::GlobalValue::NotThreadLocal, kSharedAS);
        gv->setAlignment(llvm::MaybeAlign(16));

        if (isDynamic && linkage == llvm::GlobalValue::InternalLinkage) {
            // Vulkan dynamic shared: keep the array TYPED (don't decay to T*), so
            // its OpTypeArray survives for the spec-constant length patch.
            arrayShared[nm] = {gv, arrTy};
            bufferElems[nm] = elemTy;
            bufferElemSigned[nm] = elemSigned;
            return;
        }
        // Decay [n x T]* -> T* (addrspace 3) and register like a buffer base so
        // tile[i] reuses lowerLValueAddr's GEP/load/store path unchanged.
        llvm::Value* zero =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
        llvm::Value* base = builder.CreateGEP(arrTy, gv, {zero, zero},
                                              nm + ".base");
        bufferBases[nm] = base;
        bufferElems[nm] = elemTy;
        bufferElemSigned[nm] = elemSigned;
    }

    // Coerce any scalar to an i1 truth value (for conditions).
    llvm::Value* toI1(llvm::Value* v) {
        if (v->getType()->isIntegerTy(1)) return v;
        if (v->getType()->isFloatingPointTy())
            return builder.CreateFCmpONE(
                v, llvm::ConstantFP::get(v->getType(), 0.0));
        return builder.CreateICmpNE(
            v, llvm::ConstantInt::get(v->getType(), 0));
    }

    void lowerIf(const std::shared_ptr<IfStatement>& ifs) {
        llvm::Value* cond = toI1(lowerExpr(ifs->getCondition()));
        auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", fn);
        auto* endBB  = llvm::BasicBlock::Create(ctx, "if.end", fn);
        llvm::BasicBlock* elseBB =
            ifs->getElseBranch() ? llvm::BasicBlock::Create(ctx, "if.else", fn)
                                 : endBB;
        builder.CreateCondBr(cond, thenBB, elseBB);

        builder.SetInsertPoint(thenBB);
        lowerStatement(ifs->getThenBranch());
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(endBB);

        if (ifs->getElseBranch()) {
            builder.SetInsertPoint(elseBB);
            lowerStatement(ifs->getElseBranch());
            if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(endBB);
        }
        builder.SetInsertPoint(endBB);
    }

    // for (init; cond; update) body — BB shape mirrors the host
    // (Statement.cpp): head(cond) / body / update / exit; continue→update.
    void lowerFor(const std::shared_ptr<ForStatement>& fs) {
        if (fs->getInit()) lowerStatement(fs->getInit());
        auto* head = llvm::BasicBlock::Create(ctx, "for.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "for.body", fn);
        auto* upd  = llvm::BasicBlock::Create(ctx, "for.update", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "for.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        llvm::Value* cond = fs->getCondition()
            ? toI1(lowerExpr(fs->getCondition()))
            : llvm::ConstantInt::getTrue(ctx);   // null cond ⇒ always-true
        builder.CreateCondBr(cond, body, exit);
        builder.SetInsertPoint(body);
        loopTargets.push_back({upd, exit});
        lowerStatement(fs->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(upd);
        builder.SetInsertPoint(upd);
        // Update exprs are statement-like (e.g. `j += stride`, `i++`): route
        // through lowerExprStatement so assignments hit lowerAssign.
        for (auto& u : fs->getUpdate()) lowerExprStatement(u);
        builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // Grid-stride for-each (Item 6):
    //   for (idxType idx, elemType elem : buf.range(count)) body
    // ⇒ for (idx = globalId.x; idx < count; idx += gridSize.x) {
    //        elem = buf[idx]; body
    //    }
    // The iterable MUST be `<bufferParam>.range(<count>)` — device buffers carry
    // no length, so the count is explicit (as in every GPU language). The element
    // binding is a value copy of buf[idx] (range-for semantics); writes go via the
    // index binding (`buf[idx] = …`). The iterator (index) binding is optional;
    // without it the body can read `elem` but has no index to write by.
    void lowerEnhancedFor(const std::shared_ptr<EnhancedForStatement>& efs) {
        auto mc = std::dynamic_pointer_cast<MethodCallExpression>(
            efs->getIterableExpr());
        std::string bufName;
        if (mc && mc->getMethodCallName() == "range" &&
            !mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0]))
                bufName = id->getTextValue();
        }
        if (bufName.empty()) {
            throw cajeta::Exception(
                "XPU kernel lowering: a for-each in a kernel must iterate "
                "`buffer.range(count)` (device buffers are unsized)", "XPU-N02");
        }
        auto bv = bufferBases.find(bufName);
        auto be = bufferElems.find(bufName);
        if (bv == bufferBases.end() || be == bufferElems.end()) {
            throw cajeta::Exception(
                "XPU kernel lowering: for-each receiver '" + bufName +
                "' is not a kernel buffer parameter", "XPU-N02");
        }
        if (mc->getParameters().size() != 1) {
            throw cajeta::Exception(
                "XPU kernel lowering: buffer.range(count) takes exactly one "
                "argument", "XPU-N02");
        }

        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);

        // Index type follows the iterator binding (default i32 / uint).
        llvm::Type* idxTy =
            efs->getIteratorType() ? deviceScalarType(efs->getIteratorType(), ctx)
                                   : i32;
        if (!idxTy || !idxTy->isIntegerTy()) idxTy = i32;
        bool idxSigned =
            efs->getIteratorType() && typeIsSigned(efs->getIteratorType());

        llvm::Value* count =
            coerceTo(lowerExpr(mc->getParameters()[0].expression), idxTy);
        llvm::Value* stride = coerceTo(target.gridSize(builder, mod, 0), idxTy);

        // Index slot, initialized to the global x-id. Bound to the iterator name
        // when present (so the body can `buf[idx] = …`).
        const bool hasIdx = efs->getIteratorType() != nullptr;
        std::string idxName = hasIdx ? efs->getIteratorName()
                                     : (bufName + ".fe.idx");
        llvm::Value* idxSlot = entryAlloca(idxTy, idxName);
        builder.CreateStore(coerceTo(target.globalId(builder, mod, 0), idxTy),
                            idxSlot);
        if (hasIdx) {
            values[idxName] = idxSlot;
            slotTypes[idxName] = idxTy;
            signedness[idxName] = idxSigned;
        }

        // Element binding: a per-iteration value copy of buf[idx].
        llvm::Type* elemTy = be->second;
        const std::string& elemName = efs->getElementName();
        llvm::Value* elemSlot = entryAlloca(elemTy, elemName);
        values[elemName] = elemSlot;
        slotTypes[elemName] = elemTy;
        auto sit = bufferElemSigned.find(bufName);
        signedness[elemName] = sit != bufferElemSigned.end() ? sit->second : true;

        auto* head = llvm::BasicBlock::Create(ctx, "fe.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "fe.body", fn);
        auto* upd  = llvm::BasicBlock::Create(ctx, "fe.update", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "fe.exit", fn);
        builder.CreateBr(head);

        builder.SetInsertPoint(head);
        llvm::Value* i = builder.CreateLoad(idxTy, idxSlot, idxName);
        // Thread indices are non-negative; an unsigned compare is correct and
        // lets a huge `count` (near INT_MAX) work.
        builder.CreateCondBr(builder.CreateICmpULT(i, count, "fe.cmp"),
                             body, exit);

        builder.SetInsertPoint(body);
        // elem = buf[idx]  (widen idx to i64 for the element GEP, like array idx).
        llvm::Value* i64idx =
            builder.CreateIntCast(i, i64, /*isSigned=*/idxSigned);
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, elemTy, i64idx);
        builder.CreateStore(builder.CreateLoad(elemTy, addr, "fe.elem"),
                            elemSlot);
        loopTargets.push_back({upd, exit});
        lowerStatement(efs->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(upd);

        builder.SetInsertPoint(upd);
        llvm::Value* next = builder.CreateAdd(
            builder.CreateLoad(idxTy, idxSlot, idxName), stride, "fe.next");
        builder.CreateStore(next, idxSlot);
        builder.CreateBr(head);

        builder.SetInsertPoint(exit);
    }

    // while (cond) body — head(cond) / body / exit; continue→head.
    void lowerWhile(const std::shared_ptr<WhileStatement>& ws) {
        auto* head = llvm::BasicBlock::Create(ctx, "while.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "while.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "while.exit", fn);
        builder.CreateBr(head);
        builder.SetInsertPoint(head);
        builder.CreateCondBr(toI1(lowerExpr(ws->getCondition())), body, exit);
        builder.SetInsertPoint(body);
        loopTargets.push_back({head, exit});
        lowerStatement(ws->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(head);
        builder.SetInsertPoint(exit);
    }

    // do body while (cond) — body / tail(cond) / exit; continue→tail.
    void lowerDo(const std::shared_ptr<DoStatement>& ds) {
        auto* body = llvm::BasicBlock::Create(ctx, "do.body", fn);
        auto* tail = llvm::BasicBlock::Create(ctx, "do.tail", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "do.exit", fn);
        builder.CreateBr(body);
        builder.SetInsertPoint(body);
        loopTargets.push_back({tail, exit});
        lowerStatement(ds->getBody());
        loopTargets.pop_back();
        if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(tail);
        builder.SetInsertPoint(tail);
        builder.CreateCondBr(toI1(lowerExpr(ds->getCondition())), body, exit);
        builder.SetInsertPoint(exit);
    }

    void lowerExprStatement(const ExpressionPtr& expr) {
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            if (bin->isAssignment()) { lowerAssign(bin); return; }
        }
        // Bare expression (builtin call, `i++` for side effects, …) — lower
        // and discard the value.
        lowerExpr(expr);
    }

    // `lhs = rhs` (plain) and `lhs op= rhs` (compound: load-op-store). Works on
    // both scalar locals and buffer elements via lowerLValueAddr.
    void lowerAssign(const std::shared_ptr<BinaryOpExpression>& bin) {
        ExpressionPtr lhs = exprChild(bin, 0);
        ExpressionPtr rhs = exprChild(bin, 1);
        auto [addr, elemTy] = lowerLValueAddr(lhs);
        llvm::Value* rv = lowerExpr(rhs);
        BinaryOp op = bin->getBinaryOp();
        if (op != BINARY_OP_ASSIGN) {
            llvm::Value* cur = builder.CreateLoad(elemTy, addr, "cur");
            rv = applyBinOp(compoundBase(op), cur, rv, lvalueSigned(lhs),
                            elemTy->isFloatingPointTy());
        }
        builder.CreateStore(coerceTo(rv, elemTy), addr);
    }

    static BinaryOp compoundBase(BinaryOp op) {
        switch (op) {
            case BINARY_OP_ADD_EQUALS: return BINARY_OP_ADD;
            case BINARY_OP_SUB_EQUALS: return BINARY_OP_SUB;
            case BINARY_OP_MUL_EQUALS: return BINARY_OP_MUL;
            case BINARY_OP_DIV_EQUALS: return BINARY_OP_DIV;
            case BINARY_OP_MOD_EQUALS: return BINARY_OP_MOD;
            case BINARY_OP_BITAND_EQUALS: return BINARY_OP_BITAND;
            case BINARY_OP_BITOR_EQUALS:  return BINARY_OP_BITOR;
            case BINARY_OP_BITXOR_EQUALS: return BINARY_OP_BITXOR;
            case BINARY_OP_SHIFTLEFT_EQUALS:   return BINARY_OP_SHIFTLEFT;
            case BINARY_OP_SHIFTRIGHT_EQUALS:  return BINARY_OP_SHIFTRIGHT;
            case BINARY_OP_USHIFTRIGHT_EQUALS: return BINARY_OP_USHIFTRIGHT;
            default: return BINARY_OP_ADD;  // unreachable (caller gated)
        }
    }

    bool lvalueSigned(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = signedness.find(id->getTextValue());
            return it != signedness.end() ? it->second : true;
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            if (auto b = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(ai, 0))) {
                auto it = bufferElemSigned.find(b->getTextValue());
                return it != bufferElemSigned.end() ? it->second : true;
            }
        }
        return true;
    }

    // ---- expressions ----------------------------------------------------

    llvm::Value* lowerExpr(const ExpressionPtr& expr) {
        if (!expr) unsupported("null expression");

        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(expr)) {
            const std::string& nm = id->getTextValue();
            auto bb = bufferBases.find(nm);
            if (bb != bufferBases.end()) return bb->second;  // buffer base ptr
            auto it = values.find(nm);
            if (it == values.end()) unsupported("unbound identifier '" + nm + "'");
            return builder.CreateLoad(slotTypes[nm], it->second, nm);  // load slot
        }
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(expr)) {
            return llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(ctx),
                std::stoll(stripSuffix(il->getRawValue())), /*signed=*/true);
        }
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(expr)) {
            return llvm::ConstantFP::get(
                llvm::Type::getFloatTy(ctx),
                std::stod(stripSuffix(fl->getRawValue())));
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(expr)) {
            return lowerBuiltinCall(mc);
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(expr)) {
            auto [addr, elemTy] = lowerLValueAddr(ai);
            return builder.CreateLoad(elemTy, addr, "elem");
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            return lowerBinaryOp(bin);
        }
        if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(expr)) {
            return lowerPrefix(pre);
        }
        if (auto post = std::dynamic_pointer_cast<PostfixExpression>(expr)) {
            return lowerPostfix(post);
        }
        if (auto cast = std::dynamic_pointer_cast<CastExpression>(expr)) {
            llvm::Value* v = lowerExpr(exprChild(cast, 0));
            // Prefer the declared target type; resolvedType may be unset since
            // the device lowerer walks the kernel AST without running
            // resolveTypes (the host stub body is what gets resolved).
            CajetaTypePtr ct = cast->getResolvedType();
            if (!ct) ct = cast->getDestType();
            llvm::Type* dst = deviceScalarType(ct, ctx);
            if (!dst) unsupported("cast to non-scalar type");
            return castNumeric(v, dst, typeIsSigned(ct),
                               exprSigned(exprChild(cast, 0)));
        }
        unsupported("expression form in kernel body");
    }

    llvm::Value* lowerPrefix(const std::shared_ptr<PrefixExpression>& pre) {
        ExpressionPtr operand = exprChild(pre, 0);
        switch (pre->getOp()) {
            case PREFIX_OP_POSITIVE:
                return lowerExpr(operand);
            case PREFIX_OP_NEGATIVE: {
                llvm::Value* v = lowerExpr(operand);
                return v->getType()->isFloatingPointTy() ? builder.CreateFNeg(v)
                                                          : builder.CreateNeg(v);
            }
            case PREFIX_OP_BITNOT:
                return builder.CreateNot(lowerExpr(operand));
            case PREFIX_OP_LOGNOT:
                return builder.CreateXor(toI1(lowerExpr(operand)),
                                         llvm::ConstantInt::getTrue(ctx));
            case PREFIX_OP_INC:
            case PREFIX_OP_DEC:
                return lowerIncDec(operand, pre->getOp() == PREFIX_OP_INC,
                                   /*returnOld=*/false);
        }
        unsupported("prefix operator");
    }

    llvm::Value* lowerPostfix(const std::shared_ptr<PostfixExpression>& post) {
        return lowerIncDec(exprChild(post, 0),
                           post->getOp() == POSTFIX_OP_INC, /*returnOld=*/true);
    }

    // Shared ++/-- on an l-value: load, ±1, store. returnOld picks postfix
    // (old value) vs prefix (new value) semantics.
    llvm::Value* lowerIncDec(const ExpressionPtr& operand, bool inc,
                             bool returnOld) {
        auto [addr, ty] = lowerLValueAddr(operand);
        llvm::Value* cur = builder.CreateLoad(ty, addr, "cur");
        bool fp = ty->isFloatingPointTy();
        llvm::Value* one = fp ? (llvm::Value*) llvm::ConstantFP::get(ty, 1.0)
                              : (llvm::Value*) llvm::ConstantInt::get(ty, 1);
        llvm::Value* nv = applyBinOp(inc ? BINARY_OP_ADD : BINARY_OP_SUB,
                                     cur, one, lvalueSigned(operand), fp);
        builder.CreateStore(nv, addr);
        return returnOld ? cur : nv;
    }

    llvm::Value* castNumeric(llvm::Value* v, llvm::Type* dst, bool dstSigned,
                             bool srcSigned) {
        llvm::Type* src = v->getType();
        if (src == dst) return v;
        bool sFp = src->isFloatingPointTy(), dFp = dst->isFloatingPointTy();
        if (sFp && dFp)   return builder.CreateFPCast(v, dst);
        if (!sFp && !dFp) return builder.CreateIntCast(v, dst, srcSigned);
        if (!sFp && dFp)  return srcSigned ? builder.CreateSIToFP(v, dst)
                                           : builder.CreateUIToFP(v, dst);
        return dstSigned ? builder.CreateFPToSI(v, dst)   // fp -> int
                         : builder.CreateFPToUI(v, dst);
    }

    // Address (and element type) of an l-value. Only buffer/array indexing
    // is supported as an assignment target in the slice.
    std::pair<llvm::Value*, llvm::Type*> lowerLValueAddr(const ExpressionPtr& e) {
        // Scalar local / param: assign straight to its alloca slot.
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            const std::string& nm = id->getTextValue();
            auto it = values.find(nm);
            if (it == values.end())
                unsupported("assignment to unbound local '" + nm + "'");
            return {it->second, slotTypes[nm]};
        }
        auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
        if (!ai) unsupported("l-value that isn't a buffer index or local");
        auto baseId = std::dynamic_pointer_cast<IdentifierExpression>(
            exprChild(ai, 0));
        if (!baseId) unsupported("buffer index on a non-identifier base");
        // A typed dynamic shared array (Vulkan): index it as an array
        // (gep arrTy, gv, {0, i}) so the OpTypeArray survives in the SPIR-V.
        if (auto as = arrayShared.find(baseId->getTextValue());
            as != arrayShared.end()) {
            llvm::Value* idx = lowerExpr(exprChild(ai, 1));
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            if (idx->getType() != i64)
                idx = builder.CreateIntCast(idx, i64,
                                            exprSigned(exprChild(ai, 1)));
            llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
            llvm::Value* addr = builder.CreateGEP(
                as->second.second, as->second.first, {zero, idx}, "dynsh.idx");
            return {addr, bufferElems[baseId->getTextValue()]};
        }
        auto bv = bufferBases.find(baseId->getTextValue());
        auto be = bufferElems.find(baseId->getTextValue());
        if (bv == bufferBases.end() || be == bufferElems.end()) {
            unsupported("index on non-buffer '" + baseId->getTextValue() + "'");
        }
        ExpressionPtr idxExpr = exprChild(ai, 1);
        llvm::Value* idx = lowerExpr(idxExpr);
        // GEP wants an i64 index; widen by the index's signedness.
        if (idx->getType() != llvm::Type::getInt64Ty(ctx)) {
            idx = builder.CreateIntCast(idx, llvm::Type::getInt64Ty(ctx),
                                        exprSigned(idxExpr));
        }
        // Element pointer is a backend decision: NVPTX/AMDGPU GEP the base
        // pointer; Vulkan routes descriptor-buffer handles through getpointer
        // (shared-mem globals still GEP). See LoweringTarget::bufferElementPtr.
        llvm::Value* addr =
            target.bufferElementPtr(builder, mod, bv->second, be->second, idx);
        return {addr, be->second};
    }

    // Device builtins (Thread / Workgroup coordinates, Barrier). The mapping
    // from builtin name to which coordinate is read is SHARED; only the leaf
    // intrinsic emission is per-backend (LoweringTarget) — that split is the
    // measured seam (cajeta-amd.md §2).
    llvm::Value* lowerBuiltinCall(const std::shared_ptr<MethodCallExpression>& mc) {
        std::string recv;
        if (!mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0])) {
                recv = id->getTextValue();
            }
        }
        const std::string& name = mc->getMethodCallName();

        if (recv == "Thread") {
            if (name == "x") return target.threadId(builder, mod, 0);
            if (name == "y") return target.threadId(builder, mod, 1);
            if (name == "z") return target.threadId(builder, mod, 2);
            if (name == "globalIdX") return target.globalId(builder, mod, 0);
            if (name == "globalIdY") return target.globalId(builder, mod, 1);
            if (name == "globalIdZ") return target.globalId(builder, mod, 2);
        } else if (recv == "Workgroup") {
            if (name == "x") return target.workgroupId(builder, mod, 0);
            if (name == "y") return target.workgroupId(builder, mod, 1);
            if (name == "z") return target.workgroupId(builder, mod, 2);
            if (name == "dimX") return target.workgroupDim(builder, mod, 0);
            if (name == "dimY") return target.workgroupDim(builder, mod, 1);
            if (name == "dimZ") return target.workgroupDim(builder, mod, 2);
        } else if (recv == "Barrier") {
            if (name == "workgroup") {
                target.workgroupBarrier(builder, mod);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        } else if (recv == "Wave") {
            const auto& args = mc->getParameters();
            if (name == "width") return target.waveWidth(builder, mod);
            if (name == "laneId") return target.waveLaneId(builder, mod);
            if (name == "isFirstLane") {
                // A width-agnostic cooperation helper, built on laneId(): the
                // "one lane commits the wave's result" guard. Lowered here
                // (not a seam point) so every backend gets it for free.
                llvm::Value* lane = target.waveLaneId(builder, mod);
                return builder.CreateICmpEQ(
                    lane, llvm::ConstantInt::get(lane->getType(), 0),
                    "wave.isfirst");
            }
            if (name == "shuffleSync") {
                if (args.size() != 2) unsupported("Wave.shuffleSync arity");
                llvm::Value* value = lowerExpr(args[0].expression);
                llvm::Value* srcLane = lowerExpr(args[1].expression);
                return target.waveShuffle(builder, mod, value, srcLane);
            }
            if (name == "ballotSync") {
                if (args.size() != 1) unsupported("Wave.ballotSync arity");
                llvm::Value* pred = toI1(lowerExpr(args[0].expression));
                return target.waveBallot(builder, mod, pred);
            }
            if (name == "reduceSum") {
                if (args.size() != 1) unsupported("Wave.reduceSum arity");
                return target.waveReduceSum(builder, mod,
                                            lowerExpr(args[0].expression));
            }
        }
        // A user-defined @Device helper call (resolved within the kernel's
        // class). Lower the helper to a device function (cached) and call it.
        if (auto m = resolveDeviceMethod(recv, name, mc)) {
            llvm::Function* hfn = lowerDeviceFn(m);
            const auto& args = mc->getParameters();
            std::vector<llvm::Value*> argv;
            argv.reserve(args.size());
            for (unsigned i = 0; i < args.size(); ++i)
                argv.push_back(coerceTo(lowerExpr(args[i].expression),
                                        hfn->getArg(i)->getType()));
            return builder.CreateCall(hfn, argv, hfn->getReturnType()->isVoidTy()
                                                     ? "" : "dev.call");
        }
        unsupported("device builtin '" + recv + "." + name + "()'");
    }

    // Resolve a call `name(args)` / `Cls.name(args)` to a sibling @Device method
    // of the kernel's class (first cut: same class only), matched by arity.
    MethodPtr resolveDeviceMethod(
            const std::string& recv, const std::string& name,
            const std::shared_ptr<MethodCallExpression>& mc) {
        if (!cls || !deviceFns) return nullptr;
        if (!recv.empty()) {
            // `Cls.helper(...)` — accept only the kernel's own (simple) class
            // name (the last segment of the qualified name).
            std::string q = cls->toCanonical();
            std::string simple = q.substr(q.find_last_of('.') + 1);
            if (recv != simple) return nullptr;
        }
        unsigned argc = (unsigned) mc->getParameters().size();
        for (auto& kv : cls->getMethods()) {
            const MethodPtr& m = kv.second;
            if (!m || m->getName() != name || !isDevice(*m)) continue;
            if (m->getParameters().size() != argc) continue;
            return m;
        }
        return nullptr;
    }

    // Lower a @Device method to a device function (scalar params + scalar/void
    // return), cached. alwaysinline so every backend folds the call away. A
    // nullptr cache entry means it's mid-lowering → a recursive call (rejected).
    llvm::Function* lowerDeviceFn(const MethodPtr& m) {
        auto it = deviceFns->find(m.get());
        if (it != deviceFns->end()) {
            if (!it->second) unsupported("recursive @Device call");
            return it->second;
        }
        (*deviceFns)[m.get()] = nullptr;            // mark in-progress

        std::vector<LoweringTarget::KernelParam> params = collectParams(m, ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size());
        for (auto& p : params) {
            if (p.isBuffer)
                unsupported("@Device helper with a Buffer/array param "
                            "(scalar params only for now)");
            tys.push_back(p.type);
        }
        llvm::Type* retTy = m->getReturnType()
                                ? deviceScalarType(m->getReturnType(), ctx)
                                : nullptr;
        if (!retTy) retTy = llvm::Type::getVoidTy(ctx);
        auto* fnTy = llvm::FunctionType::get(retTy, tys, /*vararg=*/false);
        std::string fname = "__cajeta_xpu_dev." +
            (cls ? cls->toCanonical() + "." : std::string()) + m->getName();
        auto* hfn = llvm::Function::Create(
            fnTy, llvm::GlobalValue::InternalLinkage, fname, &mod);
        hfn->addFnAttr(llvm::Attribute::AlwaysInline);
        unsigned i = 0;
        for (auto& p : params) hfn->getArg(i++)->setName(p.name);

        DeviceLowerer sub(mod, hfn, target);
        sub.setParams(params);
        sub.setDeviceContext(cls, deviceFns);
        sub.lowerBody(m);

        (*deviceFns)[m.get()] = hfn;
        return hfn;
    }

    llvm::Value* lowerBinaryOp(const std::shared_ptr<BinaryOpExpression>& bin) {
        BinaryOp op = bin->getBinaryOp();
        // && / || evaluate lazily — control flow, not eager operands.
        if (op == BINARY_OP_LOGAND || op == BINARY_OP_LOGOR)
            return lowerLogical(bin);
        ExpressionPtr le = exprChild(bin, 0), re = exprChild(bin, 1);
        llvm::Value* l = lowerExpr(le);
        llvm::Value* r = lowerExpr(re);
        // The LHS drives the operation's signedness — matches the language's
        // "lhs type is the result type" rule (BinaryOpExpression::resolveTypes)
        // and keeps an unsigned `i >> 2` a logical shift despite the literal.
        bool sign = exprSigned(le);
        return applyBinOp(op, l, r, sign,
                          l->getType()->isFloatingPointTy() ||
                          r->getType()->isFloatingPointTy());
    }

    // Short-circuit lhs && rhs / lhs || rhs via branch + phi.
    llvm::Value* lowerLogical(const std::shared_ptr<BinaryOpExpression>& bin) {
        bool isAnd = bin->getBinaryOp() == BINARY_OP_LOGAND;
        llvm::Value* l = toI1(lowerExpr(exprChild(bin, 0)));
        llvm::BasicBlock* lhsEnd = builder.GetInsertBlock();  // snapshot
        auto* rhsBB  = llvm::BasicBlock::Create(ctx, "logic.rhs", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "logic.done", fn);
        if (isAnd) builder.CreateCondBr(l, rhsBB, doneBB);   // AND: false short-circuits
        else       builder.CreateCondBr(l, doneBB, rhsBB);   // OR:  true short-circuits
        builder.SetInsertPoint(rhsBB);
        llvm::Value* r = toI1(lowerExpr(exprChild(bin, 1)));
        llvm::BasicBlock* rhsEnd = builder.GetInsertBlock();
        builder.CreateBr(doneBB);
        builder.SetInsertPoint(doneBB);
        llvm::PHINode* phi = builder.CreatePHI(llvm::Type::getInt1Ty(ctx), 2);
        phi->addIncoming(llvm::ConstantInt::getBool(ctx, !isAnd), lhsEnd);
        phi->addIncoming(r, rhsEnd);
        return phi;
    }

    // Core binary op on two lowered values, after width/fp unification.
    llvm::Value* applyBinOp(BinaryOp op, llvm::Value* l, llvm::Value* r,
                            bool sign, bool /*fpHint*/) {
        unifyOperands(l, r, sign);
        bool fp = l->getType()->isFloatingPointTy();
        switch (op) {
            case BINARY_OP_ADD: return fp ? builder.CreateFAdd(l, r) : builder.CreateAdd(l, r);
            case BINARY_OP_SUB: return fp ? builder.CreateFSub(l, r) : builder.CreateSub(l, r);
            case BINARY_OP_MUL: return fp ? builder.CreateFMul(l, r) : builder.CreateMul(l, r);
            case BINARY_OP_DIV: return fp ? builder.CreateFDiv(l, r)
                                          : (sign ? builder.CreateSDiv(l, r) : builder.CreateUDiv(l, r));
            case BINARY_OP_MOD: return fp ? builder.CreateFRem(l, r)
                                          : (sign ? builder.CreateSRem(l, r) : builder.CreateURem(l, r));
            case BINARY_OP_BITAND: return builder.CreateAnd(l, r);
            case BINARY_OP_BITOR:  return builder.CreateOr(l, r);
            case BINARY_OP_BITXOR: return builder.CreateXor(l, r);
            case BINARY_OP_SHIFTLEFT:   return builder.CreateShl(l, r);
            case BINARY_OP_SHIFTRIGHT:  return sign ? builder.CreateAShr(l, r) : builder.CreateLShr(l, r);
            case BINARY_OP_USHIFTRIGHT: return builder.CreateLShr(l, r);
            case BINARY_OP_LT: return fp ? builder.CreateFCmpOLT(l, r) : (sign ? builder.CreateICmpSLT(l, r) : builder.CreateICmpULT(l, r));
            case BINARY_OP_LE: return fp ? builder.CreateFCmpOLE(l, r) : (sign ? builder.CreateICmpSLE(l, r) : builder.CreateICmpULE(l, r));
            case BINARY_OP_GT: return fp ? builder.CreateFCmpOGT(l, r) : (sign ? builder.CreateICmpSGT(l, r) : builder.CreateICmpUGT(l, r));
            case BINARY_OP_GE: return fp ? builder.CreateFCmpOGE(l, r) : (sign ? builder.CreateICmpSGE(l, r) : builder.CreateICmpUGE(l, r));
            case BINARY_OP_EQ: return fp ? builder.CreateFCmpOEQ(l, r) : builder.CreateICmpEQ(l, r);
            case BINARY_OP_NE: return fp ? builder.CreateFCmpONE(l, r) : builder.CreateICmpNE(l, r);
            default: unsupported("binary operator in kernel body");
        }
    }

    // Bring two scalars to a common type: fp dominates int (int→fp); among
    // ints, extend the narrower to the wider (sext if signed, else zext).
    void unifyOperands(llvm::Value*& l, llvm::Value*& r, bool sign) {
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return;
        bool lFp = lt->isFloatingPointTy(), rFp = rt->isFloatingPointTy();
        if (lFp || rFp) {
            llvm::Type* fT = lFp ? lt : rt;
            if (!lFp)        l = sign ? builder.CreateSIToFP(l, fT) : builder.CreateUIToFP(l, fT);
            else if (lt != fT) l = builder.CreateFPCast(l, fT);
            if (!rFp)        r = sign ? builder.CreateSIToFP(r, fT) : builder.CreateUIToFP(r, fT);
            else if (rt != fT) r = builder.CreateFPCast(r, fT);
            return;
        }
        llvm::Type* wide = lt->getIntegerBitWidth() >= rt->getIntegerBitWidth() ? lt : rt;
        l = builder.CreateIntCast(l, wide, sign);
        r = builder.CreateIntCast(r, wide, sign);
    }

    // ---- helpers --------------------------------------------------------

    ExpressionPtr exprChild(const std::shared_ptr<Expression>& e, size_t i) {
        if (e->getChildren().size() <= i) unsupported("missing operand");
        return std::dynamic_pointer_cast<Expression>(e->getChildren()[i]);
    }

    bool exprSigned(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            auto it = signedness.find(id->getTextValue());
            return it != signedness.end() ? it->second : true;
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            if (auto b = std::dynamic_pointer_cast<IdentifierExpression>(
                    exprChild(ai, 0))) {
                auto it = bufferElemSigned.find(b->getTextValue());
                if (it != bufferElemSigned.end()) return it->second;
            }
            return true;
        }
        if (std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) return true;
        return false;
    }

    llvm::Value* coerceTo(llvm::Value* v, llvm::Type* ty) {
        if (v->getType() == ty) return v;
        if (v->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
            return builder.CreateFPCast(v, ty);
        if (v->getType()->isIntegerTy() && ty->isIntegerTy())
            return builder.CreateIntCast(v, ty, /*signed=*/true);
        return v;  // best effort; shape mismatches surface in the verifier
    }

    static std::string stripSuffix(const std::string& raw) {
        std::string s = raw;
        while (!s.empty() && (s.back() == 'f' || s.back() == 'F' ||
                              s.back() == 'd' || s.back() == 'D' ||
                              s.back() == 'l' || s.back() == 'L' ||
                              s.back() == 'u' || s.back() == 'U')) {
            s.pop_back();
        }
        return s;
    }
};

} // namespace

// ---- LoweringTarget default hooks (NVPTX/AMDGPU pointer-arg model) --------
//
// These reproduce the pre-Vulkan signature/parameter behavior, so NVPTX and
// AMDGPU inherit them unchanged. Vulkan overrides all three (SpirvTarget).

llvm::Function* LoweringTarget::createKernel(
    llvm::Module& m, const std::string& name,
    const std::vector<KernelParam>& params) {
    llvm::LLVMContext& ctx = m.getContext();
    // Buffer<T> / arrays -> addrspace(1) pointers; primitives by value.
    std::vector<llvm::Type*> tys;
    tys.reserve(params.size());
    for (auto& p : params) {
        tys.push_back(p.isBuffer
                          ? (llvm::Type*) llvm::PointerType::get(ctx, kGlobalAS)
                          : p.type);
    }
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), tys,
                                         /*vararg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      name, &m);
    unsigned i = 0;
    for (auto& p : params) fn->getArg(i++)->setName(p.name);
    decorateKernel(fn, m);  // calling convention + any kernel-marker metadata
    return fn;
}

llvm::Value* LoweringTarget::materializeParam(llvm::IRBuilderBase& /*b*/,
                                              llvm::Module& /*m*/,
                                              llvm::Function* fn, unsigned idx,
                                              const KernelParam& /*p*/) {
    return fn->getArg(idx);  // the value IS the argument
}

llvm::Value* LoweringTarget::bufferElementPtr(llvm::IRBuilderBase& b,
                                              llvm::Module& /*m*/,
                                              llvm::Value* base,
                                              llvm::Type* elemTy,
                                              llvm::Value* index) {
    // addrspace-preserving GEP — the base pointer carries its address space
    // (1 for global buffers, 3 for shared globals); correct on NVPTX/AMDGPU.
    return b.CreateGEP(elemTy, base, {index}, "idx");
}

// Admit the kernel parameters: Buffer<T>/arrays carry an element type,
// primitives a scalar type. This classification is backend-neutral; HOW the
// params become a signature is the backend's call (target.createKernel /
// materializeParam) — the Vulkan fork.
static std::vector<LoweringTarget::KernelParam> collectParams(
        const MethodPtr& method, llvm::LLVMContext& ctx) {
    std::vector<LoweringTarget::KernelParam> params;
    for (auto& p : method->getParameterList()) {
        if (!p) continue;
        if (p->getName() == "this") continue;
        CajetaTypePtr t = p->getType();
        if (isBufferType(t)) {
            llvm::Type* elem = nullptr;
            bool elemSigned = true;
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                if (!cls->getTypeArguments().empty()) {
                    elem = deviceScalarType(cls->getTypeArguments()[0], ctx);
                    elemSigned = typeIsSigned(cls->getTypeArguments()[0]);
                }
            }
            if (!elem) elem = llvm::Type::getFloatTy(ctx);  // default element
            params.push_back({p->getName(), /*isBuffer=*/true, elem, elemSigned});
        } else {
            llvm::Type* st = deviceScalarType(t, ctx);
            if (!st) unsupported("kernel parameter type '" +
                                 (t ? t->toCanonical() : std::string("?")) + "'");
            params.push_back({p->getName(), /*isBuffer=*/false, st,
                              typeIsSigned(t)});
        }
    }
    return params;
}

std::vector<KernelParamInfo> collectKernelParamInfo(const MethodPtr& method,
                                                    llvm::LLVMContext& ctx) {
    std::vector<KernelParamInfo> info;
    if (!method) return info;
    for (auto& p : collectParams(method, ctx)) {
        unsigned bytes = 0;
        if (!p.isBuffer && p.type)
            bytes = (p.type->getScalarSizeInBits() + 7u) / 8u;
        info.push_back({p.isBuffer, bytes});
    }
    return info;
}

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            LoweringTarget& target) {
    if (!method) unsupported("null kernel method");
    llvm::LLVMContext& ctx = deviceModule.getContext();

    std::vector<LoweringTarget::KernelParam> params = collectParams(method, ctx);

    llvm::Function* fn =
        target.createKernel(deviceModule, method->getName(), params);

    DeviceLowerer lowerer(deviceModule, fn, target);
    lowerer.setParams(std::move(params));
    // A per-kernel cache of lowered @Device helper functions (shared with any
    // nested helper lowering), so each helper is emitted once and recursion is
    // caught. The functions live in the module; the cache is just for dedup.
    DeviceLowerer::DeviceFnCache deviceFns;
    lowerer.setDeviceContext(method->getParent(), &deviceFns);
    lowerer.lowerBody(method);
    return fn;
}

} // namespace xpu
} // namespace cajeta
