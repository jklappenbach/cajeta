//
// NVPTX kernel lowering — see header.
//

#include "NvptxKernelLowering.h"

#include "../../method/Method.h"
#include "../../type/FormalParameter.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaClass.h"
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
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

[[noreturn]] void unsupported(const std::string& what) {
    throw cajeta::Exception(
        "NVPTX kernel lowering: unsupported construct — " + what,
        "XPU-N01");
}

// addrspace(1) == CUDA global memory, addrspace(3) == CUDA shared / workgroup
// memory (AddressSpace.h nvidiaNumberFor: Global->1, Shared->3).
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
    DeviceLowerer(llvm::Module& m, llvm::Function* fn)
        : mod(m), ctx(m.getContext()), builder(ctx), fn(fn) {}

    void lowerBody(const MethodPtr& method) {
        builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        // Materialize params now that the entry block exists. Scalars get a
        // mutable alloca slot (so they can be reassigned / serve as loop
        // counters); buffers keep their direct addrspace(1) base pointer.
        for (auto& pb : paramBindings) {
            if (pb.bufferElem) {
                bufferBases[pb.name] = pb.arg;
                bufferElems[pb.name] = pb.bufferElem;
                bufferElemSigned[pb.name] = pb.elemSigned;
            } else {
                llvm::Type* t = pb.arg->getType();
                llvm::Value* slot = entryAlloca(t, pb.name);
                builder.CreateStore(pb.arg, slot);
                values[pb.name] = slot;
                slotTypes[pb.name] = t;
                signedness[pb.name] = pb.isSigned;
            }
        }
        lowerStatement(method->getBlock());
        // Kernels return void; close any open block.
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateRetVoid();
        }
    }

    // Record a parameter binding; materialized into the entry block by
    // lowerBody (allocas need the entry block to exist first).
    void bindParam(const std::string& name, llvm::Value* v,
                   bool isSigned, llvm::Type* bufferElem, bool elemSigned) {
        paramBindings.push_back({name, v, isSigned, bufferElem, elemSigned});
    }

private:
    llvm::Module& mod;
    llvm::LLVMContext& ctx;
    llvm::IRBuilder<> builder;
    llvm::Function* fn;
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

    struct ParamBinding {
        std::string name; llvm::Value* arg; bool isSigned; llvm::Type* bufferElem;
        bool elemSigned;
    };
    std::vector<ParamBinding> paramBindings;
    std::map<std::string, bool> bufferElemSigned;  // buffer name -> elem signed?
    // At most one dynamic (runtime-sized) shared array per kernel — CUDA's
    // extern __shared__ is a single region; multiple would alias the same base.
    bool emittedDynamicShared = false;

    // continue/break targets for the innermost enclosing loop.
    struct LoopTarget { llvm::BasicBlock* continueBB; llvm::BasicBlock* breakBB; };
    std::vector<LoopTarget> loopTargets;

    // Allocate a slot in the function entry block (so it dominates every use
    // regardless of which loop/branch block is current). Mirrors the host's
    // entry-positioned-IRBuilder idiom (LocalVariableDeclaration.cpp).
    llvm::AllocaInst* entryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::BasicBlock& entry = fn->getEntryBlock();
        llvm::IRBuilder<> eb(&entry, entry.begin());
        return eb.CreateAlloca(ty, nullptr, name + ".slot");
    }

    llvm::Value* readSreg(llvm::Intrinsic::ID id) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&mod, id);
        return builder.CreateCall(f, {}, "sreg");
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
        if (std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            unsupported("for-each loop in kernel body (next increment)");
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
        if (std::dynamic_pointer_cast<ReturnStatement>(node)) {
            // @Kernel returns void; a bare `return;` just closes the block.
            builder.CreateRetVoid();
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
    // memory in CUDA is reserved per block (every thread sees the same region),
    // NOT a per-thread alloca, so it lowers to ONE module-level addrspace(3)
    // global; we then register its decayed element pointer in the buffer maps,
    // after which indexing (tile[i]), assignment, and compound-assignment all
    // reuse the existing addrspace-agnostic buffer path (LLVM tracks the
    // address space on the pointer). Two flavors, by whether `size` folds:
    //   - constant size N -> STATIC: an internal [N x T] global (per-block,
    //     reserved by ptxas; .shared in PTX).
    //   - runtime size     -> DYNAMIC: an external, unsized [0 x T] global
    //     (.extern .shared). The byte count comes from the launch config
    //     (cuLaunchKernel sharedMemBytes); CUDA allows one such region/kernel.
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
            n = 0;                                      // unsized
            linkage = llvm::GlobalValue::ExternalLinkage;
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(elemTy, n);
        init = (linkage == llvm::GlobalValue::ExternalLinkage)
                   ? nullptr                            // external: no initializer
                   : (llvm::Constant*) llvm::UndefValue::get(arrTy);
        auto* gv = new llvm::GlobalVariable(
            mod, arrTy, /*isConstant=*/false, linkage, init,
            // '_' not '.': the device global's name becomes a PTX symbol, and
            // '.' is PTX's directive separator (the AsmPrinter would mangle it).
            fn->getName().str() + "_" + nm, /*InsertBefore=*/nullptr,
            llvm::GlobalValue::NotThreadLocal, kSharedAS);
        gv->setAlignment(llvm::MaybeAlign(16));

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
            llvm::Type* dst = deviceScalarType(cast->getResolvedType(), ctx);
            if (!dst) unsupported("cast to non-scalar type");
            return castNumeric(v, dst, typeIsSigned(cast->getResolvedType()),
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
        llvm::Value* addr = builder.CreateGEP(be->second, bv->second, {idx}, "idx");
        return {addr, be->second};
    }

    llvm::Value* lowerBuiltinCall(const std::shared_ptr<MethodCallExpression>& mc) {
        std::string recv;
        if (!mc->getChildren().empty()) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                    mc->getChildren()[0])) {
                recv = id->getTextValue();
            }
        }
        const std::string& name = mc->getMethodCallName();
        using I = llvm::Intrinsic::ID;

        auto tid = [&](I id) { return readSreg(id); };
        auto global = [&](I ctaid, I ntid, I t) {
            // ctaid * ntid + tid
            llvm::Value* m = builder.CreateMul(readSreg(ctaid), readSreg(ntid));
            return builder.CreateAdd(m, readSreg(t));
        };

        if (recv == "Thread") {
            if (name == "x") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
            if (name == "y") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_y);
            if (name == "z") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_z);
            if (name == "globalIdX")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
            if (name == "globalIdY")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_y,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_y,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_y);
            if (name == "globalIdZ")
                return global(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_z,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_z,
                              llvm::Intrinsic::nvvm_read_ptx_sreg_tid_z);
        } else if (recv == "Workgroup") {
            if (name == "x") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x);
            if (name == "y") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_y);
            if (name == "z") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_z);
            if (name == "dimX") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x);
            if (name == "dimY") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_y);
            if (name == "dimZ") return tid(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_z);
        } else if (recv == "Barrier") {
            if (name == "workgroup") {
                // bar.sync 0 — synchronize all threads in the CTA.
                llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
                    &mod, llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
                builder.CreateCall(f, {llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), 0)});
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            }
        }
        unsupported("device builtin '" + recv + "." + name + "()'");
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

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    if (!method) unsupported("null kernel method");
    llvm::LLVMContext& ctx = deviceModule.getContext();

    // Build the device function signature. Buffer<T> / arrays become
    // addrspace(1) pointers; primitives pass by value.
    std::vector<llvm::Type*> paramTys;
    struct PInfo {
        std::string name; bool isSigned; llvm::Type* bufferElem; bool elemSigned;
    };
    std::vector<PInfo> infos;
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
            paramTys.push_back(llvm::PointerType::get(ctx, kGlobalAS));
            infos.push_back({p->getName(), false, elem, elemSigned});
        } else {
            llvm::Type* st = deviceScalarType(t, ctx);
            if (!st) unsupported("kernel parameter type '" +
                                 (t ? t->toCanonical() : std::string("?")) + "'");
            paramTys.push_back(st);
            infos.push_back({p->getName(), typeIsSigned(t), nullptr, false});
        }
    }

    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), paramTys, /*vararg=*/false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, method->getName(), &deviceModule);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);

    // nvvm.annotations kernel marker (belt-and-suspenders alongside the CC).
    llvm::Metadata* ops[] = {
        llvm::ValueAsMetadata::get(fn),
        llvm::MDString::get(ctx, "kernel"),
        llvm::ValueAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)),
    };
    deviceModule.getOrInsertNamedMetadata("nvvm.annotations")
        ->addOperand(llvm::MDNode::get(ctx, ops));

    DeviceLowerer lowerer(deviceModule, fn);
    unsigned i = 0;
    for (auto& info : infos) {
        llvm::Argument* arg = fn->getArg(i++);
        arg->setName(info.name);
        lowerer.bindParam(info.name, arg, info.isSigned, info.bufferElem,
                          info.elemSigned);
    }
    lowerer.lowerBody(method);
    return fn;
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
