//
// Created by James Klappenbach on 4/8/23.
//

#include "../../error/Diagnostics.h"
#include "BinaryOpExpression.h"
#include "OperatorDispatch.h"
#include "../LocalVariableDeclaration.h"
#include "../../error/CajetaExceptions.h"
#include "../../error/DiagnosticEngine.h"
#include "../../error/Exception.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaVector.h"
#include "../../type/CajetaFunctionType.h"
#include "../../type/VectorOps.h"
#include "../../type/CajetaMatrix.h"
#include "../../type/CajetaQuaternion.h"
#include "../../type/QuaternionOps.h"
#include "../../type/MatrixOps.h"
#include "../../util/MemoryManager.h"
#include "cajeta/ownership/ReturnTitleAudit.h"
#include "cajeta/ownership/OwnedBindCheck.h"
#include "cajeta/ownership/TitleClassifier.h"
#include "Expression.h"
#include "DotExpression.h"
#include "MethodCallExpression.h"
#include "Identifier.h"
#include "../../field/ParameterField.h"
#include "../../type/FormalParameter.h"
#include "AggregateInitializerExpression.h"
#include "NewExpression.h"
#include "../../compile/ScriptUnitSynthesis.h"
#include "LiteralExpression.h"

#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Renders an assignment TARGET as source-like text for the ownership
    // diagnostics: `this.host`, `out[...]`, `x`. Falls back to `<target>` rather
    // than "", since the notes are grepped and a blank field merges sites.
    static string assignTargetText(const ExpressionPtr& target) {
        if (!target) return "<target>";
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(target)) {
            return id->getTextValue();
        }
        if (auto dot = dynamic_pointer_cast<DotExpression>(target)) {
            string path = DotExpression::buildPath(target);
            if (!path.empty()) return path;
            auto& kids = dot->getChildren();
            auto root = kids.empty()
                ? nullptr : dynamic_pointer_cast<Expression>(kids[0]);
            string recv = dynamic_pointer_cast<ThisExpression>(root)
                ? string("this") : string("…");
            return recv + "." + dot->getIdentifier();
        }
        if (auto aix = dynamic_pointer_cast<ArrayIndexExpression>(target)) {
            auto& kids = aix->getChildren();
            string recv = kids.empty()
                ? string()
                : assignTargetText(
                      dynamic_pointer_cast<Expression>(kids[0]));
            if (recv == "<target>") recv.clear();
            return recv + "[…]";
        }
        return "<target>";
    }

    // Emits `if (condTrap) { llvm.trap; unreachable; }` and leaves the builder in
    // the following ok block; `label` names both blocks. The caller computes the
    // trap predicate; only reached when CompilerFlags::ubTraps is on.
    void emitUbTrap(CajetaModulePtr module,
                           llvm::IRBuilder<>& b,
                           llvm::Value* condTrap,
                           const std::string& label) {
        if (!condTrap) return;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        // A per-lane `<N x i1>` trap condition cannot drive a branch: reduce it to a
        // scalar "any lane traps" i1 first, or the verifier rejects the `br`.
        if (condTrap->getType()->isVectorTy()) {
            condTrap = b.CreateOrReduce(condTrap);
        }
        llvm::Function* curFn = b.GetInsertBlock()->getParent();
        auto* trapBB = llvm::BasicBlock::Create(ctx, label + ".trap", curFn);
        auto* okBB = llvm::BasicBlock::Create(ctx, label + ".ok", curFn);
        b.CreateCondBr(condTrap, trapBB, okBB);
        b.SetInsertPoint(trapBB);
        // In a session the runtime unwinds the cell to its guard; it RETURNS when no
        // cell is guarded, so the trap below still fires outside one.
        if (module->getFlags().trapsUnwind) {
            llvm::FunctionCallee unwind = lmod->getOrInsertFunction(
                "__cajeta_session_trap_unwind",
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                        {llvm::PointerType::get(ctx, 0)},
                                        false));
            const std::string what =
                label == "div"  ? "divide by zero"
              : label == "mod"  ? "remainder by zero"
              : (label == "shl" || label == "shr" || label == "ushr")
                                ? "shift amount is at least the bit width"
                                : "arithmetic overflow in " + label;
            b.CreateCall(unwind, {b.CreateGlobalString(what, ".trapwhat")});
        }
        llvm::Function* trapFn = llvm::Intrinsic::getOrInsertDeclaration(
            lmod, llvm::Intrinsic::trap);
        b.CreateCall(trapFn);
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    // Emits a signed-overflow-checked op through llvm.s{add,sub,mul}.with.overflow:
    // traps on the intrinsic's overflow bit and returns its wrapping result.
    // Operands must share one integer type. Only used when overflowChecks is On.
    llvm::Value* emitSignedOverflowOp(CajetaModulePtr module,
                                             llvm::IRBuilder<>& b,
                                             llvm::Intrinsic::ID intrinId,
                                             llvm::Value* l,
                                             llvm::Value* r,
                                             const std::string& label) {
        auto* lmod = module->getLlvmModule();
        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
            lmod, intrinId, {l->getType()});
        llvm::Value* pair = b.CreateCall(fn, {l, r}, label + ".pair");
        llvm::Value* ofBit = b.CreateExtractValue(pair, {1}, label + ".of");
        emitUbTrap(module, b, ofBit, label);
        return b.CreateExtractValue(pair, {0}, label + ".val");
    }

    // fp4/fp6/fp8 have no native arithmetic on most targets; widen sub-fp16 operands
    // to fp16, perform the op there, then truncate back to the result type.
    static llvm::Value* emitFpBinOp(CajetaModulePtr module, llvm::Value* lhs, llvm::Value* rhs,
                                    llvm::Instruction::BinaryOps op) {
        auto* builder = module->getBuilder();
        llvm::Type* resultTy = lhs->getType();
        if (resultTy->isFloatingPointTy() && resultTy->getScalarSizeInBits() < 16) {
            llvm::Type* halfTy = llvm::Type::getHalfTy(resultTy->getContext());
            llvm::Value* lw = builder->CreateFPExt(lhs, halfTy);
            llvm::Value* rw = rhs->getType() == halfTy ? rhs : builder->CreateFPExt(rhs, halfTy);
            llvm::Value* res = builder->CreateBinOp(op, lw, rw);
            return builder->CreateFPTrunc(res, resultTy);
        }
        return builder->CreateBinOp(op, lhs, rhs);
    }

    // The kind word for an interface fat body built from `rhsAst`: OWNED when the
    // source tenders a title, a select on its flag when the title is runtime, and
    // BORROWED otherwise. `i64Ty` types the constant or select handed back.
    static llvm::Value* interfaceKindFor(CajetaModulePtr module,
                                         const ExpressionPtr& rhsAst,
                                         llvm::Type* i64Ty) {
        llvm::Constant* ownedK = llvm::ConstantInt::get(
            i64Ty, (uint64_t) IFACE_KIND_OWNED_CLASS);
        llvm::Constant* borrowedK = llvm::ConstantInt::get(
            i64Ty, (uint64_t) IFACE_KIND_BORROWED_CLASS);
        if (!rhsAst) return borrowedK;
        ownership::TitleShape s = ownership::classify(rhsAst, module);
        ownership::TitleVerdict v = ownership::policy(s, ownership::ConsumerRole::StoreSlot);
        if (v.answer == ownership::TitleAnswer::Owned) return ownedK;
        if (v.answer != ownership::TitleAnswer::Runtime) return borrowedK;
        llvm::Value* flag = ownership::titleFlag(s, module);
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(flag)) {
            return c->isZero() ? borrowedK : ownedK;
        }
        auto* builder = module->getBuilder();
        return builder->CreateSelect(
            builder->CreateICmpNE(flag, llvm::ConstantInt::get(i64Ty, 0)),
            ownedK, borrowedK, "iface_kind");
    }

    // Records `arr[i] = local` as a slot borrow on the array's local so the lent
    // value cannot escape the frame. No-op unless both sides are bare identifiers.
    static void recordSlotBorrowOnArray(CajetaModulePtr module,
                                        const ExpressionPtr& lhsIndex,
                                        const ExpressionPtr& rhsAst) {
        if (!lhsIndex || !rhsAst || lhsIndex->kind() != ExprKind::ArrayIndex) return;
        if (rhsAst->kind() != ExprKind::Identifier) return;
        CajetaTypePtr rt = rhsAst->getResolvedType();
        if (!rt || ((rt->getTypeFlags() & PRIMITIVE_FLAG)
                    && !std::dynamic_pointer_cast<CajetaArray>(rt))) {
            return;
        }
        if (auto rc = std::dynamic_pointer_cast<CajetaClass>(rt)) {
            if (rc->getQName() && rc->getQName()->getTypeName() == "String"
                    && rc->getQName()->getPackageName() == "cajeta.lang") {
                return;
            }
        }
        auto& kids = lhsIndex->getChildren();
        if (kids.size() < 2) return;
        auto recv = std::dynamic_pointer_cast<Expression>(kids[0]);
        if (!recv || recv->kind() != ExprKind::Identifier) return;
        auto sc = module->getScopeStack().peek();
        if (!sc) return;
        FieldPtr arrF = sc->getField(
            std::static_pointer_cast<IdentifierExpression>(recv)->getTextValue());
        FieldPtr srcF = sc->getField(
            std::static_pointer_cast<IdentifierExpression>(rhsAst)->getTextValue());
        if (!arrF || !srcF) return;
        if (std::dynamic_pointer_cast<ParameterField>(srcF)) return;
        if (!srcF->getDropEntry() || srcF->isRuntimeConditionalOwner()) return;
        arrF->addSlotBorrowedLocal(-1, srcF->getName());
    }

    // Stores an interface value into an INLINE 24-byte {data, vtable, kind} body:
    // a concrete class instance assembles the body in place, an interface value
    // memcpys it, and null memsets it. A plain store would write only `data`.
    static void storeInterfaceInlineBody(CajetaModulePtr module, llvm::Value* slot,
            llvm::Value* rhsVal, const std::shared_ptr<CajetaClass>& ifaceClass,
            ExpressionPtr rhsAst) {
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* bodyTy = ifaceClass->getLlvmType();

        CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;
        auto rhsClass = std::dynamic_pointer_cast<CajetaClass>(rhsType);
        bool rhsIsInterface = rhsClass && rhsClass->isInterface();

        if (rhsClass && !rhsIsInterface) {
            llvm::Value* dataSlot = builder->CreateStructGEP(bodyTy, slot, 0, "iface_data");
            llvm::Value* vtSlot = builder->CreateStructGEP(bodyTy, slot, 1, "iface_vtable");
            llvm::Value* kindSlot = builder->CreateStructGEP(bodyTy, slot, 2, "iface_kind");
            builder->CreateStore(rhsVal, dataSlot);
            std::string ifaceCanonical = ifaceClass->getQName()->toCanonical();
            llvm::Constant* vtableRef = nullptr;
            if (auto gv = rhsClass->getInterfaceVTable(ifaceCanonical)) {
                vtableRef = CajetaModule::ensureGlobalInModule(
                    module->emitTargetLlvmModule(), gv);
            }
            if (!vtableRef) {
                // The per-(class, iface) vtable is synthesized before any real dispatch site
                // compiles, so a null here can only reach a dead slot.
                vtableRef = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrTy));
            }
            builder->CreateStore(vtableRef, vtSlot);
            builder->CreateStore(interfaceKindFor(module, rhsAst, i64Ty), kindSlot);
        } else if (llvm::isa<llvm::ConstantPointerNull>(rhsVal)) {
            // `arr[i] = null` zeroes the 24-byte body; a memcpy from the null source
            // would fault.
            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
            uint64_t bodyBytes = dl.getTypeAllocSize(bodyTy);
            builder->CreateMemSet(slot, builder->getInt8(0), bodyBytes,
                llvm::MaybeAlign(8));
        } else {
            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
            uint64_t bodyBytes = dl.getTypeAllocSize(bodyTy);
            builder->CreateMemCpy(slot, llvm::MaybeAlign(8),
                rhsVal, llvm::MaybeAlign(8), bodyBytes);
            // The copy's kind is the SOURCE's title, not the source body's word verbatim:
            // two OWNED words would free one instance twice. Null keeps 0.
            {
                llvm::Value* kindSlot = builder->CreateStructGEP(
                    bodyTy, slot, 2, "iface_kind");
                llvm::Value* dataSlot0 = builder->CreateStructGEP(
                    bodyTy, slot, 0, "iface_data");
                llvm::Value* dataVal = builder->CreateLoad(ptrTy, dataSlot0);
                llvm::Value* isNull = builder->CreateIsNull(dataVal);
                llvm::Value* kindVal = builder->CreateSelect(isNull,
                    llvm::ConstantInt::get(i64Ty, 0),
                    interfaceKindFor(module, rhsAst, i64Ty));
                builder->CreateStore(kindVal, kindSlot);
            }
        }
    }

    // Whether an integer `+`, `-` or `*` takes a SIGNED overflow check. Signedness
    // follows the TYPED operand: a bare literal adapts to its sibling and only
    // votes when both operands are literals, so `u - 1` on a uint64 stays unsigned.
    inline bool binaryOverflowIsSigned(ExpressionPtr a, ExpressionPtr b) {
        auto flags = [](ExpressionPtr e) -> long {
            if (!e) return 0;
            auto t = e->getResolvedType();
            return t ? (long) t->getTypeFlags() : 0;
        };
        bool aLit = dynamic_pointer_cast<IntegerLiteralExpression>(a) != nullptr;
        bool bLit = dynamic_pointer_cast<IntegerLiteralExpression>(b) != nullptr;
        if (aLit && !bLit) return (flags(b) & SIGNED_FLAG) != 0;
        if (bLit && !aLit) return (flags(a) & SIGNED_FLAG) != 0;
        return ((flags(a) | flags(b)) & SIGNED_FLAG) != 0;
    }

    // l-value to r-value coercion of `v`, the value `ast` generated: loads through
    // a local or static slot, an array-element GEP and a field GEP, and passes
    // every r-value (calls, `new`, casts, moves, phis, non-pointers) through.
    llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v, ExpressionPtr ast) {
        if (!v) return v;
        if (llvm::isa<llvm::ConstantPointerNull>(v)) {
            return v;
        }
        if (!v->getType()->isPointerTy()) {
            return v;
        }
        auto* builder = module->getBuilder();
        bool treatAllocaAsSlot = !ast
            || dynamic_pointer_cast<IdentifierExpression>(ast) != nullptr;
        if (treatAllocaAsSlot) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                return builder->CreateLoad(a->getAllocatedType(), a);
            }
            if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
                return builder->CreateLoad(g->getValueType(), g);
            }
        }
        if (ast && dynamic_pointer_cast<ArrayIndexExpression>(ast)) {
            CajetaTypePtr elemType = ast->getResolvedType();
            if (elemType) {
                // An interface element is a 24-byte fat-pointer body stored INLINE, so the
                // element GEP already points AT the body: hand it back rather than load it.
                auto elemClass = dynamic_pointer_cast<CajetaClass>(elemType);
                if (elemClass && elemClass->isInterface()
                        && llvm::isa<llvm::GetElementPtrInst>(v)) {
                    return v;
                }
                llvm::Type* loadTy;
                // A value-type element is stored inline, so it loads as its body struct; this
                // must precede the class-ref / STRUCT_FLAG rule below.
                bool elemIsValueType = elemClass && elemClass->isValueType();
                if (elemIsValueType) {
                    loadTy = elemType->getLlvmType();
                } else if (dynamic_pointer_cast<CajetaClass>(elemType) ||
                    (elemType->getTypeFlags() & STRUCT_FLAG)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = elemType->getLlvmType();
                }
                if (loadTy) return builder->CreateLoad(loadTy, v);
            }
        }
        // Gate on `v` being a GEP or global: a variable-size struct field hands back an
        // already-materialized value, which `loadTy != v->getType()` cannot tell apart.
        if (auto dot = dynamic_pointer_cast<DotExpression>(ast)) {
            if (auto resolved = ast->getResolvedType()) {
                auto resolvedClass = dynamic_pointer_cast<CajetaClass>(resolved);
                bool resolvedIsInterface = resolvedClass && resolvedClass->isInterface();
                if (resolvedIsInterface && llvm::isa<llvm::GetElementPtrInst>(v)) {
                    return v;
                }
                // Reference-typed fields, arrays and plain class refs, live in the parent as
                // `ptr`, so they load as `ptr`; views, interfaces and value types keep their
                // body inline and load with the body type.
                llvm::Type* loadTy;
                auto resolvedValCls = dynamic_pointer_cast<CajetaClass>(resolved);
                bool fieldIsValueType = resolvedValCls && resolvedValCls->isValueType();
                bool fieldIsClassRef = resolvedValCls != nullptr
                    && !dynamic_pointer_cast<CajetaView>(resolved)
                    && !dynamic_pointer_cast<CajetaArray>(resolved)
                    && !fieldIsValueType;
                bool fieldIsInterface = false;
                if (auto rc = dynamic_pointer_cast<CajetaClass>(resolved)) {
                    fieldIsInterface = rc->isInterface();
                }
                if (dynamic_pointer_cast<CajetaArray>(resolved)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else if (fieldIsClassRef && !fieldIsInterface) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = resolved->getLlvmType();
                }
                if (loadTy) {
                    if (llvm::isa<llvm::GetElementPtrInst>(v)
                            || llvm::isa<llvm::GlobalVariable>(v)) {
                        llvm::Value* loaded = builder->CreateLoad(loadTy, v);
                        if (!dot->getChildren().empty()) {
                            auto recv = dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                            loaded = DotExpression::maybeBswap(module, loaded, recv);
                        }
                        return loaded;
                    }
                }
                return v;
            }
        }
        if (dynamic_pointer_cast<SpawnExpression>(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<AggregateInitializerExpression>(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<NewExpression>(ast)) {
            return v;
        }
        if (isConditionalKind(ast)) {
            return v;
        }
        if (isMoveKind(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<MethodCallExpression>(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<CastExpression>(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<ArraySliceExpression>(ast)) {
            return v;
        }
        if (dynamic_pointer_cast<ClassLiteralExpression>(ast)) {
            return v;
        }
        if (auto tle = dynamic_pointer_cast<TextLiteralExpression>(ast)) {
            if (auto rt = tle->getResolvedType()) {
                if (dynamic_pointer_cast<CajetaClass>(rt)) {
                    return v;
                }
            }
        }
        if (auto bop = dynamic_pointer_cast<BinaryOpExpression>(ast)) {
            if (bop->getBinaryOp() == BINARY_OP_ADD) {
                if (auto rt = bop->getResolvedType()) {
                    auto cls = dynamic_pointer_cast<CajetaClass>(rt);
                    if (cls && cls->getQName()
                            && cls->getQName()->getTypeName() == "String"
                            && cls->getQName()->getPackageName() == "cajeta.lang") {
                        return v;
                    }
                }
            }
        }
        if (llvm::isa<llvm::CallInst>(v)) {
            return v;
        }
        if (v->getType()->isPointerTy() && ast) {
            if (auto resolved = ast->getResolvedType()) {
                if (dynamic_pointer_cast<CajetaArray>(resolved)) {
                    return v;
                }
                if (dynamic_pointer_cast<CajetaView>(resolved)) {
                    return v;
                }
                if (auto rc = dynamic_pointer_cast<CajetaClass>(resolved)) {
                    if (rc->isInterface()) {
                        return v;
                    }
                    // A class-typed slot holds a `ptr` (the class-pass-by-pointer rule), so load it
                    // as `ptr`: resolved->getLlvmType() is the inline body struct, not the slot.
                    llvm::Type* ptrTy = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                    if (v->getType() == ptrTy) {
                        return builder->CreateLoad(ptrTy, v);
                    }
                }
                if (llvm::Type* loadTy = resolved->getLlvmType()) {
                    if (loadTy != v->getType()) {
                        return builder->CreateLoad(loadTy, v);
                    }
                }
            }
        }
        return v;
    }

    // loadIfLValue for the call sites with no AST to hand: only an AllocaInst is
    // unwrapped.
    static llvm::Value* loadIfAlloca(CajetaModulePtr module, llvm::Value* v) {
        return loadIfLValue(module, v, nullptr);
    }

    // Arithmetic on two POINTER operands, reached only from a wildcard monomorph's
    // dead body, which still has to pass LLVM verify: round-trips through i64 so
    // `-`, `*` and `/` are legal IR. Never executed.
    static llvm::Value* deadPtrArith(CajetaModulePtr module, llvm::Value* l,
            llvm::Value* r, int op) {
        auto* builder = module->getBuilder();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(*module->getLlvmContext());
        llvm::Value* li = builder->CreatePtrToInt(l, i64Ty, "deadptr.l");
        llvm::Value* ri = builder->CreatePtrToInt(r, i64Ty, "deadptr.r");
        llvm::Value* v = nullptr;
        if (op == BINARY_OP_SUB) {
            v = builder->CreateSub(li, ri);
        } else if (op == BINARY_OP_MUL) {
            v = builder->CreateMul(li, ri);
        } else {
            llvm::Value* zero = llvm::ConstantInt::get(i64Ty, 0);
            llvm::Value* isZero = builder->CreateICmpEQ(ri, zero);
            llvm::Value* safe = builder->CreateSelect(isZero,
                llvm::ConstantInt::get(i64Ty, 1), ri);
            v = builder->CreateSDiv(li, safe);
        }
        return builder->CreateIntToPtr(v, l->getType(), "deadptr.res");
    }

    // Coerces two arithmetic operands to one LLVM type: a vector splats the scalar
    // side, FP wins over integer, then the wider type wins. `lhsSigned`/`rhsSigned`
    // pick the FILL BIT when a narrower operand widens, defaulting to signed.
    static std::pair<llvm::Value*, llvm::Value*> coerceArithPair(
            CajetaModulePtr module, llvm::Value* l, llvm::Value* r,
            bool lhsSigned = true, bool rhsSigned = true) {
        auto* builder = module->getBuilder();
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return {l, r};

        if (lt->isVectorTy() || rt->isVectorTy()) {
            if (lt->isVectorTy() && !rt->isVectorTy()) {
                auto* vt = llvm::cast<llvm::FixedVectorType>(lt);
                r = vecops::splat(*builder,
                    vecops::coerceScalar(*builder, r, vt->getElementType()),
                    vt->getNumElements());
            } else if (rt->isVectorTy() && !lt->isVectorTy()) {
                auto* vt = llvm::cast<llvm::FixedVectorType>(rt);
                l = vecops::splat(*builder,
                    vecops::coerceScalar(*builder, l, vt->getElementType()),
                    vt->getNumElements());
            }
            return {l, r};
        }

        if (lt->isFloatingPointTy() || rt->isFloatingPointTy()) {
            if (lt->isIntegerTy()) l = builder->CreateSIToFP(l, rt);
            if (rt->isIntegerTy()) r = builder->CreateSIToFP(r, lt);
            lt = l->getType();
            rt = r->getType();
            if (lt->getScalarSizeInBits() > rt->getScalarSizeInBits()) {
                r = builder->CreateFPExt(r, lt);
            } else if (rt->getScalarSizeInBits() > lt->getScalarSizeInBits()) {
                l = builder->CreateFPExt(l, rt);
            }
            return {l, r};
        }
        if (lt->isIntegerTy() && rt->isIntegerTy()) {
            unsigned lb = lt->getScalarSizeInBits();
            unsigned rb = rt->getScalarSizeInBits();
            if (lb < rb) l = builder->CreateIntCast(l, rt, lhsSigned);
            else if (rb < lb) r = builder->CreateIntCast(r, lt, rhsSigned);
            return {l, r};
        }
        return {l, r};
    }


    // Locates the hidden ownership word for a `recv.field` store: writes the word's
    // address (a FIXED byte delta from the field slot, so it holds inside any
    // descendant) and the field's bit index. False, outputs untouched, if no bit.
    static bool locateFieldOwnershipBit(
            const CajetaModulePtr& module, llvm::IRBuilder<>* builder,
            const std::shared_ptr<DotExpression>& dot, llvm::Value* slotPtr,
            llvm::Value** wordPtrOut, int* bitIdxOut,
            StructurePropertyPtr* propOut = nullptr) {
        if (!dot || !slotPtr || !slotPtr->getType()->isPointerTy()) return false;
        auto& ch = dot->getChildren();
        auto recv = ch.empty() ? nullptr
            : std::dynamic_pointer_cast<Expression>(ch[0]);
        if (auto aix = std::dynamic_pointer_cast<ArrayIndexExpression>(
                ch.empty() ? nullptr : ch[0])) {
            if (!aix->getResolvedType()) aix->resolveTypes(module);
            if (!CajetaClass::arrayElementCarriesMemberBits(
                    aix->getResolvedType())) {
                recv = nullptr;
            }
        }
        if (recv && !recv->getResolvedType()) recv->resolveTypes(module);
        auto recvClass = recv
            ? std::dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())
            : nullptr;
        if (!recvClass) return false;

        StructurePropertyPtr prop;
        CajetaClassPtr decl;
        std::function<bool(const CajetaClassPtr&)> find =
            [&](const CajetaClassPtr& cls) -> bool {
                if (!cls) return false;
                auto pit = cls->getProperties().find(dot->getIdentifier());
                if (pit != cls->getProperties().end()) {
                    prop = pit->second;
                    decl = cls;
                    return true;
                }
                for (auto& sup : cls->getSuperClasses()) {
                    if (find(sup)) return true;
                }
                return false;
            };
        find(recvClass);
        if (!prop || !decl || !CajetaClass::fieldHasOwnershipBit(prop)) {
            return false;
        }

        int fieldIdx = decl->getFieldLlvmIndex(prop);
        int wordIdx = decl->getOwnershipWordLlvmIndex();
        auto* declTy = llvm::dyn_cast_or_null<llvm::StructType>(
            decl->getLlvmType());
        if (fieldIdx < 0 || wordIdx < 0 || !declTy || declTy->isOpaque()) {
            return false;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        const llvm::StructLayout* sl = dl.getStructLayout(declTy);
        int64_t delta = (int64_t) sl->getElementOffset((unsigned) wordIdx)
            - (int64_t) sl->getElementOffset((unsigned) fieldIdx);
        auto& ctx = *module->getLlvmContext();
        *wordPtrOut = builder->CreateInBoundsGEP(
            llvm::Type::getInt8Ty(ctx), slotPtr,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), delta),
            "own_bits_addr");
        *bitIdxOut = decl->ownershipBitIndexOf(prop);
        if (propOut) *propOut = prop;
        return true;
    }

    // `*` on a Matrix LHS: matmul against a Matrix (inner dimension checked),
    // matVec against a Vector, element-wise scale against a scalar. Stamps
    // resolvedType; returns nullptr for any other op or RHS, so the caller falls through.
    llvm::Value* BinaryOpExpression::generateMatrixMul(
            CajetaModulePtr module, llvm::Value* lhs, llvm::Value* rhs,
            const ExpressionPtr& lhsAst, const ExpressionPtr& rhsAst) {
        if (binaryOp != BINARY_OP_MUL) return nullptr;
        auto lhsM = dynamic_pointer_cast<CajetaMatrix>(lhsAst->getResolvedType());
        if (!lhsM) return nullptr;
        auto* builder = module->getBuilder();
        bool isFloat =
            lhsM->getElementType()->getLlvmType()->isFloatingPointTy();
        llvm::Value* l = loadIfLValue(module, lhs, lhsAst);
        CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;

        if (auto rhsM = dynamic_pointer_cast<CajetaMatrix>(rhsType)) {
            if (lhsM->getCols() != rhsM->getRows()) {
                throw Exception(
                    "Matrix multiply shape mismatch: " + lhsM->toCanonical()
                    + " * " + rhsM->toCanonical() + " (inner dimensions "
                    + std::to_string(lhsM->getCols()) + " and "
                    + std::to_string(rhsM->getRows()) + " must match)",
                    "CAJETA_ERROR_MATRIX_SHAPE");
            }
            llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
            resolvedType = CajetaMatrix::getOrCreate(
                module, lhsM->getElementType(), lhsM->getRows(), rhsM->getCols());
            return matops::matmul(*builder, l, lhsM->getRows(), lhsM->getCols(),
                                  r, rhsM->getCols(), isFloat);
        }
        if (auto rhsV = dynamic_pointer_cast<CajetaVector>(rhsType)) {
            if (rhsV->getLanes() != lhsM->getCols()) {
                throw Exception(
                    "Matrix-vector shape mismatch: " + lhsM->toCanonical()
                    + " * " + rhsV->toCanonical() + " (vector length "
                    + std::to_string(rhsV->getLanes()) + " must equal column "
                    "count " + std::to_string(lhsM->getCols()) + ")",
                    "CAJETA_ERROR_MATRIX_SHAPE");
            }
            llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
            resolvedType = CajetaVector::getOrCreate(
                module, lhsM->getElementType(), lhsM->getRows());
            return matops::matVec(*builder, l, lhsM->getRows(), lhsM->getCols(),
                                  r, isFloat);
        }
        if (rhsType && (rhsType->getTypeFlags() & PRIMITIVE_FLAG)
                && (rhsType->getTypeFlags() & NUMBER_FLAG)) {
            llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
            r = vecops::coerceScalar(*builder, r,
                                     lhsM->getElementType()->getLlvmType());
            resolvedType = lhsM;
            return matops::scale(*builder, l, r, isFloat);
        }
        return nullptr;
    }

    CajetaTypePtr BinaryOpExpression::comparisonResultType(CajetaModulePtr module) {
        CajetaTypePtr boolTy = CajetaType::of("boolean");
        if (children.size() < 2) return boolTy;
        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        auto rhs = dynamic_pointer_cast<Expression>(children[1]);
        if (!lhs || !rhs) return boolTy;
        CajetaTypePtr lt = lhs->getResolvedType();
        CajetaTypePtr rt = rhs->getResolvedType();
        if (!lt && !rt) return nullptr;
        const char* sym = nullptr;
        switch (binaryOp) {
            case BINARY_OP_LT: sym = "<";  break;
            case BINARY_OP_LE: sym = "<="; break;
            case BINARY_OP_GT: sym = ">";  break;
            case BINARY_OP_GE: sym = ">="; break;
            case BINARY_OP_EQ: sym = "=="; break;
            case BINARY_OP_NE: sym = "!="; break;
            default: return boolTy;
        }
        std::string name = std::string("operator") + sym;
        for (const CajetaTypePtr& side : {lt, rt}) {
            auto cls = dynamic_pointer_cast<CajetaClass>(side);
            if (!cls || cls->isInterface()
                    || (cls->getTypeFlags() & PRIMITIVE_FLAG)) {
                continue;
            }
            vector<ParameterEntry> entries;
            entries.push_back(ParameterEntry(lt ? lt : side, "", nullptr));
            entries.push_back(ParameterEntry(rt ? rt : side, "", nullptr));
            if (MethodPtr m = cls->resolveMethod(name, entries,
                    /*isConstructor=*/false, /*floatingParams=*/false)) {
                if (m->getReturnType()) return m->getReturnType();
            }
        }
        return boolTy;
    }

    // Lowers every binary form: short-circuit `&&`/`||`, assignment and compound
    // assignment with the ownership bookkeeping a store implies, operator overloads,
    // matrix/vector/quaternion ops, String concatenation, and primitive arithmetic.
    llvm::Value* BinaryOpExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (std::getenv("CAJETA_TRACE_BINOP") || std::getenv("CAJETA_DEBUG_ICMP")) {
            llvm::errs() << "BinaryOpExpression::generateCode binaryOp=" << binaryOp
                         << " children.size=" << children.size() << "\n";
        }

        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2) {
            rejectStaleGenerationUse(
                module, dynamic_pointer_cast<Expression>(children[1]),
                "the assigned variable");
        }

        // Short-circuit ops evaluate rhs conditionally, ahead of the upfront-evaluate path.
        if (binaryOp == BINARY_OP_LOGAND || binaryOp == BINARY_OP_LOGOR) {
            auto lhsAst = dynamic_pointer_cast<Expression>(children[0]);
            llvm::Value* lhsVal = loadIfLValue(
                module, children[0]->generateCode(module), lhsAst);
            llvm::Type* i1Ty = llvm::Type::getInt1Ty(*module->getLlvmContext());
            if (lhsVal->getType() != i1Ty) {
                llvm::Value* zero = llvm::ConstantInt::get(lhsVal->getType(), 0);
                lhsVal = builder->CreateICmpNE(lhsVal, zero);
            }
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::LLVMContext& ctx = *module->getLlvmContext();
            llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(ctx,
                binaryOp == BINARY_OP_LOGAND ? "land_rhs" : "lor_rhs", parentFn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx,
                binaryOp == BINARY_OP_LOGAND ? "land_merge" : "lor_merge", parentFn);
            llvm::BasicBlock* lhsBB = builder->GetInsertBlock();
            if (binaryOp == BINARY_OP_LOGAND) {
                builder->CreateCondBr(lhsVal, rhsBB, mergeBB);
            } else {
                builder->CreateCondBr(lhsVal, mergeBB, rhsBB);
            }
            builder->SetInsertPoint(rhsBB);
            auto rhsAst = dynamic_pointer_cast<Expression>(children[1]);
            llvm::Value* rhsVal = loadIfLValue(
                module, children[1]->generateCode(module), rhsAst);
            if (rhsVal->getType() != i1Ty) {
                llvm::Value* zero = llvm::ConstantInt::get(rhsVal->getType(), 0);
                rhsVal = builder->CreateICmpNE(rhsVal, zero);
            }
            llvm::BasicBlock* rhsEndBB = builder->GetInsertBlock();
            builder->CreateBr(mergeBB);
            builder->SetInsertPoint(mergeBB);
            llvm::PHINode* phi = builder->CreatePHI(i1Ty, 2);
            phi->addIncoming(rhsVal, rhsEndBB);
            phi->addIncoming(
                binaryOp == BINARY_OP_LOGAND
                    ? llvm::ConstantInt::getFalse(ctx)
                    : llvm::ConstantInt::getTrue(ctx),
                lhsBB);
            return phi;
        }

        // Indexed-assignment overload: `recv[idx] = value` on a class declaring
        // `operator[]=`. Must short-circuit BEFORE the LHS codegen below, which would
        // call `operator[]` (the read form) for its side effects and discard it.
        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2) {
            if (auto eIdx = dynamic_pointer_cast<ArrayIndexExpression>(
                    children[0])) {
                auto& ech = eIdx->getChildren();
                auto eRecv = ech.empty() ? nullptr
                    : dynamic_pointer_cast<DotExpression>(ech[0]);
                bool onThis = false;
                if (eRecv) {
                    auto& rch = eRecv->getChildren();
                    onThis = !rch.empty()
                        && dynamic_pointer_cast<ThisExpression>(rch[0]);
                }
                if (onThis) {
                    if (auto eSrc = dynamic_pointer_cast<IdentifierExpression>(
                            children[1])) {
                        if (auto sc = module->getScopeStack().peek()) {
                            sc->rejectCapturedBorrowParam(
                                eSrc->getTextValue(),
                                "element of `" + eRecv->getIdentifier() + "`",
                                (int) getSourceLine());
                        }
                    }
                }
            }
        }
        if (binaryOp == BINARY_OP_ASSIGN
                && !children.empty()
                && dynamic_pointer_cast<ArrayIndexExpression>(children[0])) {
            auto arrIdxAst = dynamic_pointer_cast<Expression>(children[0]);
            auto& arrIdxChildren = arrIdxAst->getChildren();
            if (arrIdxChildren.size() >= 2) {
                auto recvAst = dynamic_pointer_cast<Expression>(arrIdxChildren[0]);
                auto idxAst  = dynamic_pointer_cast<Expression>(arrIdxChildren[1]);
                auto valAst  = dynamic_pointer_cast<Expression>(children[1]);
                if (recvAst && idxAst && valAst) {
                    if (!recvAst->getResolvedType()) recvAst->resolveTypes(module);
                    auto recvClass = dynamic_pointer_cast<CajetaClass>(
                        recvAst->getResolvedType());
                    bool recvIsArr = dynamic_pointer_cast<CajetaArray>(
                        recvAst->getResolvedType()) != nullptr;
                    if (recvClass && !recvIsArr && !recvClass->isInterface()
                            && !(recvClass->getTypeFlags() & PRIMITIVE_FLAG)) {
                        if (!idxAst->getResolvedType()) idxAst->resolveTypes(module);
                        if (!valAst->getResolvedType()) valAst->resolveTypes(module);
                        CajetaTypePtr idxType = idxAst->getResolvedType();
                        CajetaTypePtr valType = valAst->getResolvedType();
                        // In `m[k] #= call()` the operand has no resolved type yet; the callee's return type is it.
                        if (!valType) {
                            ExpressionPtr valLeaf = moveInner(valAst);
                            ownership::TitleShape vs = ownership::classify(
                                valLeaf ? valLeaf : valAst, module);
                            if (vs.callee) valType = vs.callee->getReturnType();
                        }
                        if (idxType && valType) {
                            llvm::Value* recvVal = recvAst->generateCode(module);
                            llvm::Value* idxVal  = idxAst->generateCode(module);
                            llvm::Value* valVal  = valAst->generateCode(module);
                            recvVal = loadIfLValue(module, recvVal, recvAst);
                            idxVal = loadIfLValue(module, idxVal, idxAst);
                            valVal = loadIfLValue(module, valVal, valAst);
                            std::vector<ParameterEntry> entries;
                            entries.push_back(ParameterEntry(idxType, "", idxVal));
                            entries.push_back(ParameterEntry(valType, "", valVal));
                            std::string opName = "operator[]=";
                            // The `#T` contract check for the indexed store: this form lowers straight to
                            // invokeMethod, so MethodCallExpression's own check never sees it.
                            if (auto opTgt = recvClass->resolveMethod(opName,
                                    entries, /*isConstructor=*/false,
                                    /*floatingParams=*/false)) {
                                auto opFormals = opTgt->getParameterList();
                                bool opStatic = opTgt->getModifiers().find(STATIC)
                                    != opTgt->getModifiers().end();
                                bool opHasThis = !opFormals.empty()
                                    && opFormals.front()->getName() == "this";
                                size_t opOff = (opStatic || !opHasThis) ? 0 : 1;
                                ExpressionPtr opArgs[2] = { idxAst, valAst };
                                for (size_t a = 0; a < 2; ++a) {
                                    size_t fi = opOff + a;
                                    if (fi >= opFormals.size()) break;
                                    auto& ofp = opFormals[fi];
                                    if (!ofp || !ofp->isTransferred()) continue;
                                    if (!opArgs[a]) continue;
                                    ownership::rejectOwnedFormalArgument(opArgs[a],
                                        /*callerTransferred=*/false, module, opName,
                                        ofp->getName(), (int) getSourceLine());
                                }
                            }
                            // The lowered call's transfer word: bit 0 is the index argument, bit 1 the
                            // value argument; a runtime-owner source forwards its captured flag.
                            for (ExpressionPtr advAst : { idxAst, valAst }) {
                                auto advId = std::dynamic_pointer_cast<IdentifierExpression>(advAst);
                                if (!advId) continue;
                                auto advScope = module->getScopeStack().peek();
                                if (!advScope) continue;
                                FieldPtr advF = advScope->getField(advId->getTextValue());
                                bool advIsLocalOwner = advF && advF->getDropEntry()
                                    && !std::dynamic_pointer_cast<ParameterField>(advF);
                                if (!advIsLocalOwner) continue;
                                auto advM = module->getCurrentMethod();
                                if (!advM || !advM->isFinalUseOfLocal(advId->getTextValue(),
                                        advId->getSourceLine(), advId->getSourceColumn())) {
                                    continue;
                                }
                                if (auto* eng = DiagnosticEngine::active()) {
                                    eng->report("warning", "CAJETA_WARN_LAST_USE_TRANSFER",
                                        "`" + advId->getTextValue() + "` is lent here at its "
                                        "final use (an indexed store lends, like `put`), so "
                                        "nothing in this scope reads it again — if the map is "
                                        "meant to KEEP it, spell `#" + advId->getTextValue()
                                            + "` to transfer the title. As written the title "
                                        "stays with the local and is released at scope exit.",
                                        module->getSourcePath(),
                                        advId->getSourceLine(), advId->getSourceColumn() + 1);
                                }
                            }
                            llvm::Value* opxWord = builder->getInt64(0);
                            {
                                int64_t constBits = 0;
                                llvm::Value* dynWord = nullptr;
                                auto wordBit = [&](ExpressionPtr ast, int bit) {
                                    if (!ast) return;
                                    ownership::TitleShape ws =
                                        ownership::classify(ast, module);
                                    if (ws.answer != ownership::TitleAnswer::Owned
                                            && ws.answer != ownership::TitleAnswer::Runtime) {
                                        return;
                                    }
                                    llvm::Value* rf = ownership::titleFlag(ws, module);
                                    if (auto* rc = llvm::dyn_cast<llvm::ConstantInt>(rf)) {
                                        if (!rc->isZero()) constBits |= ((int64_t) 1) << bit;
                                        return;
                                    }
                                    llvm::Value* b = builder->CreateShl(
                                        builder->CreateAnd(rf, builder->getInt64(1)),
                                        builder->getInt64((uint64_t) bit));
                                    dynWord = dynWord ? builder->CreateOr(dynWord, b) : b;
                                };
                                wordBit(idxAst, 0);
                                wordBit(valAst, 1);
                                if (dynWord) {
                                    opxWord = constBits
                                        ? builder->CreateOr(dynWord,
                                              builder->getInt64(
                                                  (uint64_t) constBits))
                                        : dynWord;
                                } else if (constBits) {
                                    opxWord = builder->getInt64(
                                        (uint64_t) constBits);
                                }
                            }
                            if (auto m = recvClass->resolveMethod(opName,
                                    entries, /*isConstructor=*/false,
                                    /*floatingParams=*/false)) {
                                recvClass->invokeMethod(opName, entries,
                                    /*isConstructor=*/false, recvVal,
                                    /*callerModule=*/module,
                                    /*forceDirectCall=*/false,
                                    /*explicitMethodTypeArgs=*/{},
                                    /*sretTarget=*/nullptr,
                                    /*transferWord=*/opxWord);
                                return valVal;
                            }
                        }
                        // A class that DECLARES `operator[]=` but matched no overload must not fall
                        // through: the array store below would misread the receiver.
                        bool declaresIndexedStore = false;
                        for (auto& dm : recvClass->getMethodList()) {
                            if (dm && dm->getName() == "operator[]=") { declaresIndexedStore = true; break; }
                        }
                        if (declaresIndexedStore) {
                            throw Exception(
                                "indexed store on `" + recvClass->toCanonical()
                                + "`: no `operator[]=` takes (`"
                                + (idxType ? idxType->toCanonical() : std::string("?"))
                                + "`, `" + (valType ? valType->toCanonical() : std::string("?"))
                                + "`)" + (valType ? std::string() : std::string(
                                    " — the value's type is not resolved here (a call "
                                    "whose callee is not yet known?); bind it to a typed "
                                    "local first")) + ".",
                                "CAJETA_ERROR_UNRESOLVED_METHOD",
                                module->getSourcePath(), (int) getSourceLine(), -1);
                        }
                    }
                }
            }
        }

        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2
                && (dynamic_pointer_cast<DotExpression>(children[0])
                    || dynamic_pointer_cast<ArrayIndexExpression>(children[0]))) {
            if (auto rhsId = dynamic_pointer_cast<IdentifierExpression>(
                    children[1])) {
                auto psScope = module->getScopeStack().peek();
                FieldPtr psSrc = psScope
                    ? psScope->getField(rhsId->getTextValue()) : nullptr;
                if (std::getenv("CAJETA_TRACE_PLAINSTORE")
                        && psSrc && psSrc->isRuntimeConditionalOwner()) {
                    auto psM = module->getCurrentMethod();
                    std::string psCls;
                    if (!module->getStructureStack().empty()
                            && module->getStructureStack().back()) {
                        psCls = module->getStructureStack().back()
                            ->getQName()->toCanonical();
                    }
                    llvm::errs() << "[plainstore] " << psCls
                        << ":" << getSourceLine()
                        << " m=" << (psM ? psM->getName() : std::string("?"))
                        << " rhs=" << rhsId->getTextValue()
                        << " f=" << (const void*) psSrc.get() << "\n";
                }
                if (psSrc && psSrc->isRuntimeConditionalOwner()
                        && !psSrc->isOwnershipAudited()) {
                    if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
                        const std::string& n = rhsId->getTextValue();
                        eng->report("warning", "CAJETA_WARN_PLAIN_RETAIN_STORE",
                            "`" + n + "` may arrive owned on some calls; a "
                            "plain store borrows it and the armed entry frees "
                            "it at exit. Spell `dst #= " + n + "` to move its "
                            "title, or store a copy (`dst = " + n + ".clone()`)",
                            module->getSourcePath(), (int) getSourceLine(), -1);
                    }
                }
            }
        }

        // Mark a bare-identifier LHS assigned BEFORE the LHS generates: it is a write
        // target, and its codegen would otherwise trip the not-yet-assigned check.
        bool lhsWasMoved = false;
        if (binaryOp == BINARY_OP_ASSIGN && !children.empty()) {
            if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (auto sc = module->getScopeStack().peek()) {
                    sc->markAssigned(lhsId->getTextValue());
                    lhsWasMoved = sc->isBorrow(lhsId->getTextValue());
                    if (lhsWasMoved) sc->restoreOwnership(lhsId->getTextValue());
                }
            }
        }

        // Fuse `dst[i] #= src[j]` into a FORWARDING slot move before the RHS codegen:
        // the source bit transfers verbatim and the borrowed-take panic is suppressed.
        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2) {
            auto fwdLhs = dynamic_pointer_cast<ArrayIndexExpression>(children[0]);
            std::shared_ptr<MoveExpression> fwdMv = isMoveKind(children[1])
                ? std::static_pointer_cast<MoveExpression>(children[1]) : nullptr;
            while (fwdMv && !fwdMv->getChildren().empty()) {
                auto deeper = isMoveKind(fwdMv->getChildren()[0])
                    ? std::static_pointer_cast<MoveExpression>(fwdMv->getChildren()[0]) : nullptr;
                if (!deeper) break;
                fwdMv = deeper;
            }
            auto fwdDotLhs = dynamic_pointer_cast<DotExpression>(children[0]);
            auto fwdIdLhs = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if ((fwdLhs || fwdDotLhs || fwdIdLhs) && fwdMv && !fwdMv->getChildren().empty()) {
                auto srcNode = fwdMv->getChildren()[0];
                bool srcIsSlot = dynamic_pointer_cast<ArrayIndexExpression>(srcNode) != nullptr
                    || dynamic_pointer_cast<DotExpression>(srcNode) != nullptr;
                if (srcIsSlot) {
                    auto fwdL = dynamic_pointer_cast<Expression>(children[0]);
                    auto fwdS = dynamic_pointer_cast<Expression>(srcNode);
                    if (fwdL && !fwdL->getResolvedType()) fwdL->resolveTypes(module);
                    if (fwdS && !fwdS->getResolvedType()) fwdS->resolveTypes(module);
                    auto fwdBits = [](const CajetaTypePtr& t) {
                        return CajetaClass::arrayElementCarriesSlotBits(t)
                            || CajetaClass::arrayElementCarriesArraySlotBits(t);
                    };
                    if (fwdL && fwdS
                            && (fwdIdLhs || fwdBits(fwdL->getResolvedType()))
                            && fwdBits(fwdS->getResolvedType())) {
                        fwdMv->setForwardingSlotMove(true);
                    }
                }
            }
        }
        // Target-type an RHS array literal from the LHS element type BEFORE the operands
        // generate below, so `int64[] xs; xs = [1,2,3];` widens per the LHS.
        if (binaryOp == BINARY_OP_ASSIGN) {
            if (auto arrLit =
                    dynamic_pointer_cast<ArrayLiteralExpression>(children[1])) {
                if (auto lhsE = dynamic_pointer_cast<Expression>(children[0])) {
                    if (!lhsE->getResolvedType()) lhsE->resolveTypes(module);
                    CajetaTypePtr lt = lhsE->getResolvedType();
                    if (auto at = dynamic_pointer_cast<CajetaArray>(lt)) {
                        arrLit->setElementType(at->getElementType());
                    } else if (auto ctor =
                                   collectionLiteralFromArray(lt, arrLit)) {
                        children[1] = ctor;
                    }
                }
            } else if (auto agg = dynamic_pointer_cast<
                           AggregateInitializerExpression>(children[1])) {
                if (auto lhsE = dynamic_pointer_cast<Expression>(children[0])) {
                    if (!lhsE->getResolvedType()) lhsE->resolveTypes(module);
                    CajetaTypePtr lt = lhsE->getResolvedType();
                    if (dynamic_pointer_cast<CajetaClass>(lt)
                            && !dynamic_pointer_cast<CajetaArray>(lt)) {
                        agg->setExpectedType(lt);
                    }
                }
            } else if (auto mapLit = dynamic_pointer_cast<
                           MapLiteralExpression>(children[1])) {
                if (auto lhsE = dynamic_pointer_cast<Expression>(children[0])) {
                    if (!lhsE->getResolvedType()) lhsE->resolveTypes(module);
                    CajetaTypePtr lt = lhsE->getResolvedType();
                    if (dynamic_pointer_cast<CajetaClass>(lt)
                            && !dynamic_pointer_cast<CajetaArray>(lt)) {
                        mapLit->setExpectedType(lt);
                    }
                }
            }
        }
        llvm::Value* lhs = children[0]->generateCode(module);
        llvm::Value* rhs = children[1]->generateCode(module);
        ExpressionPtr lhsAst = dynamic_pointer_cast<Expression>(children[0]);
        ExpressionPtr rhsAst = dynamic_pointer_cast<Expression>(children[1]);

        // An lvalue acquires exactly as a declaration does: a plain store of a `#T`
        // result records a BORROW of a value nothing owns, so the temporary is freed
        // at end of statement. Sited AFTER the RHS codegen, which resolves the overload.
        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2) {
            if (auto rhsCall = dynamic_pointer_cast<MethodCallExpression>(
                    children[1])) {
                if (MethodPtr rm = rhsCall->getResolvedMethod()) {
                    if (rm->isReturnsOwnership()) {
                        auto holder = module->getCurrentMethod();
                        std::string in = holder
                            ? (holder->getParent()
                                ? holder->getParent()->toCanonical() + "."
                                : std::string()) + holder->getName()
                            : std::string("<none>");
                        std::string calleeKey =
                            (rm->getParent()
                                ? rm->getParent()->toCanonical() + "."
                                : std::string()) + rm->getName();
                        const bool synthAsg = holder
                            && (holder->isMethodTemplateInstantiation()
                                || (holder->getParent()
                                    && holder->getParent()->isInstantiation()));
                        if (ownership::ReturnTitleAudit::enabled()
                                && !ownership::ownedBindWarns()) {
                            ownership::ReturnTitleAudit::ownedBind(
                                calleeKey + " pos=assign", in,
                                synthAsg ? 0 : getSourceLine());
                        }
                        // Classpath origin: a note, never an error, since the consumer cannot edit the archive.
                        bool cpOriginAsg = module->isClasspathOrigin();
                        if (!cpOriginAsg && holder && holder->getParent()
                                && holder->getParent()->getModule()) {
                            cpOriginAsg = holder->getParent()->getModule()
                                ->isClasspathOrigin();
                        }
                        ownership::rejectPlainOwnedBind(
                            calleeKey,
                            assignTargetText(
                                dynamic_pointer_cast<Expression>(children[0])),
                            module->getSourcePath(),
                            synthAsg ? 0 : (int) getSourceLine(),
                            in + " pos=assign", cpOriginAsg);
                    }
                }
            }
        }

        if (lhs == nullptr || rhs == nullptr) {
            const char* opSym = opdispatch::binaryOpSymbol(binaryOp);
            std::string side = (lhs == nullptr && rhs == nullptr) ? "both operands"
                             : (lhs == nullptr ? "the left operand"
                                               : "the right operand");
            throw locatedException(
                getSourceLine(), getSourceColumn() + 1,
                std::string("binary operator '") + (opSym ? opSym : "?")
                + "' has no value for " + side
                + " (a sub-expression did not resolve to a value — e.g. a member"
                  " or method that does not exist on that type, such as `.length`"
                  " on an array, where the size accessor is `count()`)",
                "CAJETA_ERROR_NULL_OPERAND");
        }
        // A `void` call is present but valueless, so the null guard above misses it.
        {
            auto isVoidOperand = [](const ExpressionPtr& e) {
                return e && e->getResolvedType()
                    && e->getResolvedType()->toCanonical() == "void";
            };
            bool lVoid = isVoidOperand(lhsAst);
            bool rVoid = isVoidOperand(rhsAst);
            if (lVoid || rVoid) {
                const char* opSym = opdispatch::binaryOpSymbol(binaryOp);
                std::string what = opSym
                    ? (std::string("binary operator '") + opSym + "'")
                    : std::string("assignment");
                std::string side = (lVoid && rVoid) ? "both operands"
                                 : (lVoid ? "the left operand"
                                          : "the right-hand side");
                throw locatedException(
                    getSourceLine(), getSourceColumn() + 1,
                    what + " has a 'void' expression as " + side
                    + " (a void call produces no value to use here)",
                    "CAJETA_ERROR_NULL_OPERAND");
            }
        }

        // Matrix ops, intercepted before the built-in/vector path: `+ - /` are
        // element-wise over same-shape matrices, `== !=` reduce to masks, `*` is matmul.
        if (lhsAst) {
            if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
            if (auto lhsM = dynamic_pointer_cast<CajetaMatrix>(
                    lhsAst->getResolvedType())) {
                if (rhsAst && !rhsAst->getResolvedType())
                    rhsAst->resolveTypes(module);
                auto rhsM = rhsAst ? dynamic_pointer_cast<CajetaMatrix>(
                    rhsAst->getResolvedType()) : nullptr;
                bool isFloat = lhsM->getElementType()->getLlvmType()
                    ->isFloatingPointTy();
                bool isSigned =
                    (lhsM->getElementType()->getTypeFlags() & SIGNED_FLAG) != 0;
                bool isCmp = binaryOp == BINARY_OP_EQ || binaryOp == BINARY_OP_NE
                    || binaryOp == BINARY_OP_LT || binaryOp == BINARY_OP_LE
                    || binaryOp == BINARY_OP_GT || binaryOp == BINARY_OP_GE;
                if (isCmp) {
                    llvm::Value* l = loadIfLValue(module, lhs, lhsAst);
                    llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                    if (rhsM) {
                        if (rhsM->getRows() != lhsM->getRows()
                                || rhsM->getCols() != lhsM->getCols()) {
                            throw Exception(
                                "Matrix comparison requires same-shape operands "
                                "(got " + lhsM->toCanonical() + " and "
                                + rhsM->toCanonical() + ")",
                                "CAJETA_ERROR_MATRIX_SHAPE");
                        }
                    } else {
                        auto* vt = llvm::cast<llvm::FixedVectorType>(l->getType());
                        r = vecops::splat(*builder,
                            vecops::coerceScalar(*builder, r, vt->getElementType()),
                            vt->getNumElements());
                    }
                    llvm::Value* cmp = nullptr;
                    switch (binaryOp) {
                        case BINARY_OP_EQ: cmp = isFloat
                            ? builder->CreateFCmpOEQ(l, r, "mat.cmp")
                            : builder->CreateICmpEQ(l, r, "mat.cmp"); break;
                        case BINARY_OP_NE: cmp = isFloat
                            ? builder->CreateFCmpUNE(l, r, "mat.cmp")
                            : builder->CreateICmpNE(l, r, "mat.cmp"); break;
                        case BINARY_OP_LT: cmp = isFloat
                            ? builder->CreateFCmpOLT(l, r, "mat.cmp")
                            : (isSigned ? builder->CreateICmpSLT(l, r, "mat.cmp")
                                        : builder->CreateICmpULT(l, r, "mat.cmp"));
                            break;
                        case BINARY_OP_LE: cmp = isFloat
                            ? builder->CreateFCmpOLE(l, r, "mat.cmp")
                            : (isSigned ? builder->CreateICmpSLE(l, r, "mat.cmp")
                                        : builder->CreateICmpULE(l, r, "mat.cmp"));
                            break;
                        case BINARY_OP_GT: cmp = isFloat
                            ? builder->CreateFCmpOGT(l, r, "mat.cmp")
                            : (isSigned ? builder->CreateICmpSGT(l, r, "mat.cmp")
                                        : builder->CreateICmpUGT(l, r, "mat.cmp"));
                            break;
                        case BINARY_OP_GE: cmp = isFloat
                            ? builder->CreateFCmpOGE(l, r, "mat.cmp")
                            : (isSigned ? builder->CreateICmpSGE(l, r, "mat.cmp")
                                        : builder->CreateICmpUGE(l, r, "mat.cmp"));
                            break;
                        default: break;
                    }
                    resolvedType = CajetaMatrix::getOrCreate(
                        module, CajetaType::of("boolean"),
                        lhsM->getRows(), lhsM->getCols());
                    return cmp;
                }
                bool elementwise = binaryOp == BINARY_OP_ADD
                    || binaryOp == BINARY_OP_SUB || binaryOp == BINARY_OP_DIV;
                if (elementwise && rhsM) {
                    if (rhsM->getRows() != lhsM->getRows()
                            || rhsM->getCols() != lhsM->getCols()) {
                        throw Exception(
                            "Matrix element-wise op requires operands of the "
                            "same shape (got " + lhsM->toCanonical() + " and "
                            + rhsM->toCanonical() + ")",
                            "CAJETA_ERROR_MATRIX_SHAPE");
                    }
                    llvm::Value* l = loadIfLValue(module, lhs, lhsAst);
                    llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                    switch (binaryOp) {
                        case BINARY_OP_ADD:
                            resolvedType = lhsM;
                            return isFloat ? builder->CreateFAdd(l, r, "mat.add")
                                           : builder->CreateAdd(l, r, "mat.add");
                        case BINARY_OP_SUB:
                            resolvedType = lhsM;
                            return isFloat ? builder->CreateFSub(l, r, "mat.sub")
                                           : builder->CreateSub(l, r, "mat.sub");
                        case BINARY_OP_DIV:
                            resolvedType = lhsM;
                            return isFloat
                                ? builder->CreateFDiv(l, r, "mat.div")
                                : (isSigned
                                    ? builder->CreateSDiv(l, r, "mat.div")
                                    : builder->CreateUDiv(l, r, "mat.div"));
                        default: break;
                    }
                }
                if (llvm::Value* mv = generateMatrixMul(
                        module, lhs, rhs, lhsAst, rhsAst))
                    return mv;
            }
        }

        // Vector comparisons yield a per-lane mask typed Vector<boolean,N>. The scalar
        // path below mis-reads FP-ness on `<N x T>`, so vectors are handled first.
        if (lhsAst) {
            if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
            if (auto lhsV = dynamic_pointer_cast<CajetaVector>(
                    lhsAst->getResolvedType())) {
                bool isCmp = binaryOp == BINARY_OP_EQ || binaryOp == BINARY_OP_NE
                    || binaryOp == BINARY_OP_LT || binaryOp == BINARY_OP_LE
                    || binaryOp == BINARY_OP_GT || binaryOp == BINARY_OP_GE;
                if (isCmp) {
                    bool isFloat = lhsV->getElementType()->getLlvmType()
                        ->isFloatingPointTy();
                    bool isSigned =
                        (lhsV->getElementType()->getTypeFlags() & SIGNED_FLAG) != 0;
                    llvm::Value* l = loadIfLValue(module, lhs, lhsAst);
                    llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                    if (!r->getType()->isVectorTy()) {
                        auto* vt = llvm::cast<llvm::FixedVectorType>(l->getType());
                        r = vecops::splat(*builder,
                            vecops::coerceScalar(*builder, r, vt->getElementType()),
                            vt->getNumElements());
                    }
                    llvm::Value* cmp = nullptr;
                    switch (binaryOp) {
                        case BINARY_OP_EQ: cmp = isFloat
                            ? builder->CreateFCmpOEQ(l, r, "vec.cmp")
                            : builder->CreateICmpEQ(l, r, "vec.cmp"); break;
                        case BINARY_OP_NE: cmp = isFloat
                            ? builder->CreateFCmpUNE(l, r, "vec.cmp")
                            : builder->CreateICmpNE(l, r, "vec.cmp"); break;
                        case BINARY_OP_LT: cmp = isFloat
                            ? builder->CreateFCmpOLT(l, r, "vec.cmp")
                            : (isSigned ? builder->CreateICmpSLT(l, r, "vec.cmp")
                                        : builder->CreateICmpULT(l, r, "vec.cmp"));
                            break;
                        case BINARY_OP_LE: cmp = isFloat
                            ? builder->CreateFCmpOLE(l, r, "vec.cmp")
                            : (isSigned ? builder->CreateICmpSLE(l, r, "vec.cmp")
                                        : builder->CreateICmpULE(l, r, "vec.cmp"));
                            break;
                        case BINARY_OP_GT: cmp = isFloat
                            ? builder->CreateFCmpOGT(l, r, "vec.cmp")
                            : (isSigned ? builder->CreateICmpSGT(l, r, "vec.cmp")
                                        : builder->CreateICmpUGT(l, r, "vec.cmp"));
                            break;
                        case BINARY_OP_GE: cmp = isFloat
                            ? builder->CreateFCmpOGE(l, r, "vec.cmp")
                            : (isSigned ? builder->CreateICmpSGE(l, r, "vec.cmp")
                                        : builder->CreateICmpUGE(l, r, "vec.cmp"));
                            break;
                        default: break;
                    }
                    resolvedType = CajetaVector::getOrCreate(
                        module, CajetaType::of("boolean"), lhsV->getLanes());
                    return cmp;
                }
            }
        }

        // Quaternion `*` is the Hamilton product or a vector rotation; `+ -` are element-wise.
        if (lhsAst) {
            if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
            if (auto lhsQ = dynamic_pointer_cast<CajetaQuaternion>(
                    lhsAst->getResolvedType())) {
                if (rhsAst && !rhsAst->getResolvedType())
                    rhsAst->resolveTypes(module);
                CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;
                llvm::Value* l = loadIfLValue(module, lhs, lhsAst);
                if (binaryOp == BINARY_OP_MUL) {
                    if (dynamic_pointer_cast<CajetaQuaternion>(rhsType)) {
                        llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                        resolvedType = lhsQ;
                        return quatops::multiply(*builder, l, r);
                    }
                    if (auto rhsV = dynamic_pointer_cast<CajetaVector>(rhsType)) {
                        if (rhsV->getLanes() != 3) {
                            throw Exception(
                                "Quaternion * Vector rotation requires a "
                                "3-component vector (got " + rhsV->toCanonical()
                                + ")", "CAJETA_ERROR_QUATERNION_METHOD");
                        }
                        llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                        resolvedType = CajetaVector::getOrCreate(
                            module, lhsQ->getElementType(), 3);
                        return quatops::rotate(*builder, l, r);
                    }
                }
                if ((binaryOp == BINARY_OP_ADD || binaryOp == BINARY_OP_SUB)
                        && dynamic_pointer_cast<CajetaQuaternion>(rhsType)) {
                    llvm::Value* r = loadIfLValue(module, rhs, rhsAst);
                    resolvedType = lhsQ;
                    return binaryOp == BINARY_OP_ADD
                        ? builder->CreateFAdd(l, r, "quat.add")
                        : builder->CreateFSub(l, r, "quat.sub");
                }
            }
        }

        // Operator overloading: an `operator<sym>` on an operand's class dispatches
        // before the built-in path, resolved through resolveMethod so base-class
        // operators are visible. A null symbol means no overloadable form exists.
        const char* opSym = opdispatch::binaryOpSymbol(binaryOp);
        if (opSym && lhsAst) {
            if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
            auto lhsClass = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
            if (lhsClass && !lhsClass->isInterface()
                    && !(lhsClass->getTypeFlags() & PRIMITIVE_FLAG)) {
                const bool fp = false;
                if (rhsAst && !rhsAst->getResolvedType()) {
                    rhsAst->resolveTypes(module);
                }
                CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;
                if (!rhsType) rhsType = CajetaType::of(rhs);
                CajetaTypePtr lhsType = lhsAst->getResolvedType();
                // resolveMethod's canonical name dereferences each parameter type, so bail out
                // rather than look up with a null one.
                if (!rhsType || !lhsType) {
                    goto fallthrough_to_builtin;
                }
                {
                    llvm::Value* lhsVal = loadIfLValue(module, lhs, lhsAst);
                    llvm::Value* rhsVal = loadIfLValue(module, rhs, rhsAst);
                    // Cajeta binary overloads are STATIC (both operands explicit), so invokeMethod
                    // gets a null receiver; opdispatch derives `!= > >= <=` from the base form.
                    auto tryInvoke = [&](std::string name, bool swap)
                            -> std::pair<bool, llvm::Value*> {
                        vector<ParameterEntry> ents;
                        if (swap) {
                            ents.push_back(ParameterEntry(rhsType, "", rhsVal));
                            ents.push_back(ParameterEntry(lhsType, "", lhsVal));
                        } else {
                            ents.push_back(ParameterEntry(lhsType, "", lhsVal));
                            ents.push_back(ParameterEntry(rhsType, "", rhsVal));
                        }
                        if (!lhsClass->resolveMethod(name, ents,
                                /*isConstructor=*/false, /*floatingParams=*/fp)) {
                            return {false, nullptr};
                        }
                        return {true, lhsClass->invokeMethod(name, ents,
                            /*isConstructor=*/false,
                            /*thisInstance=*/nullptr,
                            /*callerModule=*/module)};
                    };
                    auto negate = [&](llvm::Value* v) -> llvm::Value* {
                        return builder->CreateNot(v,
                            std::string("derived.") + opSym);
                    };
                    std::pair<bool, llvm::Value*> disp =
                        opdispatch::dispatchBinaryOperator(
                            binaryOp, tryInvoke, negate);
                    if (disp.first) {
                        return disp.second;
                    }
                    // An ORDERING comparison between a class and a numeric primitive with no
                    // override would silently compare a POINTER against the number: fail loud.
                    if ((binaryOp == BINARY_OP_LT || binaryOp == BINARY_OP_LE
                            || binaryOp == BINARY_OP_GT
                            || binaryOp == BINARY_OP_GE)
                            && rhsType
                            && (rhsType->getTypeFlags() & PRIMITIVE_FLAG)
                            && (rhsType->getTypeFlags() & NUMBER_FLAG)) {
                        throw Exception(
                            "no 'operator" + std::string(opSym) + "' on '"
                                + lhsClass->getQName()->toCanonical()
                                + "' accepts a '"
                                + (rhsType ? rhsType->toCanonical()
                                           : std::string("?"))
                                + "' operand — ordering comparisons on class "
                                  "types require an operator override "
                                  "(pointer ordering is not a comparison)",
                            "CAJETA_ERROR_NO_MATCHING_OVERLOAD",
                            "", getSourceLine(), getSourceColumn());
                    }
                }
            }
        }
        fallthrough_to_builtin:;

        // Signedness cannot be recovered from an llvm::Value: getTypeFlagsOf keys on
        // llvm::Type::TypeID, where every integer width shares IntegerTyID. Ask the
        // AST's resolved type first, and never force resolution from inside codegen.
        auto typeFlagsFor = [&](const ExpressionPtr& ast,
                                llvm::Value* v) -> long {
            if (ast) {
                if (CajetaTypePtr rt = ast->getResolvedType()) {
                    if ((rt->getTypeFlags() & PRIMITIVE_FLAG) != 0) {
                        return (long) rt->getTypeFlags();
                    }
                }
                // A bare identifier usually has no resolved type here: read the DECLARED type
                // out of the scope, and a cast's target type from getDestType.
                if (auto cast = dynamic_pointer_cast<CastExpression>(ast)) {
                    if (CajetaTypePtr dt = cast->getDestType()) {
                        if ((dt->getTypeFlags() & PRIMITIVE_FLAG) != 0) {
                            return (long) dt->getTypeFlags();
                        }
                    }
                }
                if (auto id = dynamic_pointer_cast<IdentifierExpression>(ast)) {
                    if (auto sc = module->getScopeStack().peek()) {
                        if (FieldPtr f = sc->getField(id->getTextValue())) {
                            if (CajetaTypePtr ft = f->getType()) {
                                if ((ft->getTypeFlags() & PRIMITIVE_FLAG) != 0) {
                                    return (long) ft->getTypeFlags();
                                }
                            }
                        }
                    }
                }
            }
            return (long) CajetaType::getTypeFlagsOf(v);
        };
        long lhsTypeFlags = typeFlagsFor(lhsAst, lhs);
        long rhsTypeFlags = typeFlagsFor(rhsAst, rhs);

        // Tri-state: 1 signed, 0 unsigned, -1 undetermined. The value-derived fallback
        // reports SIGNED_FLAG clear, so an operand only votes when its type is known.
        auto knownSignedness = [&](const ExpressionPtr& ast) -> int {
            if (!ast) return -1;
            auto fromType = [](const CajetaTypePtr& t) -> int {
                if (!t) return -1;
                if ((t->getTypeFlags() & PRIMITIVE_FLAG) == 0) return -1;
                return (t->getTypeFlags() & SIGNED_FLAG) != 0 ? 1 : 0;
            };
            if (CajetaTypePtr rt = ast->getResolvedType()) {
                int r = fromType(rt);
                if (r >= 0) return r;
            }
            if (auto cast = dynamic_pointer_cast<CastExpression>(ast)) {
                int r = fromType(cast->getDestType());
                if (r >= 0) return r;
            }
            if (auto id = dynamic_pointer_cast<IdentifierExpression>(ast)) {
                if (auto sc = module->getScopeStack().peek()) {
                    if (FieldPtr f = sc->getField(id->getTextValue())) {
                        int r = fromType(f->getType());
                        if (r >= 0) return r;
                    }
                }
            }
            // A bare integer literal is a SIGNED int by default.
            if (dynamic_pointer_cast<IntegerLiteralExpression>(ast)) return 1;
            return -1;
        };
        const int lhsSignKnown = knownSignedness(lhsAst);
        const int rhsSignKnown = knownSignedness(rhsAst);

        // Usual arithmetic conversions at equal rank: UNSIGNED WINS, coerceArithPair
        // having settled width. With neither operand determined, keep the flag-OR.
        const bool signednessDetermined =
            (lhsSignKnown >= 0) || (rhsSignKnown >= 0);
        const bool intOpIsSigned = signednessDetermined
            ? !(lhsSignKnown == 0 || rhsSignKnown == 0)
            : (((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0);

        llvm::Value* result = nullptr;

        auto loadL = [&](llvm::Value* v) { return loadIfLValue(module, v, lhsAst); };
        auto loadR = [&](llvm::Value* v) { return loadIfLValue(module, v, rhsAst); };
        (void) loadL; (void) loadR;

        // Non-assignment ops need both sides as r-values; assignment forms coerce rhs only.
        switch (binaryOp) {
            case BINARY_OP_ASSIGN: {
                // A record field only initializes inside the record's own constructor; statics
                // stay assignable, and a class field that merely holds a record is unaffected.
                if (auto recDotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    auto& rch = recDotLhs->getChildren();
                    auto recv = rch.empty() ? nullptr
                        : dynamic_pointer_cast<Expression>(rch[0]);
                    if (recv) {
                        if (!recv->getResolvedType()) recv->resolveTypes(module);
                        auto recvClass = dynamic_pointer_cast<CajetaClass>(
                            recv->getResolvedType());
                        if (recvClass && recvClass->isRecordType()) {
                            bool ctorSelfInit = false;
                            if (dynamic_pointer_cast<ThisExpression>(recv)) {
                                auto cur = module->getCurrentMethod();
                                ctorSelfInit = cur && cur->isConstructor()
                                    && cur->getParent().get() == recvClass.get();
                            }
                            StructurePropertyPtr found;
                            std::function<bool(const CajetaClassPtr&)> findP =
                                [&](const CajetaClassPtr& cls) -> bool {
                                    if (!cls) return false;
                                    auto pit = cls->getProperties().find(
                                        recDotLhs->getIdentifier());
                                    if (pit != cls->getProperties().end()) {
                                        found = pit->second;
                                        return true;
                                    }
                                    for (auto& sup : cls->getSuperClasses()) {
                                        if (findP(sup)) return true;
                                    }
                                    return false;
                                };
                            findP(recvClass);
                            bool staticField = found && found->isStatic();
                            bool mutField = found
                                && found->getModifiers().count(MUT);
                            if (!ctorSelfInit && !staticField && !mutField) {
                                throw Exception(
                                    "cannot assign to immutable field '"
                                        + recDotLhs->getIdentifier()
                                        + "' of record '"
                                        + recvClass->getQName()->toCanonical()
                                        + "' — records are immutable; use "
                                          "with(" + recDotLhs->getIdentifier()
                                        + ": value) to produce an updated copy",
                                    "CAJETA_ERROR_RECORD_IMMUTABLE");
                            }
                        }
                    }
                }
                // `m[r][c] = e` writes directly into m's slot at flat lane r*C+c: the row m[r]
                // is a fresh value, not an l-value. Must run before the vector path below.
                if (auto outer = dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    auto& oc = outer->getChildren();
                    if (oc.size() >= 2) {
                        auto inner = dynamic_pointer_cast<ArrayIndexExpression>(
                            dynamic_pointer_cast<Expression>(oc[0]));
                        if (inner && inner->getChildren().size() >= 2) {
                            auto& ic = inner->getChildren();
                            auto mBase = dynamic_pointer_cast<Expression>(ic[0]);
                            if (mBase && !mBase->getResolvedType())
                                mBase->resolveTypes(module);
                            if (auto matT = dynamic_pointer_cast<CajetaMatrix>(
                                    mBase ? mBase->getResolvedType() : nullptr)) {
                                llvm::Type* i32Ty = llvm::Type::getInt32Ty(
                                    *module->getLlvmContext());
                                auto toI32 = [&](llvm::Value* v) {
                                    return v->getType() == i32Ty ? v
                                        : builder->CreateIntCast(v, i32Ty, false,
                                                                 "mat.idx");
                                };
                                llvm::Value* slot = mBase->generateCode(module);
                                llvm::Value* rIdx = toI32(loadIfLValue(module,
                                    ic[1]->generateCode(module),
                                    dynamic_pointer_cast<Expression>(ic[1])));
                                llvm::Value* cIdx = toI32(loadIfLValue(module,
                                    oc[1]->generateCode(module),
                                    dynamic_pointer_cast<Expression>(oc[1])));
                                llvm::Type* matLlvm = matT->getLlvmType();
                                llvm::Value* cur = builder->CreateLoad(
                                    matLlvm, slot, "mat.cur");
                                llvm::Value* rv = vecops::coerceScalar(*builder,
                                    loadR(rhs),
                                    matT->getElementType()->getLlvmType());
                                llvm::Value* nv = matops::setElement(*builder, cur,
                                    matT->getRows(), matT->getCols(), rIdx, cIdx, rv);
                                builder->CreateStore(nv, slot);
                                result = nv;
                                break;
                            }
                        }
                    }
                }
                // `v.x = e` / `v[i] = e`: load the `<N x T>` from the base l-value, insert the
                // lane, store back. lhsAst's own codegen yields the element, not a slot.
                {
                    ExpressionPtr vbase;
                    CajetaVectorPtr vvec;
                    llvm::Value* vlane = nullptr;
                    if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                        auto& ch = dotLhs->getChildren();
                        if (!ch.empty()) {
                            if (auto be = dynamic_pointer_cast<Expression>(ch[0])) {
                                if (!be->getResolvedType()) be->resolveTypes(module);
                                if (auto vt = dynamic_pointer_cast<CajetaVector>(
                                        be->getResolvedType())) {
                                    int lane = vecops::laneForComponentName(
                                        dotLhs->getIdentifier());
                                    if (lane < 0 || (unsigned) lane >= vt->getLanes()) {
                                        throw Exception(
                                            "component '." + dotLhs->getIdentifier()
                                            + "' is out of range for Vector<...,"
                                            + std::to_string(vt->getLanes()) + ">",
                                            "CAJETA_ERROR_VECTOR_COMPONENT");
                                    }
                                    vbase = be; vvec = vt;
                                    vlane = builder->getInt32((unsigned) lane);
                                }
                            }
                        }
                    } else if (auto arrLhs =
                            dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                        auto& ch = arrLhs->getChildren();
                        if (ch.size() >= 2) {
                            if (auto be = dynamic_pointer_cast<Expression>(ch[0])) {
                                if (!be->getResolvedType()) be->resolveTypes(module);
                                if (auto vt = dynamic_pointer_cast<CajetaVector>(
                                        be->getResolvedType())) {
                                    vbase = be; vvec = vt;
                                    auto ie = dynamic_pointer_cast<Expression>(ch[1]);
                                    vlane = loadIfLValue(
                                        module, ch[1]->generateCode(module), ie);
                                }
                            }
                        }
                    }
                    if (vbase && vvec) {
                        llvm::Value* slot = vbase->generateCode(module);
                        llvm::Type* vecLlvm = vvec->getLlvmType();
                        llvm::Value* cur = builder->CreateLoad(vecLlvm, slot,
                                                               "vec.cur");
                        llvm::Value* rv = vecops::coerceScalar(*builder,
                            loadR(rhs),
                            vvec->getElementType()->getLlvmType());
                        llvm::Value* nv = builder->CreateInsertElement(
                            cur, rv, vlane, "vec.set");
                        builder->CreateStore(nv, slot);
                        result = nv;
                        break;
                    }
                }
                // Struct-to-struct assignment memcpys by allocation size. STRUCT_FLAG is the
                // discriminator; STRUCT_TYPE_ID is a composite id and false-positives under `&`.
                if ((lhsTypeFlags & STRUCT_FLAG) && (rhsTypeFlags & STRUCT_FLAG)) {
                    llvm::Type* structTy = nullptr;
                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                        structTy = a->getAllocatedType();
                    } else if (lhsAst && lhsAst->getResolvedType()) {
                        structTy = lhsAst->getResolvedType()->getLlvmType();
                    }
                    if (structTy) {
                        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                        llvm::Value* size = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(*module->getLlvmContext()),
                            dl.getTypeAllocSize(structTy));
                        llvm::Align align(dl.getABITypeAlign(structTy));
                        builder->CreateMemCpy(lhs, align, rhs, align, size);
                        // Struct assignment yields the destination address (lvalue convention).
                        result = lhs;
                        break;
                    }
                }
                // A value-type field or element stores its body INLINE while the RHS is usually
                // an address: copy the body, or the pointer bits land on the first field.
                if (dynamic_pointer_cast<DotExpression>(lhsAst)
                        || dynamic_pointer_cast<IdentifierExpression>(lhsAst)
                        || dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsCls = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
                    // Only an inline value slot takes the copy path; a ptr-typed slot is a reference.
                    bool identInlineSlot = true;
                    if (dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                        auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(lhs);
                        identInlineSlot = a && !a->getAllocatedType()->isPointerTy();
                    }
                    if (lhsCls && lhsCls->isValueType()
                            && identInlineSlot
                            && !lhsCls->isInterface()
                            && !dynamic_pointer_cast<CajetaView>(lhsAst->getResolvedType())) {
                        if (rhsAst) {
                            if (!rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                            auto rhsCls = dynamic_pointer_cast<CajetaClass>(
                                rhsAst->getResolvedType());
                            if (rhsCls && rhsCls.get() != lhsCls.get()
                                    && (rhsCls->isRecordType() || lhsCls->isRecordType())) {
                                throw Exception(
                                    "cannot implicitly convert '"
                                        + rhsCls->toCanonical() + "' to '"
                                        + lhsCls->toCanonical()
                                        + "' — record upcasts slice; write an "
                                          "explicit cast: ("
                                        + lhsCls->getQName()->getTypeName()
                                        + ") value",
                                    "CAJETA_ERROR_RECORD_IMPLICIT_CAST");
                            }
                        }
                        llvm::Type* bodyTy = lhsCls->getLlvmType();
                        if (bodyTy && bodyTy->isStructTy()) {
                            // Overwriting a shared-capable value releases the OLD value's stakes; a copy
                            // from an lvalue then retains the new ones, while an rvalue carries its stakes
                            // with the bytes. Self-assign skips the pair: release could free at rc == 1.
                            bool hooked = lhsCls->isSharedCapableValue();
                            bool selfAssign = false;
                            if (hooked) {
                                auto lId = dynamic_pointer_cast<IdentifierExpression>(lhsAst);
                                auto rId = dynamic_pointer_cast<IdentifierExpression>(rhsAst);
                                selfAssign = lId && rId
                                    && lId->getTextValue() == rId->getTextValue();
                            }
                            bool rhsLvalue = rhsAst
                                && (dynamic_pointer_cast<IdentifierExpression>(rhsAst)
                                    || dynamic_pointer_cast<DotExpression>(rhsAst)
                                    || dynamic_pointer_cast<ArrayIndexExpression>(rhsAst)
                                    || dynamic_pointer_cast<CastExpression>(rhsAst));
                            llvm::Module* hookModule =
                                builder->GetInsertBlock()->getModule();
                            if (hooked && !selfAssign) {
                                lhsCls->emitValueSharedOp(*builder, lhs, module,
                                    hookModule, /*retain=*/false);
                            }
                            if (rhs->getType() == bodyTy) {
                                builder->CreateStore(rhs, lhs);
                            } else if (rhs->getType()->isPointerTy()) {
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Value* size = llvm::ConstantInt::get(
                                    llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                    dl.getTypeAllocSize(bodyTy));
                                llvm::Align align(dl.getABITypeAlign(bodyTy));
                                builder->CreateMemCpy(lhs, align, rhs, align, size);
                            } else {
                                builder->CreateStore(rhs, lhs);
                            }
                            // Slice<T> FIELD stores resolve in place: arena or <= 256 B payloads copy into a
                            // fresh field-owned root, larger windows take a stake. Locals stay borrows.
                            bool lhsIsSliceField = hooked && !selfAssign
                                && dynamic_pointer_cast<DotExpression>(lhsAst)
                                && lhsCls->getQName()
                                && lhsCls->getQName()->getPackageName() == "cajeta.lang"
                                && lhsCls->getQName()->getTypeName().rfind("Slice", 0) == 0;
                            if (lhsIsSliceField) {
                                int64_t elemSize = 8;
                                auto& props = lhsCls->getProperties();
                                auto pit = props.find("store");
                                if (pit != props.end()) {
                                    if (auto arrT = dynamic_pointer_cast<CajetaArray>(
                                            pit->second->getType())) {
                                        if (auto et = arrT->getElementType()) {
                                            if (llvm::Type* lt = et->getLlvmType()) {
                                                elemSize = (int64_t) module
                                                    ->getLlvmModule()->getDataLayout()
                                                    .getTypeAllocSize(lt);
                                            }
                                        }
                                    }
                                }
                                if (llvm::Function* resolveSliceFn =
                                        module->getRuntimeFunction("__cajeta_slice_resolve")) {
                                    builder->CreateCall(resolveSliceFn, {lhs,
                                        llvm::ConstantInt::get(
                                            llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                            (uint64_t) elemSize)});
                                }
                            } else if (hooked && !selfAssign && rhsLvalue) {
                                lhsCls->emitValueSharedOp(*builder, lhs, module,
                                    hookModule, /*retain=*/true);
                            }
                            result = lhs;
                            break;
                        }
                    }
                }
                // An interface FIELD holds the 24-byte body inline, while an interface VALUE is
                // a pointer to such a body: memcpy the body, or the field's vtable/kind stay
                // zero. Gated on a DotExpression, since interface locals are pointer slots.
                if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsCls = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
                    if (lhsCls && lhsCls->isInterface()) {
                        llvm::Type* ifaceTy = lhsAst->getResolvedType()->getLlvmType();
                        if (ifaceTy && ifaceTy->isStructTy()) {
                            storeInterfaceInlineBody(module, lhs, loadR(rhs),
                                lhsCls, rhsAst);
                            result = lhs;
                            break;
                        }
                    }
                }
                // A plain `obj.f = w` from a scope-owned String LVALUE would store the borrowed
                // wrapper and dangle when the declaring scope frees it: store a FRESH resolved
                // wrapper instead. Rvalue and `#`-move sources transfer as before.
                if (auto dotLhs2 = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    if (lhsAst && !lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsCls2 = dynamic_pointer_cast<CajetaClass>(
                        lhsAst->getResolvedType());
                    bool lhsIsString = lhsCls2 && lhsCls2->getQName()
                        && lhsCls2->getQName()->getTypeName() == "String"
                        && lhsCls2->getQName()->getPackageName() == "cajeta.lang";
                    if (lhsIsString && rhsAst) {
                        if (!rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                        auto rhsCls2 = dynamic_pointer_cast<CajetaClass>(
                            rhsAst->getResolvedType());
                        bool rhsIsString = rhsCls2 && rhsCls2->getQName()
                            && rhsCls2->getQName()->getTypeName() == "String"
                            && rhsCls2->getQName()->getPackageName() == "cajeta.lang";
                        // A string literal is a String even when its resolvedType is a placeholder.
                        if (!rhsIsString) {
                            if (auto lit = dynamic_pointer_cast<TextLiteralExpression>(rhsAst)) {
                                LiteralType lt = lit->getLiteralType();
                                rhsIsString = (lt == LITERAL_TYPE_STRING
                                               || lt == LITERAL_TYPE_TEXT_BLOCK);
                            }
                        }
                        bool rhsIsLvalue =
                            dynamic_pointer_cast<IdentifierExpression>(rhsAst)
                            || dynamic_pointer_cast<DotExpression>(rhsAst)
                            || dynamic_pointer_cast<ArrayIndexExpression>(rhsAst);
                        int maskBit = -1;
                        if (rhsIsLvalue) {
                            if (auto idRhs = dynamic_pointer_cast<
                                    IdentifierExpression>(rhsAst)) {
                                if (auto sc = module->getScopeStack().peek()) {
                                    FieldPtr srcField =
                                        sc->getField(idRhs->getTextValue());
                                    if (auto pf = dynamic_pointer_cast<
                                            ParameterField>(srcField)) {
                                        auto fp = pf->getFormalParameter();
                                        if (fp && fp->isTransferred()) {
                                            rhsIsLvalue = false;
                                        } else if (fp) {
                                            if (auto cm = module->getCurrentMethod()) {
                                                int idx = 0;
                                                for (auto& p : cm->getParameterList()) {
                                                    if (!p || p->getName() == "this") continue;
                                                    if (p->getName() == fp->getName()) {
                                                        maskBit = idx;
                                                        break;
                                                    }
                                                    idx++;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (rhsIsString) {
                            llvm::Value* mvBit = nullptr;
                            // fldResolve: the field takes its own copy; otherwise a static owner is adopted.
                            bool fldResolve = false;
                            {
                                llvm::Value* ft = ownership::storeTitleFlag(
                                    rhsAst, ownership::ConsumerRole::StoreString,
                                    module, "a String field");
                                if (!ft) {
                                    fldResolve = rhsAst->kind() != ExprKind::TextLiteral;
                                } else if (!llvm::isa<llvm::ConstantInt>(ft)) {
                                    mvBit = ft;
                                } else if (llvm::cast<llvm::ConstantInt>(ft)->isZero()) {
                                    fldResolve = true;
                                }
                            }
                            llvm::Function* resolveFn = (fldResolve || mvBit)
                                ? module->getRuntimeFunction("__cajeta_string_resolve")
                                : nullptr;
                            llvm::Value* maskWord = nullptr;
                            if (rhsIsLvalue && resolveFn && maskBit >= 0) {
                                if (auto cmw = module->getCurrentMethod()) {
                                    maskWord = cmw->getTransferWordArg();
                                }
                            }
                            llvm::Value* srcWrapper = loadR(rhs);
                            llvm::Value* fresh = nullptr;
                            if (maskWord || (mvBit && resolveFn)) {
                                auto& lctx = *module->getLlvmContext();
                                llvm::Value* bit = maskWord
                                    ? builder->CreateAnd(
                                          builder->CreateLShr(maskWord, maskBit),
                                          llvm::ConstantInt::get(
                                              llvm::Type::getInt64Ty(lctx), 1))
                                    : mvBit;
                                llvm::Value* isMove = builder->CreateICmpNE(
                                    bit, llvm::ConstantInt::get(
                                        bit->getType(), 0),
                                    "fld_is_move");
                                llvm::Function* fnOwner =
                                    builder->GetInsertBlock()->getParent();
                                auto* bbResolve = llvm::BasicBlock::Create(
                                    lctx, "fld_resolve", fnOwner);
                                auto* bbStore = llvm::BasicBlock::Create(
                                    lctx, "fld_store", fnOwner);
                                auto* bbFrom = builder->GetInsertBlock();
                                builder->CreateCondBr(isMove, bbStore, bbResolve);
                                builder->SetInsertPoint(bbResolve);
                                llvm::Value* resolved = builder->CreateCall(
                                    resolveFn, {srcWrapper}, "fld_resolved");
                                builder->CreateBr(bbStore);
                                builder->SetInsertPoint(bbStore);
                                llvm::PHINode* phi = builder->CreatePHI(
                                    llvm::PointerType::get(lctx, 0), 2, "fld_fresh");
                                phi->addIncoming(srcWrapper, bbFrom);
                                phi->addIncoming(resolved, bbResolve);
                                fresh = phi;
                            } else {
                                fresh = (fldResolve && resolveFn)
                                    ? builder->CreateCall(resolveFn, {srcWrapper},
                                                          "str_resolve")
                                    : srcWrapper;
                            }
                            bool inStdlibClass = false;
                            if (!module->getStructureStack().empty()) {
                                auto owner = module->getStructureStack().back();
                                if (owner && owner->getQName()) {
                                    const std::string& pkg =
                                        owner->getQName()->getPackageName();
                                    inStdlibClass = pkg == "cajeta"
                                        || pkg.rfind("cajeta.", 0) == 0;
                                }
                            }
                            if (rhsIsLvalue && resolveFn && !inStdlibClass) {
                                if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
                                    eng->report("note", "CAJETA_LINT_SLICE_RESOLVED",
                                        "String store resolves silently (copies when "
                                        "<= 256 B or arena/SSO-backed, otherwise takes "
                                        "a shared stake); a `#` transfer of the source "
                                        "would make this store free",
                                        module->getSourcePath(),
                                        (int) getSourceLine(), -1);
                                }
                            }
                            // Skipped inside constructors: a stack class body is not zero-initialized, so
                            // the first store would read garbage as the old wrapper.
                            bool inCtor = false;
                            if (auto cm = module->getCurrentMethod()) {
                                inCtor = cm->isConstructor();
                            }
                            if (!inCtor) {
                                llvm::Value* oldVal = builder->CreateLoad(
                                    llvm::PointerType::get(*module->getLlvmContext(), 0),
                                    lhs, "str_old");
                                if (llvm::Function* dropFn =
                                        module->getRuntimeFunction("__cajeta_string_drop")) {
                                    builder->CreateCall(dropFn, {oldVal});
                                }
                            }
                            builder->CreateStore(fresh, lhs);
                            // This path breaks out of the assign switch before the general field-ownership
                            // block below, so it records the title bit itself.
                            {
                                llvm::Value* strWordPtr = nullptr;
                                int strBitIdx = -1;
                                if (locateFieldOwnershipBit(
                                        module, builder, dotLhs2, lhs,
                                        &strWordPtr, &strBitIdx)) {
                                    auto& sctx = *module->getLlvmContext();
                                    llvm::Type* i64Ty =
                                        llvm::Type::getInt64Ty(sctx);
                                    llvm::Value* w = builder->CreateLoad(
                                        i64Ty, strWordPtr, "own_bits");
                                    w = builder->CreateOr(w,
                                        llvm::ConstantInt::get(
                                            i64Ty, 1ULL << strBitIdx));
                                    builder->CreateStore(w, strWordPtr);
                                }
                            }
                            result = fresh;
                            break;
                        }
                    }
                }
                llvm::Value* rhsVal = loadR(rhs);
                // Coerce rhs to the destination's element type: without the GEP path a wide
                // literal (`xs[0] = 10`, i64) writes 8 bytes into a 4-byte slot.
                llvm::Type* slotTy = nullptr;
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                    slotTy = a->getAllocatedType();
                } else if (lhsAst && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (auto elemType = lhsAst->getResolvedType()) {
                        if (dynamic_pointer_cast<CajetaClass>(elemType) ||
                            (elemType->getTypeFlags() & STRUCT_FLAG)) {
                            slotTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                        } else if (llvm::Type* lt = elemType->getLlvmType()) {
                            slotTy = lt;
                        }
                    }
                } else if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    // Walk the inheritance chain: an inherited field lives on an ancestor's map, and
                    // a null slotTy lets a wide literal store past the slot's width.
                    if (!dotLhs->getChildren().empty()) {
                        auto recv = dynamic_pointer_cast<Expression>(dotLhs->getChildren()[0]);
                        if (recv) {
                            if (!recv->getResolvedType()) recv->resolveTypes(module);
                            if (auto klass = dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())) {
                                StructurePropertyPtr found;
                                std::function<bool(const CajetaClassPtr&)> findProp =
                                    [&](const CajetaClassPtr& cls) -> bool {
                                        auto pit = cls->getProperties().find(dotLhs->getIdentifier());
                                        if (pit != cls->getProperties().end()) {
                                            found = pit->second;
                                            return true;
                                        }
                                        for (auto& parent : cls->getSuperClasses()) {
                                            if (findProp(parent)) return true;
                                        }
                                        return false;
                                    };
                                if (findProp(klass)) {
                                    // Variable-size view fields cannot be resized in place (Views.md).
                                    bool isView =
                                        dynamic_pointer_cast<CajetaView>(klass) != nullptr;
                                    if (isView
                                            && CajetaView::isVariableSize(found)) {
                                        char buf[256];
                                        snprintf(buf, sizeof(buf),
                                            "cannot reassign variable-size view field '%s'; "
                                            "build a new buffer instead",
                                            found->getName().c_str());
                                        throw Exception(buf,
                                            "CAJETA_ERROR_VARSIZE_FIELD_ASSIGN");
                                    }
                                    // Arrays and plain class refs are stored as `ptr` in the class layout, so slotTy
                                    // must be `ptr`; views and interfaces keep their inline storage.
                                    auto foundCls = dynamic_pointer_cast<CajetaClass>(found->getType());
                                    bool foundIsView = dynamic_pointer_cast<CajetaView>(found->getType()) != nullptr;
                                    bool foundIsArray = dynamic_pointer_cast<CajetaArray>(found->getType()) != nullptr;
                                    bool foundIsInterface = foundCls && foundCls->isInterface();
                                    if (foundIsArray
                                            || (foundCls && !foundIsView && !foundIsInterface)) {
                                        slotTy = llvm::PointerType::get(
                                            *module->getLlvmContext(), 0);
                                    } else {
                                        slotTy = found->getType()->getLlvmType();
                                    }
                                }
                            }
                        }
                    }
                }
                // A struct field whose struct declares a non-host endianness is byte-swapped
                // after the width coercion below, so the buffer holds the declared order.
                bool needsFieldBswap = false;
                ExpressionPtr dotRecv;
                if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    if (!dotLhs->getChildren().empty()) {
                        dotRecv = dynamic_pointer_cast<Expression>(dotLhs->getChildren()[0]);
                        needsFieldBswap = (dotRecv != nullptr);
                    }
                }
                if (slotTy && rhsVal->getType() != slotTy) {
                    if (slotTy->isIntegerTy() && rhsVal->getType()->isIntegerTy()) {
                        rhsVal = builder->CreateIntCast(rhsVal, slotTy, /*isSigned=*/true);
                    } else if (slotTy->isFloatingPointTy() && rhsVal->getType()->isFloatingPointTy()) {
                        rhsVal = builder->CreateFPCast(rhsVal, slotTy);
                    } else if (slotTy->isFloatingPointTy() && rhsVal->getType()->isIntegerTy()) {
                        rhsVal = builder->CreateSIToFP(rhsVal, slotTy);
                    } else if (slotTy->isIntegerTy() && rhsVal->getType()->isFloatingPointTy()) {
                        rhsVal = builder->CreateFPToSI(rhsVal, slotTy);
                    }
                }
                if (needsFieldBswap) {
                    rhsVal = DotExpression::maybeBswap(module, rhsVal, dotRecv);
                }
                // Non-first-parent upcast: shift the stored pointer to the ancestor sub-object
                // so dispatch through the LHS-typed binding finds the right secondary vtable.
                if (lhsAst && rhsAst) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    if (!rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                    auto dstClass = dynamic_pointer_cast<CajetaClass>(
                        lhsAst->getResolvedType());
                    auto srcClass = dynamic_pointer_cast<CajetaClass>(
                        rhsAst->getResolvedType());
                    if (dstClass && srcClass
                            && dstClass.get() != srcClass.get()
                            && !dstClass->isInterface()
                            && !srcClass->isInterface()) {
                        rhsVal = CajetaClass::adjustForUpcast(
                            module, rhsVal, srcClass, dstClass);
                    }
                }
                // A store into a bit-carrying field records its title in the declaring class's
                // hidden ownership word (a FIXED byte delta from the field slot) and releases
                // a displaced owned value BEFORE the store; the bit itself lands after it.
                llvm::Value* fobWordPtr = nullptr;
                int fobBitIdx = -1;
                bool fobOwnedSpelling = false;
                bool fobFieldIsArray = false;
                llvm::Value* fobRuntimeFlag = nullptr;
                bool fobFieldIsClosure = false;
                llvm::Value* fobSameObj = nullptr;
                if (auto fobDot = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    StructurePropertyPtr fobProp;
                    if (locateFieldOwnershipBit(module, builder, fobDot, lhs,
                                                &fobWordPtr, &fobBitIdx,
                                                &fobProp)) {
                        {
                            auto& fobCtx = *module->getLlvmContext();
                            llvm::Type* i64Ty = llvm::Type::getInt64Ty(fobCtx);
                            fobFieldIsArray = (bool) dynamic_pointer_cast<
                                CajetaArray>(fobProp->getType());
                            fobFieldIsClosure = (bool) dynamic_pointer_cast<
                                CajetaFunctionType>(fobProp->getType());
                            if (llvm::Value* fobTitle = ownership::storeTitleFlag(
                                    rhsAst, ownership::ConsumerRole::StoreSlot,
                                    module, "a class field")) {
                                fobOwnedSpelling = true;
                                if (!llvm::isa<llvm::ConstantInt>(fobTitle)) {
                                    fobRuntimeFlag = fobTitle;
                                } else if (llvm::cast<llvm::ConstantInt>(fobTitle)->isZero()) {
                                    fobOwnedSpelling = false;
                                }
                            }
                            llvm::PointerType* ptrTy =
                                llvm::PointerType::get(fobCtx, 0);
                            llvm::Value* oldWord = builder->CreateLoad(
                                i64Ty, fobWordPtr, "own_bits_old");
                            llvm::Value* oldBit = builder->CreateAnd(
                                builder->CreateLShr(oldWord,
                                    llvm::ConstantInt::get(i64Ty, fobBitIdx)),
                                llvm::ConstantInt::get(i64Ty, 1));
                            llvm::Value* oldVal = builder->CreateLoad(
                                ptrTy, lhs, "own_old_val");
                            llvm::Value* wasOwned = builder->CreateICmpNE(
                                oldBit, llvm::ConstantInt::get(i64Ty, 0));
                            bool fobRhsCanAlias = true;
                            {
                                ownership::TitleShape rs = ownership::classify(rhsAst, module);
                                if (rs.family == ownership::TitleFamily::Fresh
                                        || rs.family == ownership::TitleFamily::Concat
                                        || rs.family == ownership::TitleFamily::Closure
                                        || (rs.family == ownership::TitleFamily::CallResult
                                            && rs.answer == ownership::TitleAnswer::Owned)) {
                                    fobRhsCanAlias = false;
                                }
                            }
                            if (fobRhsCanAlias && rhsVal && rhsVal->getType()->isPointerTy()) {
                                fobSameObj = builder->CreateICmpEQ(
                                    oldVal, rhsVal, "own_same_obj");
                                wasOwned = builder->CreateAnd(wasOwned,
                                    builder->CreateNot(fobSameObj), "own_displaced");
                            }
                            llvm::Function* relFn = module->getRuntimeFunction(
                                fobFieldIsArray ? "__cajeta_free_array"
                                : fobFieldIsClosure ? "__cajeta_closure_drop"
                                                    : "__cajeta_class_virtual_drop");
                            if (relFn) {
                                llvm::Function* fobFn =
                                    builder->GetInsertBlock()->getParent();
                                llvm::BasicBlock* relBB =
                                    llvm::BasicBlock::Create(fobCtx,
                                        "own_displace", fobFn);
                                llvm::BasicBlock* contBB =
                                    llvm::BasicBlock::Create(fobCtx,
                                        "own_displace_cont", fobFn);
                                builder->CreateCondBr(wasOwned, relBB, contBB);
                                builder->SetInsertPoint(relBB);
                                if (fobFieldIsArray) {
                                    auto fobArr = dynamic_pointer_cast<
                                        CajetaArray>(fobProp->getType());
                                    if (fobArr) {
                                        const llvm::DataLayout& fobADl =
                                            module->getLlvmModule()
                                                ->getDataLayout();
                                        uint64_t fobHs = fobADl
                                            .getTypeAllocSize(
                                                fobArr->getLlvmType());
                                        if (CajetaClass::
                                                arrayElementCarriesSlotBits(
                                                    fobArr->getElementType())) {
                                            if (llvm::Function* fobWalk =
                                                    module->getRuntimeFunction(
                                                        "__cajeta_tail_elem_drop_walk")) {
                                                builder->CreateCall(fobWalk,
                                                    {oldVal,
                                                     llvm::ConstantInt::get(
                                                         i64Ty, fobHs),
                                                     llvm::ConstantInt::get(
                                                         i64Ty,
                                                         fobArr->elementStrideBytes(
                                                             fobADl,
                                                             module->getLlvmContext()))});
                                            }
                                        } else if (CajetaClass::
                                                arrayElementCarriesArraySlotBits(
                                                    fobArr->getElementType())) {
                                            if (llvm::Function* fobAw =
                                                    module->getRuntimeFunction(
                                                        "__cajeta_tail_arrelem_drop_walk")) {
                                                builder->CreateCall(fobAw,
                                                    {oldVal,
                                                     llvm::ConstantInt::get(
                                                         i64Ty, fobHs),
                                                     llvm::ConstantInt::get(
                                                         i64Ty,
                                                         fobArr->elementStrideBytes(
                                                             fobADl,
                                                             module->getLlvmContext())),
                                                     llvm::ConstantInt::get(
                                                         i64Ty,
                                                         CajetaClass::arrayElementInnerDropKind(
                                                             fobArr->getElementType()))});
                                            }
                                        } else if ([&]{
                                            auto sc = dynamic_pointer_cast<
                                                CajetaClass>(
                                                    fobArr->getElementType());
                                            return sc && sc->getQName()
                                                && sc->getQName()->getTypeName()
                                                       == "String"
                                                && sc->getQName()
                                                       ->getPackageName()
                                                       == "cajeta.lang";
                                        }()) {
                                            if (llvm::Function* fobSw =
                                                    module->getRuntimeFunction(
                                                        "__cajeta_string_elem_drop_walk")) {
                                                builder->CreateCall(fobSw,
                                                    {oldVal,
                                                     llvm::ConstantInt::get(
                                                         i64Ty, fobHs),
                                                     llvm::ConstantInt::get(
                                                         i64Ty,
                                                         fobArr->elementStrideBytes(
                                                             fobADl,
                                                             module->getLlvmContext()))});
                                            }
                                        } else if (CajetaClass::
                                                arrayElementCarriesMemberBits(
                                                    fobArr->getElementType())) {
                                            if (llvm::Function* fobMw =
                                                    CajetaClass::getOrCreateMemberWalk(
                                                        module,
                                                        builder->GetInsertBlock()
                                                            ->getParent()->getParent(),
                                                        dynamic_pointer_cast<CajetaClass>(
                                                            fobArr->getElementType()),
                                                        fobHs,
                                                        fobArr->elementStrideBytes(
                                                            fobADl,
                                                            module->getLlvmContext()),
                                                        /*withFree=*/false)) {
                                                builder->CreateCall(fobMw,
                                                                    {oldVal});
                                            }
                                        }
                                    }
                                }
                                builder->CreateCall(relFn, {oldVal});
                                builder->CreateBr(contBB);
                                builder->SetInsertPoint(contBB);
                            }
                        }
                    }
                }
                // An interface ELEMENT slot is the inline 24-byte body: build or copy it in
                // place. Class-ref elements hold an 8-byte pointer and take the plain store.
                bool storedInterfaceInline = false;
                if (lhsAst && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsElemClass = dynamic_pointer_cast<CajetaClass>(
                        lhsAst->getResolvedType());
                    if (lhsElemClass && lhsElemClass->isInterface()) {
                        if (rhsAst && !rhsAst->getResolvedType()) {
                            rhsAst->resolveTypes(module);
                        }
                        storeInterfaceInlineBody(module, lhs, rhsVal,
                            lhsElemClass, rhsAst);
                        storedInterfaceInline = true;
                    }
                }
                // String elements route through the sidecar helpers so the slot's per-element
                // ownership tracks the store shape; both helpers release the old occupant.
                bool storedViaElemOwn = false;
                bool storedViaClassElem = false;
                if (!storedInterfaceInline && lhsAst && rhsAst
                        && lhs && lhs->getType()->isPointerTy()
                        && rhsVal && rhsVal->getType()->isPointerTy()) {
                    if (auto aixLhs = dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                        recordSlotBorrowOnArray(module, lhsAst, rhsAst);
                        llvm::Value* sidecar = nullptr;
                        if (!aixLhs->getChildren().empty()) {
                            if (auto idBase = dynamic_pointer_cast<IdentifierExpression>(
                                    aixLhs->getChildren()[0])) {
                                if (auto sc = module->getScopeStack().peek()) {
                                    if (FieldPtr baseField = sc->getField(
                                            idBase->getTextValue())) {
                                        sidecar = baseField->getElemOwnSidecar();
                                    }
                                }
                            }
                        }
                        // Bit-capable class elements record the store's title in the header-addressed
                        // TAIL BITMAP; a `#=` of a formal forwards the caller's runtime flag verbatim.
                        bool elemArrayTitled =
                            CajetaClass::arrayElementCarriesArraySlotBits(
                                lhsAst->getResolvedType());
                        bool elemTitled =
                            CajetaClass::arrayElementCarriesSlotBits(
                                lhsAst->getResolvedType())
                            || elemArrayTitled;
                        // The tail helper needs the array HEADER, so the receiver is regenerated only
                        // for side-effect-free shapes; anything else keeps the plain store.
                        llvm::Value* tailHdr = nullptr;
                        if (elemTitled) {
                            auto recvAst2 = aixLhs->getChildren().empty()
                                ? nullptr
                                : dynamic_pointer_cast<Expression>(
                                      aixLhs->getChildren()[0]);
                            if (recvAst2
                                    && (dynamic_pointer_cast<
                                            IdentifierExpression>(recvAst2)
                                        || dynamic_pointer_cast<DotExpression>(
                                               recvAst2))) {
                                llvm::Value* rv = recvAst2->generateCode(module);
                                tailHdr = loadIfLValue(module, rv, recvAst2);
                            }
                        }
                        if (elemTitled && tailHdr) {
                            llvm::LLVMContext& tsCtx = *module->getLlvmContext();
                            llvm::Type* tsI64 = llvm::Type::getInt64Ty(tsCtx);
                            llvm::Value* ownedVal = ownership::storeTitleFlag(
                                rhsAst, ownership::ConsumerRole::StoreSlot,
                                module, "an array element slot");
                            if (!ownedVal) ownedVal = llvm::ConstantInt::get(tsI64, 0);
                            const llvm::DataLayout& tsDl =
                                module->getLlvmModule()->getDataLayout();
                            auto recvArr = dynamic_pointer_cast<CajetaArray>(
                                dynamic_pointer_cast<Expression>(
                                    aixLhs->getChildren()[0])->getResolvedType());
                            uint64_t tsHs = 8, tsEs = 8;
                            if (recvArr) {
                                tsHs = tsDl.getTypeAllocSize(
                                    recvArr->getLlvmType());
                                tsEs = recvArr->elementStrideBytes(
                                    tsDl, &tsCtx);
                            }
                            // idx = (slot - hdr - headerSize) / elemSize, from the already-emitted address.
                            llvm::Value* slotInt = builder->CreatePtrToInt(
                                lhs, tsI64);
                            llvm::Value* hdrInt = builder->CreatePtrToInt(
                                tailHdr, tsI64);
                            llvm::Value* tsIdx = builder->CreateSDiv(
                                builder->CreateSub(
                                    builder->CreateSub(slotInt, hdrInt),
                                    llvm::ConstantInt::get(tsI64, tsHs)),
                                llvm::ConstantInt::get(tsI64, tsEs));
                            if (elemArrayTitled) {
                                if (llvm::Function* storeFn =
                                        module->getRuntimeFunction(
                                            "__cajeta_tail_arrelem_store")) {
                                    builder->CreateCall(storeFn,
                                        {tailHdr,
                                         llvm::ConstantInt::get(tsI64, tsHs),
                                         llvm::ConstantInt::get(tsI64, tsEs),
                                         tsIdx, rhsVal, ownedVal,
                                         llvm::ConstantInt::get(tsI64,
                                             CajetaClass::arrayElementInnerDropKind(
                                                 lhsAst->getResolvedType()))});
                                    storedViaElemOwn = true;
                                    storedViaClassElem = true;
                                }
                            } else if (llvm::Function* storeFn =
                                    module->getRuntimeFunction(
                                        "__cajeta_tail_elem_store")) {
                                builder->CreateCall(storeFn,
                                    {tailHdr,
                                     llvm::ConstantInt::get(tsI64, tsHs),
                                     llvm::ConstantInt::get(tsI64, tsEs),
                                     tsIdx, rhsVal, ownedVal});
                                storedViaElemOwn = true;
                                storedViaClassElem = true;
                            }
                        } else if (sidecar) {
                            // An identifier hands its wrapper over only when the local actually HOLDS title;
                            // a borrow-holding local would double-title the string its owner still frees.
                            bool takesOwnership = false;
                            llvm::Value* takesRt = nullptr;
                            if (llvm::Value* t = ownership::storeTitleFlag(
                                    rhsAst, ownership::ConsumerRole::StoreString,
                                    module, "a String array slot")) {
                                if (auto* tc = llvm::dyn_cast<llvm::ConstantInt>(t)) {
                                    takesOwnership = !tc->isZero();
                                } else {
                                    takesRt = t;
                                }
                            }
                            llvm::Function* ownFn = module->getRuntimeFunction(
                                "__cajeta_string_array_elem_set_owned");
                            llvm::Function* aliasFn = module->getRuntimeFunction(
                                "__cajeta_string_array_elem_set_alias");
                            if (takesRt && ownFn && aliasFn) {
                                auto& scCtx = *module->getLlvmContext();
                                llvm::Value* isOwn = builder->CreateICmpNE(
                                    takesRt,
                                    llvm::ConstantInt::get(
                                        takesRt->getType(), 0),
                                    "selem_is_own");
                                llvm::Function* scFn =
                                    builder->GetInsertBlock()->getParent();
                                auto* bbOwn = llvm::BasicBlock::Create(
                                    scCtx, "selem_own", scFn);
                                auto* bbAlias = llvm::BasicBlock::Create(
                                    scCtx, "selem_alias", scFn);
                                auto* bbJoin = llvm::BasicBlock::Create(
                                    scCtx, "selem_join", scFn);
                                builder->CreateCondBr(isOwn, bbOwn, bbAlias);
                                builder->SetInsertPoint(bbOwn);
                                builder->CreateCall(ownFn, {sidecar, lhs, rhsVal});
                                builder->CreateBr(bbJoin);
                                builder->SetInsertPoint(bbAlias);
                                builder->CreateCall(aliasFn, {sidecar, lhs, rhsVal});
                                builder->CreateBr(bbJoin);
                                builder->SetInsertPoint(bbJoin);
                                storedViaElemOwn = true;
                            } else if (llvm::Function* setFn =
                                    takesOwnership ? ownFn : aliasFn) {
                                builder->CreateCall(setFn, {sidecar, lhs, rhsVal});
                                storedViaElemOwn = true;
                            }
                        } else if ([&]{
                            auto selc = dynamic_pointer_cast<CajetaClass>(
                                lhsAst->getResolvedType());
                            return selc && selc->getQName()
                                && selc->getQName()->getTypeName() == "String"
                                && selc->getQName()->getPackageName()
                                       == "cajeta.lang";
                        }()) {
                            bool seTakes = false;
                            llvm::Value* seTakesRt = nullptr;
                            if (llvm::Value* t = ownership::storeTitleFlag(
                                    rhsAst, ownership::ConsumerRole::StoreString,
                                    module, "a String array slot")) {
                                if (auto* tc = llvm::dyn_cast<llvm::ConstantInt>(t)) {
                                    seTakes = !tc->isZero();
                                } else {
                                    seTakesRt = t;
                                }
                            }
                            if (llvm::Function* seFn = module->getRuntimeFunction(
                                    "__cajeta_string_elem_store")) {
                                llvm::Type* seI64 = llvm::Type::getInt64Ty(
                                    *module->getLlvmContext());
                                llvm::Value* takesArg = seTakesRt
                                    ? (seTakesRt->getType() == seI64
                                          ? seTakesRt
                                          : builder->CreateZExt(
                                                seTakesRt, seI64))
                                    : (llvm::Value*) llvm::ConstantInt::get(
                                          seI64, seTakes ? 1 : 0);
                                builder->CreateCall(seFn,
                                    {lhs, rhsVal, takesArg});
                                storedViaElemOwn = true;
                            }
                        }
                    }
                }
                if (!storedInterfaceInline && !storedViaElemOwn) {
                    builder->CreateStore(rhsVal, lhs);
                }
                // A slot-bit store already recorded the spelling (a plain store LENDS), so the
                // implicit source deactivation is skipped; sidecar-less arrays still take it.
                if (!storedViaClassElem && lhsAst
                        && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)
                        && rhsAst) {
                    if (!rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                    CajetaTypePtr elemType = lhsAst->getResolvedType();
                    auto elemClass = dynamic_pointer_cast<CajetaClass>(elemType);
                    bool elemIsArr =
                        dynamic_pointer_cast<CajetaArray>(elemType) != nullptr;
                    bool elemIsIface = elemClass && elemClass->isInterface();
                    bool elemIsPrim = elemType
                        && (elemType->getTypeFlags() & PRIMITIVE_FLAG);
                    bool elemStoresAsPointer = elemClass
                        && (elemIsArr || !elemIsPrim) && !elemIsIface;
                    (void) elemStoresAsPointer;
                }
                // STATIC fields have no per-instance ownership word, so a plain store records
                // nothing: restore the implicit transfer for exactly that shape, or a singleton
                // `instance()` hands back a freed object. Other bit-less stores keep borrows.
                if (!fobWordPtr && lhsAst && rhsAst
                        && dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    CajetaTypePtr fieldType = lhsAst->getResolvedType();
                    auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType);
                    bool fieldIsArr =
                        dynamic_pointer_cast<CajetaArray>(fieldType) != nullptr;
                    bool fieldIsIface = fieldClass && fieldClass->isInterface();
                    bool fieldIsView =
                        dynamic_pointer_cast<CajetaView>(fieldType) != nullptr;
                    bool fieldStoresAsPointer =
                        fieldIsArr || (fieldClass && !fieldIsView && !fieldIsIface);
                    bool fieldIsStatic = false;
                    if (fieldStoresAsPointer) {
                        auto legDot = dynamic_pointer_cast<DotExpression>(lhsAst);
                        auto& lch = legDot->getChildren();
                        auto legRecv = lch.empty() ? nullptr
                            : dynamic_pointer_cast<Expression>(lch[0]);
                        if (legRecv && !legRecv->getResolvedType()) {
                            legRecv->resolveTypes(module);
                        }
                        auto legRecvClass = legRecv
                            ? dynamic_pointer_cast<CajetaClass>(
                                  legRecv->getResolvedType())
                            : nullptr;
                        std::function<bool(const CajetaClassPtr&)> legFind =
                            [&](const CajetaClassPtr& cls) -> bool {
                                if (!cls) return false;
                                auto pit = cls->getProperties().find(
                                    legDot->getIdentifier());
                                if (pit != cls->getProperties().end()) {
                                    fieldIsStatic = pit->second
                                        && pit->second->isStatic();
                                    return true;
                                }
                                for (auto& sup : cls->getSuperClasses()) {
                                    if (legFind(sup)) return true;
                                }
                                return false;
                            };
                        legFind(legRecvClass);
                    }
                    if (fieldStoresAsPointer && fieldIsStatic) {
                        if (auto idExpr =
                                dynamic_pointer_cast<IdentifierExpression>(rhsAst)) {
                            if (auto sc = module->getScopeStack().peek()) {
                                FieldPtr srcField = sc->getField(
                                    idExpr->getTextValue());
                                if (srcField) {
                                    if (srcField->getDropEntry()) {
                                        ownership::deactivateLocalEntry(module, srcField);
                                    }
                                }
                            }
                        }
                    }
                }
                // The field's bit records the store's spelling; a plain store into a
                // bit-carrying field is a BORROW and the source keeps its books.
                if (fobWordPtr && fobBitIdx >= 0) {
                    auto& fobCtx2 = *module->getLlvmContext();
                    llvm::Type* i64Ty2 = llvm::Type::getInt64Ty(fobCtx2);
                    llvm::Value* w = builder->CreateLoad(
                        i64Ty2, fobWordPtr, "own_bits");
                    uint64_t mask = 1ULL << fobBitIdx;
                    llvm::Value* wOld = w;
                    if (fobOwnedSpelling && fobRuntimeFlag) {
                        w = builder->CreateAnd(w,
                            llvm::ConstantInt::get(i64Ty2, ~mask));
                        llvm::Value* fb = builder->CreateShl(
                            builder->CreateAnd(fobRuntimeFlag,
                                llvm::ConstantInt::get(i64Ty2, 1)),
                            llvm::ConstantInt::get(i64Ty2, fobBitIdx));
                        w = builder->CreateOr(w, fb);
                    } else if (fobOwnedSpelling) {
                        w = builder->CreateOr(w,
                            llvm::ConstantInt::get(i64Ty2, mask));
                    } else {
                        w = builder->CreateAnd(w,
                            llvm::ConstantInt::get(i64Ty2, ~mask));
                    }
                    if (fobSameObj) {
                        llvm::Value* keep = builder->CreateSelect(fobSameObj,
                            builder->CreateAnd(wOld, llvm::ConstantInt::get(i64Ty2, mask)),
                            llvm::ConstantInt::get(i64Ty2, 0), "own_keep");
                        w = builder->CreateOr(w, keep);
                    }
                    builder->CreateStore(w, fobWordPtr);
                    // A plain store LENDS, so record the edge (receiver -> lent local): the escape
                    // sites reject a receiver that outlives the source. `#` spellings own.
                    if (!fobOwnedSpelling) {
                        if (auto capDot = dynamic_pointer_cast<DotExpression>(
                                lhsAst)) {
                            // Only a DIRECT `this.field = p`: a nested path writes into another object's
                            // field, where the capture is not this frame's to judge.
                            auto& cch = capDot->getChildren();
                            bool onThis = !cch.empty()
                                && dynamic_pointer_cast<ThisExpression>(cch[0]);
                            if (onThis) {
                                if (auto capSrc = dynamic_pointer_cast<
                                        IdentifierExpression>(rhsAst)) {
                                    if (auto sc =
                                            module->getScopeStack().peek()) {
                                        sc->rejectCapturedBorrowParam(
                                            capSrc->getTextValue(),
                                            "field `" + capDot->getIdentifier()
                                                + "`",
                                            (int) getSourceLine());
                                    }
                                }
                            }
                        }
                        if (auto lendDot = dynamic_pointer_cast<DotExpression>(
                                lhsAst)) {
                            auto& lch = lendDot->getChildren();
                            auto recvId = lch.empty() ? nullptr
                                : dynamic_pointer_cast<IdentifierExpression>(
                                      lch[0]);
                            auto srcId = dynamic_pointer_cast<
                                IdentifierExpression>(rhsAst);
                            if (recvId && srcId) {
                                if (auto sc = module->getScopeStack().peek()) {
                                    FieldPtr srcF = sc->getField(
                                        srcId->getTextValue());
                                    bool srcIsLocalOwner = srcF
                                        && srcF->getDropEntry()
                                        && !dynamic_pointer_cast<ParameterField>(
                                               srcF);
                                    if (srcIsLocalOwner) {
                                        sc->recordLend(recvId->getTextValue(),
                                                       srcId->getTextValue());
                                    }
                                }
                            }
                        }
                    }
                }
                if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                    if (auto sc = module->getScopeStack().peek()) {
                        sc->markAssigned(lhsId->getTextValue());
                    }
                }
                // The runtime drop chain is strict LIFO, so a move-assign RETARGETS the entry
                // LocalVariableDeclaration registered at the declaration (obj + active) rather
                // than pushing a new one into the assigning block's frame.
                {
                    // Re-assigning a binding that has a drop entry: the NEW value's title decides.
                    // An owner re-arms outright, a runtime flag re-arms only when set, and a
                    // borrow leaves the entry on the old value until scope exit.
                    auto& rctx = *module->getLlvmContext();
                    llvm::Type* rI64 = llvm::Type::getInt64Ty(rctx);
                    llvm::Value* rOne = llvm::ConstantInt::get(rI64, 1);
                    llvm::Value* rhsTitle = ownership::storeTitleFlag(
                        rhsAst, ownership::ConsumerRole::Reassign, module,
                        "a re-assigned local");
                    if (auto* rc0 = llvm::dyn_cast_or_null<llvm::ConstantInt>(rhsTitle)) {
                        if (rc0->isZero()) rhsTitle = nullptr;
                        else rhsTitle = rOne;
                    }
                    if (!rhsTitle || !llvm::isa<llvm::ConstantInt>(rhsTitle)) {
                        if (auto rcId = dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                            if (auto rcSc = module->getScopeStack().peek()) {
                                FieldPtr rcField = rcSc->getField(rcId->getTextValue());
                                if (rcField && rcField->getDropEntry()) {
                                    rcField->setRuntimeConditionalOwner(true);
                                    rcField->setEntryMayBeStale(true);
                                }
                            }
                        }
                    }
                    if (rhsTitle) {
                    if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                        auto lhsClass = dynamic_pointer_cast<CajetaClass>(
                            lhsAst->getResolvedType());
                        bool lhsIsArray = (bool) dynamic_pointer_cast<CajetaArray>(
                            lhsAst->getResolvedType());
                        bool lhsIsString = lhsClass && lhsClass->getQName()
                            && lhsClass->getQName()->getTypeName() == "String"
                            && lhsClass->getQName()->getPackageName() == "cajeta.lang";
                        bool lhsIsClassRef = lhsClass && !lhsIsString
                            && !lhsClass->isValueType()
                            && !lhsClass->isSharedCapableValue()
                            && !lhsClass->isInterface();
                        if (lhsIsString || lhsIsClassRef || lhsIsArray) {
                            if (auto sc = module->getScopeStack().peek()) {
                                FieldPtr dstField = sc->getField(lhsId->getTextValue());
                                if (dstField && dstField->getDropEntry()
                                        && rhsVal && rhsVal->getType()->isPointerTy()) {
                                    // The re-arm, inline (release is cold). Drop entry layout:
                                    // obj @0, drop_fn @8, prev @16, active (i8) @24.
                                    auto& rc = *module->getLlvmContext();
                                    llvm::Type* i8 = llvm::Type::getInt8Ty(rc);
                                    llvm::Type* i1 = llvm::Type::getInt1Ty(rc);
                                    llvm::PointerType* pty = llvm::PointerType::get(rc, 0);
                                    llvm::Value* entry = dstField->getDropEntry();
                                    llvm::Function* fnOwner =
                                        builder->GetInsertBlock()->getParent();
                                    llvm::BasicBlock* contBB = nullptr;
                                    if (!llvm::isa<llvm::ConstantInt>(rhsTitle)) {
                                        auto* doBB = llvm::BasicBlock::Create(rc, "reasg_do", fnOwner);
                                        contBB = llvm::BasicBlock::Create(rc, "reasg_cont", fnOwner);
                                        builder->CreateCondBr(
                                            builder->CreateICmpNE(rhsTitle,
                                                llvm::ConstantInt::get(rhsTitle->getType(), 0)),
                                            doBB, contBB);
                                        builder->SetInsertPoint(doBB);
                                    }
                                    llvm::Value* activePtr = builder->CreateInBoundsGEP(
                                        i8, entry, llvm::ConstantInt::get(rI64, 24), "reasg.active.ptr");
                                    llvm::Value* dropFnPtr = builder->CreateInBoundsGEP(
                                        i8, entry, llvm::ConstantInt::get(rI64, 8), "reasg.dropfn.ptr");
                                    llvm::Value* active = builder->CreateLoad(i8, activePtr, "reasg.active");
                                    llvm::Value* oldObj = builder->CreateLoad(pty, entry, "reasg.old");
                                    llvm::Value* dropFn = builder->CreateLoad(pty, dropFnPtr, "reasg.dropfn");
                                    llvm::Value* need = builder->CreateAnd(
                                        builder->CreateAnd(
                                            builder->CreateICmpNE(active, llvm::ConstantInt::get(i8, 0)),
                                            builder->CreateIsNotNull(oldObj)),
                                        builder->CreateAnd(
                                            builder->CreateICmpNE(oldObj, rhsVal),
                                            builder->CreateIsNotNull(dropFn)),
                                        "reasg.release");
                                    (void) i1;
                                    auto* relBB = llvm::BasicBlock::Create(rc, "reasg_release", fnOwner);
                                    auto* storeBB = llvm::BasicBlock::Create(rc, "reasg_store", fnOwner);
                                    builder->CreateCondBr(need, relBB, storeBB);
                                    builder->SetInsertPoint(relBB);
                                    {
                                        llvm::Module* lm =
                                            builder->GetInsertBlock()->getParent()->getParent();
                                        llvm::Constant* cnt = lm->getOrInsertGlobal(
                                            "__cajeta_drop_count", rI64);
                                        builder->CreateAtomicRMW(
                                            llvm::AtomicRMWInst::Add, cnt,
                                            llvm::ConstantInt::get(rI64, 1),
                                            llvm::MaybeAlign(8),
                                            llvm::AtomicOrdering::SequentiallyConsistent);
                                    }
                                    llvm::FunctionType* dropTy = llvm::FunctionType::get(
                                        llvm::Type::getVoidTy(rc), {pty}, false);
                                    builder->CreateCall(dropTy, dropFn, {oldObj});
                                    builder->CreateBr(storeBB);
                                    builder->SetInsertPoint(storeBB);
                                    builder->CreateStore(rhsVal, entry);
                                    builder->CreateStore(llvm::ConstantInt::get(i8, 1), activePtr);
                                    if (contBB) {
                                        builder->CreateBr(contBB);
                                        builder->SetInsertPoint(contBB);
                                    }
                                }
                            }
                        }
                    }
                    }
                }
                // The expression's value is the assigned r-value (C/Java convention).
                result = rhsVal;
                break;
            }
            case BINARY_OP_ADD: {
                llvm::Value* l = loadL(lhs);
                llvm::Value* r = loadR(rhs);
                // String concatenation: pointer operands with no array side lower here. Class
                // String operands are unwrapped to their bytes and the result re-wrapped in a
                // fresh class String, so its type matches the LHS slot with no coercion.
                bool lIsArr = lhsAst && dynamic_pointer_cast<CajetaArray>(lhsAst->getResolvedType());
                bool rIsArr = rhsAst && dynamic_pointer_cast<CajetaArray>(rhsAst->getResolvedType());
                bool lIsPtr = l->getType()->isPointerTy() && !lIsArr;
                bool rIsPtr = r->getType()->isPointerTy() && !rIsArr;
                if (lIsPtr || rIsPtr) {
                    auto& llvmCtx = *module->getLlvmContext();
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                    llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);

                    // Class String may not be loaded yet (the runtime-parse bootstrap window): fall
                    // back to the legacy raw char* concat path.
                    CajetaTypePtr stringTy = CajetaType::of("String");
                    auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
                    llvm::StructType* stringStructTy = nullptr;
                    if (stringKlass && stringKlass->getLlvmType()
                            && llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
                        stringStructTy = llvm::cast<llvm::StructType>(
                            stringKlass->getLlvmType());
                    }
                    auto isClassStringType = [&](CajetaTypePtr t) -> bool {
                        auto cls = std::dynamic_pointer_cast<CajetaClass>(t);
                        return cls && cls->getQName()
                            && cls->getQName()->getTypeName() == "String"
                            && cls->getQName()->getPackageName() == "cajeta.lang";
                    };
                    // Unwrap through the mode-aware accessor: inline and windowed forms materialize
                    // into a per-thread ring scratch, owned/static roots hand out their data.
                    auto extractCStr = [&](llvm::Value* sptr) -> llvm::Value* {
                        if (!stringStructTy) return sptr;
                        llvm::FunctionType* cstrTy = llvm::FunctionType::get(
                            ptrTy, {ptrTy}, false);
                        llvm::FunctionCallee cstrFn =
                            module->getLlvmModule()->getOrInsertFunction(
                                "__cajeta_string_cstr", cstrTy);
                        return builder->CreateCall(cstrFn, {sptr}, "concat.cstr");
                    };

                    auto stringify = [&](llvm::Value* v, CajetaTypePtr vt) -> llvm::Value* {
                        if (isClassStringType(vt)) {
                            return extractCStr(v);
                        }
                        llvm::Type* t = v->getType();
                        if (t->isPointerTy()) return v;
                        if (t->isIntegerTy(1)) {
                            llvm::Value* widened = builder->CreateZExt(v, i32Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                            return builder->CreateCall(fn, {widened});
                        }
                        if (t->isIntegerTy()) {
                            // Format by the operand's OWN signedness: a uint64 past 2^63 has no signed
                            // rendering, and the int64 path would print it negative.
                            const bool uns = vt
                                && (vt->getTypeFlags() & PRIMITIVE_FLAG) != 0
                                && (vt->getTypeFlags() & SIGNED_FLAG) == 0;
                            v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/!uns);
                            llvm::Function* fn = module->getRuntimeFunction(
                                uns ? "__cajeta_u64_to_str" : "__cajeta_i64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        if (t->isFloatingPointTy()) {
                            if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                    };

                    if (lhsAst && !lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    if (rhsAst && !rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                    CajetaTypePtr lhsRT = lhsAst ? lhsAst->getResolvedType() : nullptr;
                    CajetaTypePtr rhsRT = rhsAst ? rhsAst->getResolvedType() : nullptr;
                    // Reduce every operand to a (ptr, len) pair: an integer is formatted once into a
                    // per-site stack scratch by __cajeta_i64_to_buf, which returns the length.
                    llvm::Function* strlenFn = module->getRuntimeFunction("__cajeta_str_len");
                    llvm::Function* i64BufFn = module->getRuntimeFunction("__cajeta_i64_to_buf");
                    llvm::Function* parentFn0 = builder->GetInsertBlock()->getParent();
                    struct ConcatOp { bool isInt; llvm::Value* iv; llvm::Value* ptr; llvm::Value* len; };
                    auto classify = [&](llvm::Value* v, CajetaTypePtr vt) -> ConcatOp {
                        if (!isClassStringType(vt)) {
                            llvm::Type* t = v->getType();
                            if (t->isIntegerTy() && !t->isIntegerTy(1)) {
                                const bool uns = vt
                                    && (vt->getTypeFlags() & PRIMITIVE_FLAG) != 0
                                    && (vt->getTypeFlags() & SIGNED_FLAG) == 0;
                                llvm::Value* iv = builder->CreateIntCast(
                                    v, i64Ty, /*isSigned=*/!uns);
                                // Entry-block scratch, 24 bytes: 20 digits + sign + slack, reused across loops.
                                llvm::IRBuilder<> entryB(&parentFn0->getEntryBlock(),
                                    parentFn0->getEntryBlock().begin());
                                llvm::Value* buf = entryB.CreateAlloca(
                                    llvm::ArrayType::get(i8Ty, 24), nullptr, "concat.itoa");
                                llvm::Function* bufFn = uns
                                    ? module->getRuntimeFunction(
                                        "__cajeta_u64_to_buf")
                                    : i64BufFn;
                                llvm::Value* len = builder->CreateCall(
                                    bufFn, {iv, buf}, "concat.ilen");
                                return { true, iv, buf, len };
                            }
                        }
                        if (isClassStringType(vt) && stringStructTy) {
                            // Tagged String core: Inline text lives at the aux slot's address (12 in-struct
                            // bytes); pointer forms read base + 8 + aux. Selects rather than branches, so
                            // the unselected arm's GEP stays plain and a garbage base cannot poison it.
                            llvm::Value* lt = builder->CreateLoad(i32Ty,
                                builder->CreateStructGEP(stringStructTy, v, 1,
                                    "cat.lentag"));
                            llvm::Value* blen = builder->CreateAnd(lt,
                                llvm::ConstantInt::get(i32Ty, 0x1FFFFFFF),
                                "cat.blen");
                            llvm::Value* isInl = builder->CreateICmpULE(blen,
                                llvm::ConstantInt::get(i32Ty, 12), "cat.isinl");
                            llvm::Value* inlPtr = builder->CreateStructGEP(
                                stringStructTy, v, 2, "cat.inl");
                            llvm::Value* off = builder->CreateIntCast(
                                builder->CreateLoad(i32Ty,
                                    builder->CreateStructGEP(stringStructTy, v, 2,
                                        "cat.aux")),
                                i64Ty, /*isSigned=*/false, "cat.off");
                            llvm::Value* basePtr = builder->CreateLoad(
                                builder->getPtrTy(),
                                builder->CreateStructGEP(stringStructTy, v, 3,
                                    "cat.basep"));
                            llvm::Value* winPtr = builder->CreateGEP(i8Ty, basePtr,
                                builder->CreateAdd(builder->getInt64(8), off,
                                    "cat.winoff"),
                                "cat.win");
                            llvm::Value* data = builder->CreateSelect(
                                isInl, inlPtr, winPtr, "cat.data");
                            llvm::Value* lenI64 = builder->CreateIntCast(
                                blen, i64Ty, /*isSigned=*/false, "cat.len");
                            return { false, nullptr, data, lenI64 };
                        }
                        llvm::Value* cstr = stringify(v, vt);
                        // Class String: take byteLength. bytes.data is NUL-terminated only for literals;
                        // a String wrapped over a bare byte array has no terminator.
                        llvm::Value* len;
                        if (isClassStringType(vt) && stringStructTy) {
                            llvm::Value* lt2 = builder->CreateLoad(i32Ty,
                                builder->CreateStructGEP(stringStructTy, v, 1,
                                    "concat.lentag_slot"), "concat.lentag");
                            llvm::Value* bl32 = builder->CreateAnd(lt2,
                                llvm::ConstantInt::get(i32Ty, 0x1FFFFFFF),
                                "concat.bytelen");
                            len = builder->CreateSExt(bl32, i64Ty, "concat.bytelen64");
                        } else {
                            len = builder->CreateCall(strlenFn, {cstr}, "concat.clen");
                        }
                        return { false, nullptr, cstr, len };
                    };
                    ConcatOp opL = classify(l, lhsRT);
                    ConcatOp opR = classify(r, rhsRT);
                    if (!stringStructTy || !stringKlass) {
                        auto toCStr = [&](const ConcatOp& o) -> llvm::Value* {
                            if (!o.isInt) return o.ptr;
                            return builder->CreateCall(
                                module->getRuntimeFunction("__cajeta_i64_to_str"), {o.iv});
                        };
                        llvm::Function* concat = module->getRuntimeFunction(
                            "__cajeta_str_concat");
                        result = builder->CreateCall(concat, {toCStr(opL), toCStr(opR)});
                        break;
                    }

                    // Direct concat: both operands are written straight into the result String's
                    // storage. Results <= 12 B build the INLINE form (text in the aux+base slots);
                    // longer ones build an OWNED root {count word, text, NUL} with lenTag = total.
                    const int64_t SSO_CAP = 12;
                    // Arena routing: when the escape pre-pass proved the result non-escaping, the
                    // wrapper and any buffer come from the frame arena and its reset reclaims them.
                    bool arena = this->isArenaEligible();
                    const char* wrapAllocName =
                        arena ? "__cajeta_arena_alloc_uninit" : nullptr;
                    const char* bufAllocName =
                        arena ? "__cajeta_arena_alloc_uninit" : "__cajeta_alloc_uninit";
                    llvm::Value* lenL = opL.len;
                    llvm::Value* lenR = opR.len;
                    llvm::Value* total = builder->CreateAdd(lenL, lenR, "concat.total");
                    llvm::Value* totalI32 = builder->CreateIntCast(
                        total, i32Ty, /*isSigned=*/true, "concat.total32");

                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Constant* sSize = llvm::ConstantInt::get(
                        i64Ty, dl.getTypeAllocSize(stringStructTy));
                    llvm::Value* sPtr;
                    if (wrapAllocName) {
                        llvm::FunctionType* wTy = llvm::FunctionType::get(
                            ptrTy, {i64Ty}, false);
                        llvm::FunctionCallee wFn =
                            module->getLlvmModule()->getOrInsertFunction(
                                wrapAllocName, wTy);
                        sPtr = builder->CreateCall(wFn, {sSize}, "concat.s_arena");
                    } else {
                        sPtr = MemoryManager::createMallocInstruction(
                            module, sSize, builder->GetInsertBlock());
                    }

                    llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = stringKlass->getVirtualTableGlobal()) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            module->emitTargetLlvmModule(), vt);
                    }
                    builder->CreateStore(vtableRef,
                        builder->CreateStructGEP(stringStructTy, sPtr, 0,
                            "concat.s_vtable"));

                    llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                    llvm::BasicBlock* ssoBB =
                        llvm::BasicBlock::Create(llvmCtx, "concat.inl", curFn);
                    llvm::BasicBlock* heapBB =
                        llvm::BasicBlock::Create(llvmCtx, "concat.heap", curFn);
                    llvm::BasicBlock* copyBB =
                        llvm::BasicBlock::Create(llvmCtx, "concat.copy", curFn);
                    llvm::Value* isSso = builder->CreateICmpULE(
                        total, llvm::ConstantInt::get(i64Ty, SSO_CAP), "concat.is_inl");
                    builder->CreateCondBr(isSso, ssoBB, heapBB);

                    // Inline: the text destination IS the aux slot's address, 12 in-struct bytes
                    // spanning aux + base; no buffer.
                    builder->SetInsertPoint(ssoBB);
                    llvm::Value* inlDst = builder->CreateStructGEP(
                        stringStructTy, sPtr, 2, "concat.inl_dst");
                    builder->CreateBr(copyBB);

                    // Heap: alloc { i64 count; data; NUL }, count = total; the wrapper adopts it as
                    // its OWNED root (aux = 0, base = hdr).
                    builder->SetInsertPoint(heapBB);
                    llvm::Value* arrSize = builder->CreateAdd(
                        total, llvm::ConstantInt::get(i64Ty, 9), "concat.arr_size");
                    llvm::FunctionType* allocTy = llvm::FunctionType::get(
                        ptrTy, {i64Ty}, false);
                    llvm::FunctionCallee allocFn =
                        module->getLlvmModule()->getOrInsertFunction(
                            bufAllocName, allocTy);
                    llvm::Value* arrPtr = builder->CreateCall(
                        allocFn, {arrSize}, "concat.arr_alloc");
                    builder->CreateStore(total, arrPtr);
                    llvm::Value* heapData = builder->CreateInBoundsGEP(
                        i8Ty, arrPtr, llvm::ConstantInt::get(i64Ty, 8), "concat.heap_data");
                    builder->CreateStore(llvm::ConstantInt::get(i32Ty, 0),
                        builder->CreateStructGEP(stringStructTy, sPtr, 2,
                            "concat.s_aux"));
                    builder->CreateStore(arrPtr,
                        builder->CreateStructGEP(stringStructTy, sPtr, 3,
                            "concat.s_base"));
                    builder->CreateBr(copyBB);

                    builder->SetInsertPoint(copyBB);
                    llvm::PHINode* dataPtr = builder->CreatePHI(ptrTy, 2, "concat.data");
                    dataPtr->addIncoming(inlDst, ssoBB);
                    dataPtr->addIncoming(heapData, heapBB);
                    llvm::PHINode* isInlPhi = builder->CreatePHI(
                        llvm::Type::getInt1Ty(llvmCtx), 2, "concat.was_inl");
                    isInlPhi->addIncoming(llvm::ConstantInt::getTrue(llvmCtx), ssoBB);
                    isInlPhi->addIncoming(llvm::ConstantInt::getFalse(llvmCtx), heapBB);

                    // On the Inline arm total <= 12, so the NUL at data[total] can land on
                    // cachedCpLength's first byte, which the unconditional ccp store below rewrites.
                    builder->CreateMemCpy(dataPtr, llvm::MaybeAlign(1),
                        opL.ptr, llvm::MaybeAlign(1), lenL);
                    llvm::Value* dstR = builder->CreateInBoundsGEP(
                        i8Ty, dataPtr, lenL, "concat.dst_r");
                    builder->CreateMemCpy(dstR, llvm::MaybeAlign(1),
                        opR.ptr, llvm::MaybeAlign(1), lenR);
                    llvm::Value* nulSlot = builder->CreateInBoundsGEP(
                        i8Ty, dataPtr, total, "concat.nul");
                    builder->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulSlot);
                    (void) isInlPhi;

                    // lenTag = total (Inline and OWNED roots carry no tag bits); cachedCpLength = -1.
                    builder->CreateStore(totalI32,
                        builder->CreateStructGEP(stringStructTy, sPtr, 1,
                            "concat.s_lentag"));
                    builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1),
                        builder->CreateStructGEP(stringStructTy, sPtr, 4,
                            "concat.s_cachedCpLength"));

                    // `a + b + c` nests, and the interior node's wrapper is owned by nobody once its
                    // bytes are copied here: drop it, or every chained concat leaks a wrapper. The
                    // classifier excludes arena-routed nodes and every lvalue shape.
                    if (llvm::Function* interiorDrop =
                            module->getRuntimeFunction("__cajeta_string_drop")) {
                        if (MethodCallExpression::freshOwnedStringTemp(lhsAst)
                                && l && l->getType()->isPointerTy()) {
                            builder->CreateCall(interiorDrop, {l});
                        }
                        if (MethodCallExpression::freshOwnedStringTemp(rhsAst)
                                && r && r->getType()->isPointerTy()) {
                            builder->CreateCall(interiorDrop, {r});
                        }
                    }

                    // Pin resolvedType so a caller using this concat as an argument sees the class type.
                    resolvedType = stringTy;
                    result = sPtr;
                    break;
                }
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
                };
                auto [pl, pr] = coerceArithPair(module, l, r);
                if (pl->getType()->isFPOrFPVectorTy()) {
                    result = emitFpBinOp(module, pl, pr, llvm::Instruction::FAdd);
                } else if (module->getFlags().overflowChecks == OverflowChecks::On
                        && pl->getType()->isIntegerTy()
                        && signedFromAst(lhsAst, rhsAst)) {
                    result = emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::sadd_with_overflow, pl, pr, "ofc.add");
                } else {
                    result = builder->CreateAdd(pl, pr);
                }
                break;
            }
            case BINARY_OP_SUB: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    result = deadPtrArith(module, l, r, BINARY_OP_SUB);
                    break;
                }
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
                };
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FSub);
                } else if (module->getFlags().overflowChecks == OverflowChecks::On
                        && l->getType()->isIntegerTy()
                        && signedFromAst(lhsAst, rhsAst)) {
                    result = emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::ssub_with_overflow, l, r, "ofc.sub");
                } else {
                    result = builder->CreateSub(l, r);
                }
                break;
            }
            case BINARY_OP_MUL: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    result = deadPtrArith(module, l, r, BINARY_OP_MUL);
                    break;
                }
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
                };
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FMul);
                } else if (module->getFlags().overflowChecks == OverflowChecks::On
                        && l->getType()->isIntegerTy()
                        && signedFromAst(lhsAst, rhsAst)) {
                    result = emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::smul_with_overflow, l, r, "ofc.mul");
                } else {
                    result = builder->CreateMul(l, r);
                }
                break;
            }
            case BINARY_OP_DIV: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    result = deadPtrArith(module, l, r, BINARY_OP_DIV);
                    break;
                }
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                } else {
                    if (module->getFlags().ubTraps) {
                        llvm::Value* zero = llvm::Constant::getNullValue(r->getType());
                        llvm::Value* isZero = builder->CreateICmpEQ(r, zero, "ubt.div.z");
                        emitUbTrap(module, *builder, isZero, "div");
                    }
                    result = intOpIsSigned ? builder->CreateSDiv(l, r)
                                           : builder->CreateUDiv(l, r);
                }
                break;
            }
            case BINARY_OP_BITAND: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                result = builder->CreateAnd(l, r);
                break;
            }
            case BINARY_OP_BITOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                result = builder->CreateOr(l, r);
                break;
            }
            case BINARY_OP_BITXOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                result = builder->CreateXor(l, r);
                break;
            }
            case BINARY_OP_SHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (module->getFlags().ubTraps) {
                    unsigned width = l->getType()->getScalarSizeInBits();
                    llvm::Value* widthC = llvm::ConstantInt::get(r->getType(), width);
                    // An unsigned compare catches both r >= width and a negative count.
                    llvm::Value* bad = builder->CreateICmpUGE(r, widthC, "ubt.shr.over");
                    emitUbTrap(module, *builder, bad, "shr");
                }
                // `>>` follows the SHIFTED operand's signedness alone: the right operand is a
                // COUNT, and folding its flags in the way `/` does makes `someUint64 >> 33`
                // arithmetic. `>>>` stays unconditionally logical.
                result = ((lhsTypeFlags & SIGNED_FLAG) != 0)
                    ? builder->CreateAShr(l, r)
                    : builder->CreateLShr(l, r);
                break;
            }
            case BINARY_OP_USHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (module->getFlags().ubTraps) {
                    unsigned width = l->getType()->getScalarSizeInBits();
                    llvm::Value* widthC = llvm::ConstantInt::get(r->getType(), width);
                    llvm::Value* bad = builder->CreateICmpUGE(r, widthC, "ubt.ushr.over");
                    emitUbTrap(module, *builder, bad, "ushr");
                }
                result = builder->CreateLShr(l, r);
                break;
            }
            case BINARY_OP_SHIFTLEFT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (module->getFlags().ubTraps) {
                    unsigned width = l->getType()->getScalarSizeInBits();
                    llvm::Value* widthC = llvm::ConstantInt::get(r->getType(), width);
                    llvm::Value* bad = builder->CreateICmpUGE(r, widthC, "ubt.shl.over");
                    emitUbTrap(module, *builder, bad, "shl");
                }
                result = builder->CreateShl(l, r);
                break;
            }
            case BINARY_OP_MOD: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = builder->CreateFRem(l, r);
                } else {
                    if (module->getFlags().ubTraps) {
                        llvm::Value* zero = llvm::Constant::getNullValue(r->getType());
                        llvm::Value* isZero = builder->CreateICmpEQ(r, zero, "ubt.mod.z");
                        emitUbTrap(module, *builder, isZero, "mod");
                    }
                    result = intOpIsSigned ? builder->CreateSRem(l, r)
                                           : builder->CreateURem(l, r);
                }
                break;
            }
            // Compound assignments compute at the wider type, then truncate to the slot's type.
            case BINARY_OP_ADD_EQUALS:
            case BINARY_OP_SUB_EQUALS:
            case BINARY_OP_MUL_EQUALS:
            case BINARY_OP_DIV_EQUALS:
            case BINARY_OP_BITAND_EQUALS:
            case BINARY_OP_BITOR_EQUALS:
            case BINARY_OP_BITXOR_EQUALS:
            case BINARY_OP_SHIFTRIGHT_EQUALS:
            case BINARY_OP_USHIFTRIGHT_EQUALS:
            case BINARY_OP_SHIFTLEFT_EQUALS:
            case BINARY_OP_MOD_EQUALS: {
                // Compound assignment on a class LHS: an explicit instance `operator+=` mutates
                // in place; failing that, derive from the static binary form and store the
                // result back; failing that, fall through to the primitive path below.
                const char* cmpSym  = nullptr;
                const char* baseSym = nullptr;
                switch (binaryOp) {
                    case BINARY_OP_ADD_EQUALS:        cmpSym = "+=";   baseSym = "+";   break;
                    case BINARY_OP_SUB_EQUALS:        cmpSym = "-=";   baseSym = "-";   break;
                    case BINARY_OP_MUL_EQUALS:        cmpSym = "*=";   baseSym = "*";   break;
                    case BINARY_OP_DIV_EQUALS:        cmpSym = "/=";   baseSym = "/";   break;
                    case BINARY_OP_MOD_EQUALS:        cmpSym = "%=";   baseSym = "%";   break;
                    case BINARY_OP_BITAND_EQUALS:     cmpSym = "&=";   baseSym = "&";   break;
                    case BINARY_OP_BITOR_EQUALS:      cmpSym = "|=";   baseSym = "|";   break;
                    case BINARY_OP_BITXOR_EQUALS:     cmpSym = "^=";   baseSym = "^";   break;
                    case BINARY_OP_SHIFTLEFT_EQUALS:  cmpSym = "<<=";  baseSym = "<<";  break;
                    case BINARY_OP_SHIFTRIGHT_EQUALS: cmpSym = ">>=";  baseSym = ">>";  break;
                    case BINARY_OP_USHIFTRIGHT_EQUALS:cmpSym = ">>>="; baseSym = ">>>"; break;
                    default: break;
                }
                if (cmpSym && lhsAst) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsClass = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
                    if (lhsClass && !lhsClass->isInterface()
                            && !(lhsClass->getTypeFlags() & PRIMITIVE_FLAG)) {
                        if (rhsAst && !rhsAst->getResolvedType()) {
                            rhsAst->resolveTypes(module);
                        }
                        CajetaTypePtr lhsType = lhsAst->getResolvedType();
                        CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;
                        if (lhsType && rhsType) {
                            llvm::Value* recvVal = loadIfLValue(module, lhs, lhsAst);
                            llvm::Value* rhsVal  = loadIfLValue(module, rhs, rhsAst);
                            std::string cmpName = std::string("operator") + cmpSym;
                            vector<ParameterEntry> instEntries;
                            instEntries.push_back(ParameterEntry(rhsType, "", rhsVal));
                            if (lhsClass->resolveMethod(cmpName, instEntries,
                                    /*isConstructor=*/false, /*floatingParams=*/false)) {
                                result = lhsClass->invokeMethod(cmpName, instEntries,
                                    /*isConstructor=*/false, recvVal,
                                    /*callerModule=*/module);
                                break;
                            }
                            std::string baseName = std::string("operator") + baseSym;
                            vector<ParameterEntry> binEntries;
                            binEntries.push_back(ParameterEntry(lhsType, "", recvVal));
                            binEntries.push_back(ParameterEntry(rhsType, "", rhsVal));
                            if (lhsClass->resolveMethod(baseName, binEntries,
                                    /*isConstructor=*/false, /*floatingParams=*/false)) {
                                llvm::Value* newVal = lhsClass->invokeMethod(
                                    baseName, binEntries,
                                    /*isConstructor=*/false,
                                    /*thisInstance=*/nullptr,
                                    /*callerModule=*/module);
                                if (newVal) {
                                    builder->CreateStore(newVal, lhs);
                                    result = newVal;
                                    break;
                                }
                            }
                        }
                    }
                }
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                llvm::Value* newVal = nullptr;
                bool isFp = l->getType()->isFloatingPointTy();
                // Same usual-arithmetic-conversion rule as the standalone `/` and `%`.
                bool isSigned = intOpIsSigned;
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
                };
                bool emitOfTrap = !isFp && l->getType()->isIntegerTy()
                    && module->getFlags().overflowChecks == OverflowChecks::On
                    && signedFromAst(lhsAst, rhsAst);
                // Narrow to the lhs slot's width BEFORE the op so the overflow check fires at
                // the destination type's edge; `int32 a; a += 1;` at INT32_MAX otherwise wraps.
                if (emitOfTrap) {
                    llvm::Type* slotTy = nullptr;
                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                        slotTy = a->getAllocatedType();
                    } else if (lhsAst && lhsAst->getResolvedType()) {
                        slotTy = lhsAst->getResolvedType()->getLlvmType();
                    }
                    if (slotTy && slotTy->isIntegerTy()
                            && l->getType() != slotTy) {
                        l = builder->CreateIntCast(l, slotTy, /*isSigned=*/true);
                    }
                    if (slotTy && slotTy->isIntegerTy()
                            && r->getType() != slotTy) {
                        r = builder->CreateIntCast(r, slotTy, /*isSigned=*/true);
                    }
                }
                switch (binaryOp) {
                    case BINARY_OP_ADD_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FAdd);
                        else if (emitOfTrap) newVal = emitSignedOverflowOp(
                            module, *builder,
                            llvm::Intrinsic::sadd_with_overflow, l, r, "ofc.add_eq");
                        else newVal = builder->CreateAdd(l, r);
                        break;
                    case BINARY_OP_SUB_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FSub);
                        else if (emitOfTrap) newVal = emitSignedOverflowOp(
                            module, *builder,
                            llvm::Intrinsic::ssub_with_overflow, l, r, "ofc.sub_eq");
                        else newVal = builder->CreateSub(l, r);
                        break;
                    case BINARY_OP_MUL_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FMul);
                        else if (emitOfTrap) newVal = emitSignedOverflowOp(
                            module, *builder,
                            llvm::Intrinsic::smul_with_overflow, l, r, "ofc.mul_eq");
                        else newVal = builder->CreateMul(l, r);
                        break;
                    case BINARY_OP_DIV_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                        else if (isSigned) newVal = builder->CreateSDiv(l, r);
                        else newVal = builder->CreateUDiv(l, r);
                        break;
                    case BINARY_OP_BITAND_EQUALS:     newVal = builder->CreateAnd(l, r);  break;
                    case BINARY_OP_BITOR_EQUALS:      newVal = builder->CreateOr(l, r);   break;
                    case BINARY_OP_BITXOR_EQUALS:     newVal = builder->CreateXor(l, r);  break;
                    // As with the standalone `>>`, the fill bit follows the shifted operand alone.
                    case BINARY_OP_SHIFTRIGHT_EQUALS:
                        newVal = ((lhsTypeFlags & SIGNED_FLAG) != 0)
                            ? builder->CreateAShr(l, r)
                            : builder->CreateLShr(l, r);
                        break;
                    case BINARY_OP_USHIFTRIGHT_EQUALS:newVal = builder->CreateLShr(l, r); break;
                    case BINARY_OP_SHIFTLEFT_EQUALS:  newVal = builder->CreateShl(l, r);  break;
                    case BINARY_OP_MOD_EQUALS:
                        newVal = isSigned ? builder->CreateSRem(l, r) : builder->CreateURem(l, r);
                        break;
                    default: break;
                }
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                    llvm::Type* slotTy = a->getAllocatedType();
                    if (newVal && newVal->getType() != slotTy) {
                        if (slotTy->isIntegerTy() && newVal->getType()->isIntegerTy()) {
                            newVal = builder->CreateIntCast(newVal, slotTy, /*isSigned=*/true);
                        } else if (slotTy->isFloatingPointTy() && newVal->getType()->isFloatingPointTy()) {
                            newVal = builder->CreateFPCast(newVal, slotTy);
                        }
                    }
                }
                if (newVal) {
                    builder->CreateStore(newVal, lhs);
                }
                result = newVal;
                break;
            }
            case BINARY_OP_LT:
            case BINARY_OP_LE:
            case BINARY_OP_GT:
            case BINARY_OP_GE:
            case BINARY_OP_EQ:
            case BINARY_OP_NE: {
                // Fat-aware interface `== null`: both shapes hand back a pointer to the 24-byte
                // body, and that address is never null, so compare the body's DATA word. The
                // null case memsets the body to zero.
                {
                    if (lhsAst && !lhsAst->getResolvedType())
                        lhsAst->resolveTypes(module);
                    if (rhsAst && !rhsAst->getResolvedType())
                        rhsAst->resolveTypes(module);
                    auto ifaceOf = [&](ExpressionPtr a) -> CajetaClassPtr {
                        auto c = dynamic_pointer_cast<CajetaClass>(
                            a ? a->getResolvedType() : nullptr);
                        return (c && c->isInterface()) ? c : nullptr;
                    };
                    llvm::Value* lv = loadL(lhs);
                    llvm::Value* rv = loadR(rhs);
                    CajetaClassPtr ifaceCls;
                    llvm::Value* bodyPtr = nullptr;
                    if (ifaceOf(lhsAst) && rv
                            && llvm::isa<llvm::ConstantPointerNull>(rv)) {
                        ifaceCls = ifaceOf(lhsAst); bodyPtr = lv;
                    } else if (ifaceOf(rhsAst) && lv
                            && llvm::isa<llvm::ConstantPointerNull>(lv)) {
                        ifaceCls = ifaceOf(rhsAst); bodyPtr = rv;
                    }
                    if (ifaceCls && bodyPtr) {
                        llvm::Type* bodyTy = ifaceCls->getLlvmType();
                        llvm::Type* ptrTy = llvm::PointerType::get(
                            *module->getLlvmContext(), 0);
                        llvm::Value* data = nullptr;
                        // An interface local initialized to `null` collapses to a literal null pointer;
                        // there is nothing to load, and loading it would fault.
                        if (llvm::isa<llvm::ConstantPointerNull>(bodyPtr)) {
                            result = builder->getInt1(
                                binaryOp == BINARY_OP_EQ);
                            break;
                        }
                        if (bodyPtr->getType()->isPointerTy()
                                && bodyTy && bodyTy->isStructTy()) {
                            // The body pointer is itself null for a reference that was never bound, so
                            // branch: only a live pointer gets its data word read.
                            llvm::Value* nulP =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            llvm::Function* fn =
                                builder->GetInsertBlock()->getParent();
                            llvm::BasicBlock* entryBB =
                                builder->GetInsertBlock();
                            llvm::BasicBlock* loadBB = llvm::BasicBlock::Create(
                                *module->getLlvmContext(), "iface.null.load", fn);
                            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(
                                *module->getLlvmContext(), "iface.null.done", fn);
                            llvm::Value* ptrIsNull = builder->CreateICmpEQ(
                                bodyPtr, nulP, "iface.ptrnull");
                            builder->CreateCondBr(ptrIsNull, doneBB, loadBB);

                            builder->SetInsertPoint(loadBB);
                            llvm::Value* dataSlot = builder->CreateStructGEP(
                                bodyTy, bodyPtr, 0, "iface_null_data");
                            llvm::Value* dw = builder->CreateLoad(ptrTy, dataSlot);
                            llvm::Value* dwIsNull = builder->CreateICmpEQ(
                                dw, nulP, "iface.datanull");
                            builder->CreateBr(doneBB);

                            builder->SetInsertPoint(doneBB);
                            llvm::PHINode* phi = builder->CreatePHI(
                                builder->getInt1Ty(), 2, "iface.isnull.phi");
                            phi->addIncoming(builder->getTrue(), entryBB);
                            phi->addIncoming(dwIsNull, loadBB);
                            result = (binaryOp == BINARY_OP_EQ)
                                ? static_cast<llvm::Value*>(phi)
                                : builder->CreateNot(phi, "iface.notnull");
                            break;
                        } else if (bodyPtr->getType()->isStructTy()) {
                            // A STATIC field of interface type loads as the fat VALUE, not as a pointer to
                            // it (locals and instance fields hand back the pointer), so take the data word
                            // with extractvalue rather than a load.
                            data = builder->CreateExtractValue(
                                bodyPtr, 0, "iface_null_data");
                        }
                        if (data && data->getType()->isPointerTy()) {
                            llvm::Value* nul =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            result = (binaryOp == BINARY_OP_EQ)
                                ? builder->CreateICmpEQ(data, nul, "iface.isnull")
                                : builder->CreateICmpNE(data, nul, "iface.notnull");
                            break;
                        }
                    }
                }
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                bool isFp = l->getType()->isFloatingPointTy();
                // OR in the AST-derived signedness: getTypeFlagsOf keys on the LLVM type (i32
                // for both int32 and uint32) and would make `7 > -1000` an unsigned compare.
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
                };
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0
                    || signedFromAst(lhsAst, rhsAst);
                if (isFp) {
                    switch (binaryOp) {
                        case BINARY_OP_LT: result = builder->CreateFCmpOLT(l, r); break;
                        case BINARY_OP_LE: result = builder->CreateFCmpOLE(l, r); break;
                        case BINARY_OP_GT: result = builder->CreateFCmpOGT(l, r); break;
                        case BINARY_OP_GE: result = builder->CreateFCmpOGE(l, r); break;
                        case BINARY_OP_EQ: result = builder->CreateFCmpOEQ(l, r); break;
                        case BINARY_OP_NE: result = builder->CreateFCmpUNE(l, r); break;  // UNE: NaN != NaN is TRUE (IEEE; the self-compare NaN idiom)
                        default: break;
                    }
                } else {
                    switch (binaryOp) {
                        case BINARY_OP_LT:
                            result = isSigned ? builder->CreateICmpSLT(l, r) : builder->CreateICmpULT(l, r);
                            break;
                        case BINARY_OP_LE:
                            result = isSigned ? builder->CreateICmpSLE(l, r) : builder->CreateICmpULE(l, r);
                            break;
                        case BINARY_OP_GT:
                            result = isSigned ? builder->CreateICmpSGT(l, r) : builder->CreateICmpUGT(l, r);
                            break;
                        case BINARY_OP_GE:
                            result = isSigned ? builder->CreateICmpSGE(l, r) : builder->CreateICmpUGE(l, r);
                            break;
                        case BINARY_OP_EQ: result = builder->CreateICmpEQ(l, r); break;
                        case BINARY_OP_NE: result = builder->CreateICmpNE(l, r); break;
                        default: break;
                    }
                }
                break;
            }
            case BINARY_OP_LOGAND:
            case BINARY_OP_LOGOR:
                break;
        }
        // Script units: assigning a top-level session binding REBINDS it, the runtime
        // dropping the previous occupant and taking the stored value. Emitted after
        // whichever store arm ran, and keyed on `assignment` so `k += 2` counts too.
        if (assignment && module->isScriptUnit()) {
            if (auto lhsId =
                    dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                const std::string& name = lhsId->getTextValue();
                FieldPtr lookedUp;
                if (auto sc0 = module->getScopeStack().peek()) {
                    lookedUp = sc0->getField(name);
                }
                bool seeded = lookedUp && lookedUp->isSessionSeeded();
                // Key on the FIELD's session flags, not the name: under a block-local shadow the
                // write targets the local and must not touch the registry.
                bool sessionTarget = lookedUp
                    ? (lookedUp->isSessionBound() || seeded)
                    : module->isScriptBindingName(name);
                if (sessionTarget) {
                    auto* builder = module->getBuilder();
                    llvm::Function* pfn =
                        builder->GetInsertBlock()->getParent();
                    if (pfn && pfn->getName().find(scriptEntryName())
                                   != llvm::StringRef::npos) {
                        FieldPtr f = lookedUp;
                        auto klass = f
                            ? dynamic_pointer_cast<CajetaClass>(
                                  f->getType())
                            : nullptr;
                        llvm::Function* dropFn = nullptr;
                        if (klass && klass->hasVtablePointerAtSlotZero()) {
                            klass->patchVirtualTableDropFn();
                            dropFn = module->getRuntimeFunction(
                                "__cajeta_class_virtual_drop");
                        } else if (klass && klass->getQName()
                                   && klass->getQName()->getTypeName()
                                          == "String") {
                            dropFn = module->getRuntimeFunction(
                                "__cajeta_string_drop");
                        }
                        CajetaTypePtr ft = f ? f->getType() : nullptr;
                        bool primitive =
                            ft && (ft->getTypeFlags() & PRIMITIVE_FLAG);
                        auto& sctx = *module->getLlvmContext();
                        if (f && primitive) {
                            // A primitive has no drop and no pointer to register, so its bytes are boxed as
                            // the declaration path boxes them, or a later cell reads the stale box.
                            if (llvm::Function* bindVal =
                                    module->getRuntimeFunction(
                                        "__cajeta_session_bind_value")) {
                                llvm::AllocaInst* slot =
                                    f->getOrCreateAllocation();
                                auto& dl =
                                    module->getLlvmModule()->getDataLayout();
                                uint64_t size = slot
                                    ? dl.getTypeStoreSize(
                                          slot->getAllocatedType()) : 0;
                                if (slot && size > 0) {
                                    builder->CreateCall(bindVal,
                                        {builder->CreateGlobalString(name),
                                         slot,
                                         llvm::ConstantInt::get(
                                             llvm::Type::getInt64Ty(sctx),
                                             size)});
                                }
                            }
                        } else if (llvm::Function* bindFn =
                                       module->getRuntimeFunction(
                                           "__cajeta_session_bind")) {
                            // A reference with no drop of its own is registered with a null drop_fn: visible
                            // to the next cell without the session owning it.
                            llvm::PointerType* pTy =
                                llvm::PointerType::get(sctx, 0);
                            if (f) {
                                llvm::Value* cur = builder->CreateLoad(
                                    pTy, f->getOrCreateAllocation());
                                builder->CreateCall(bindFn,
                                    {builder->CreateGlobalString(name), cur,
                                     dropFn ? (llvm::Value*) dropFn
                                            : (llvm::Value*)
                                              llvm::ConstantPointerNull::get(
                                                  pTy)});
                            }
                        }
                    }
                }
            }
        }
        return result;
    }

} // code