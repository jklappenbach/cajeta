//
// Created by James Klappenbach on 4/8/23.
//

#include "BinaryOpExpression.h"
#include "../../error/CajetaExceptions.h"
#include "../../error/Exception.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaStruct.h"
#include "../../type/CajetaArray.h"
#include "Expression.h"
#include "DotExpression.h"
#include "Identifier.h"
#include "AggregateInitializerExpression.h"
#include "NewExpression.h"

namespace cajeta {

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

    // l-value → r-value coercion. Three cases:
    //   (1) IdentifierExpression alloca: load with the alloca's allocated type.
    //       Common case for local vars.
    //   (2) ArrayIndex GEP: load the element value. Reference-typed elements load as
    //       `ptr` (the slot stores a pointer to the referenced object); primitive
    //       elements load their own LLVM type.
    //   (3) DotExpression GEP into a struct/class field: load with the field's
    //       LLVM type and bswap if the receiver's endianness differs from host.
    // Constants and intermediate r-values pass through unchanged.
    //
    // The AllocaInst → load mapping is gated on the AST being an
    // IdentifierExpression (or absent — legacy callers without an AST). Other
    // expression types that legitimately produce AllocaInst values that are
    // NOT slot pointers (LambdaExpression's closure record, NewExpression's
    // stack allocations, etc.) must pass through unchanged, because the
    // alloca address IS the value being yielded.
    llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v, ExpressionPtr ast) {
        if (!v) return v;
        auto* builder = module->getBuilder();
        bool treatAllocaAsSlot = !ast
            || dynamic_pointer_cast<IdentifierExpression>(ast) != nullptr;
        if (treatAllocaAsSlot) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                return builder->CreateLoad(a->getAllocatedType(), a);
            }
        }
        if (ast && dynamic_pointer_cast<ArrayIndexExpression>(ast)) {
            CajetaTypePtr elemType = ast->getResolvedType();
            if (elemType) {
                llvm::Type* loadTy;
                // Class-typed elements (CajetaArray, CajetaStruct, plain
                // CajetaClass) are stored as pointers in the array's
                // data slot. Loading the slot yields the heap reference,
                // not the struct contents. CajetaArray and CajetaStruct
                // both inherit from CajetaClass, so the dynamic_cast
                // catches all three. Primitives load as their value type.
                if (dynamic_pointer_cast<CajetaClass>(elemType) ||
                    (elemType->getTypeFlags() & STRUCT_FLAG)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = elemType->getLlvmType();
                }
                if (loadTy) return builder->CreateLoad(loadTy, v);
            }
        }
        // DotExpression usually returns a field's GEP slot pointer that
        // needs loading through. The exception is variable-size struct
        // fields (Structs.md § Inline length-prefix layout), where
        // DotExpression returns a runtime helper's already-materialized
        // String pointer — that's the value, not a slot to load. Gate
        // on v being an actual GEP so we don't double-load the helper
        // result. Doesn't use `loadTy != v->getType()` because a
        // `pointer`-typed field has loadTy == ptr == v->getType() but
        // still needs the load.
        if (auto dot = dynamic_pointer_cast<DotExpression>(ast)) {
            if (auto resolved = ast->getResolvedType()) {
                // S9.5.4 — interface fields are 24-byte fat-pointer
                // bodies inline in the parent. Dispatch and assignment
                // alike want a pointer to the body, not a loaded 24-byte
                // value. Treat the GEP itself as the language-level
                // interface value (mirrors how CajetaAggregate works:
                // the value IS the pointer).
                auto resolvedClass = dynamic_pointer_cast<CajetaClass>(resolved);
                bool resolvedIsInterface = resolvedClass && resolvedClass->isInterface();
                if (resolvedIsInterface && llvm::isa<llvm::GetElementPtrInst>(v)) {
                    return v;
                }
                // Array-typed fields are stored in the parent class
                // struct as `ptr` (see CajetaClass::generatePrototype's
                // fieldLayoutType rule), not as the inline header
                // struct. Loading the slot must use ptr, not the
                // header type — otherwise we read past the slot into
                // the next field's bytes. Same rule that
                // ArrayIndexExpression and the slot-type computation
                // in BINARY_OP_ASSIGN already follow.
                //
                // S6.3: same `ptr` rule when the PARENT is a CajetaStruct
                // and the field is a plain class ref. Structs lay out
                // class-typed fields as pointer slots (per Structs.md
                // § "Class references occupy a single pointer-width slot");
                // loading the receiver's class struct body through an
                // 8-byte slot would walk past the slot into neighbor
                // bytes. Class-of-class still loads as the class struct
                // because CajetaClass embeds class fields inline today
                // (the known wart noted in CajetaClass::generatePrototype).
                llvm::Type* loadTy;
                bool parentIsStruct = false;
                if (!dot->getChildren().empty()) {
                    auto recv = dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                    if (recv && recv->getResolvedType()) {
                        parentIsStruct = dynamic_pointer_cast<CajetaStruct>(
                            recv->getResolvedType()) != nullptr;
                    }
                }
                bool fieldIsClassRef = dynamic_pointer_cast<CajetaClass>(resolved) != nullptr
                    && !dynamic_pointer_cast<CajetaView>(resolved)
                    && !dynamic_pointer_cast<CajetaArray>(resolved);
                if (dynamic_pointer_cast<CajetaArray>(resolved)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else if (parentIsStruct && fieldIsClassRef) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = resolved->getLlvmType();
                }
                if (loadTy) {
                    if (llvm::isa<llvm::GetElementPtrInst>(v)) {
                        llvm::Value* loaded = builder->CreateLoad(loadTy, v);
                        if (!dot->getChildren().empty()) {
                            auto recv = dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                            loaded = DotExpression::maybeBswap(module, loaded, recv);
                        }
                        return loaded;
                    }
                }
            }
        }
        // SpawnExpression returns the malloc'd Task<T>* directly — the
        // pointer IS the language-level value (Task is a heap class;
        // local Task<T> slots hold ptrs, not the struct itself). The
        // generic "pointer-with-different-type" branch below would
        // otherwise load the entire struct through the ptr because
        // resolvedType's getLlvmType() returns the struct type, not
        // the pointer-to-struct shape. Pre-empt it before that catch-
        // all fires.
        if (dynamic_pointer_cast<SpawnExpression>(ast)) {
            return v;
        }
        // P2a/P2b — aggregate-init always returns a body pointer (alloca
        // for stack, malloc for heap); the value IS the reference, not
        // an l-value to load through. Covers both struct (via the
        // CajetaAggregate branch above) and plain CajetaClass (the
        // unified-class rollout broadens aggregate-init to any class
        // for both heap and stack paths).
        if (dynamic_pointer_cast<AggregateInitializerExpression>(ast)) {
            return v;
        }
        // P7.3+ — NewExpression also returns a body pointer (malloc for
        // `new`/`heap`, alloca for `stack`). Same shape as Aggregate-
        // InitExpression: the value IS the reference. Without this
        // bypass, an assignment like `this.h = heap Hello()` loads the
        // entire Hello struct (which for a vtable-only class is just
        // the vtable pointer's 8 bytes) through the heap ptr and stores
        // those bytes into h's slot — leaving h pointing at the static
        // vtable address. Then the stack-drop walking h calls free on
        // the vtable address (invalid pointer crash).
        if (dynamic_pointer_cast<NewExpression>(ast)) {
            return v;
        }
        // IdentifierExpression that resolved to a class property via the
        // implicit-this fallback also returns a GEP — same load story.
        // We detect it by v being a pointer-typed value (the GEP) while
        // ast's resolvedType is a non-pointer scalar (the field's type).
        // The standalone pointer-with-different-type check below catches
        // this; an explicit branch isn't needed here.
        if (v->getType()->isPointerTy() && ast) {
            if (auto resolved = ast->getResolvedType()) {
                // CajetaArray-typed values are reference-typed: the
                // pointer IS the heap header pointer, not a slot
                // holding the array struct. Don't deref. Same shape
                // the ArrayIndex branch above uses for array elements.
                if (dynamic_pointer_cast<CajetaArray>(resolved)) {
                    return v;
                }
                // Same rule for struct/view aggregates — Cajeta passes
                // these by pointer, so the value IS the pointer. Pre-S6.2
                // this was reachable only when an aggregate-producing
                // expression (e.g. view ctor) left resolvedType null;
                // AggregateInitializerExpression now sets resolvedType to
                // the struct type, so without this branch the catch-all
                // below would load the whole struct through the pointer
                // and corrupt the receiving HeapField slot.
                if (dynamic_pointer_cast<CajetaView>(resolved)) {
                    return v;
                }
                // S9.5.4 — interface values are 24-byte fat-pointer
                // bodies; the language-level value is the pointer to
                // the body (same convention as CajetaAggregate). The
                // catch-all `loadTy != v->getType()` below would
                // otherwise load 24 bytes through the body pointer and
                // hand back the wrong shape.
                if (auto rc = dynamic_pointer_cast<CajetaClass>(resolved)) {
                    if (rc->isInterface()) {
                        return v;
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

    // Backwards-compat shim — most call sites don't have the ast handy. Behaves like the
    // old loadIfAlloca: only AllocaInst is unwrapped.
    static llvm::Value* loadIfAlloca(CajetaModulePtr module, llvm::Value* v) {
        return loadIfLValue(module, v, nullptr);
    }

    // Coerce two arithmetic operands to a common LLVM type. Cajeta lacks a global type-
    // promotion pass, so the binary-op site does the minimum needed for the IR verifier
    // to accept the result. Strategy: if either is FP, both go to the wider FP; else if
    // both are integers, both go to the wider integer (signed).
    static std::pair<llvm::Value*, llvm::Value*> coerceArithPair(
            CajetaModulePtr module, llvm::Value* l, llvm::Value* r) {
        auto* builder = module->getBuilder();
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return {l, r};

        if (lt->isFloatingPointTy() || rt->isFloatingPointTy()) {
            // Promote the int side to FP, then widen FP to the larger.
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
            if (lb < rb) l = builder->CreateIntCast(l, rt, /*isSigned=*/true);
            else if (rb < lb) r = builder->CreateIntCast(r, lt, /*isSigned=*/true);
            return {l, r};
        }
        return {l, r};
    }


    /**
     * Business logic:
     *  - First, translate our arguments into types,and normalize into generic types (number instead of int*, etc)
     *  - Next, determine if we need to promote the RHS.  If so, promote.  Otherwise, throw an error
     *  - Look up the operator and see if its overridable.  If so, check the LHS for override method entry.  If exists, call.
     *  - Otherwise, execute standard library op.
     *
     * @param module
     * @return
     */
    llvm::Value* BinaryOpExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (std::getenv("CAJETA_TRACE_BINOP")) {
            llvm::errs() << "BinaryOpExpression::generateCode binaryOp=" << binaryOp
                         << " children.size=" << children.size() << "\n";
        }

        // Short-circuit ops need to evaluate rhs only conditionally — handle before the
        // upfront-evaluate path the other ops use.
        if (binaryOp == BINARY_OP_LOGAND || binaryOp == BINARY_OP_LOGOR) {
            llvm::Value* lhsVal = loadIfAlloca(module, children[0]->generateCode(module));
            llvm::Type* i1Ty = llvm::Type::getInt1Ty(*module->getLlvmContext());
            // Coerce lhs to i1 if it isn't already (e.g. an i32 from a comparison-less subexpr).
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
            // && evaluates rhs when lhs is true; || evaluates rhs when lhs is false.
            if (binaryOp == BINARY_OP_LOGAND) {
                builder->CreateCondBr(lhsVal, rhsBB, mergeBB);
            } else {
                builder->CreateCondBr(lhsVal, mergeBB, rhsBB);
            }
            builder->SetInsertPoint(rhsBB);
            llvm::Value* rhsVal = loadIfAlloca(module, children[1]->generateCode(module));
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

        // Indexed-assignment operator overload: `recv[idx] = value` on a
        // class with `operator[]= (idx_t, value_t)` defined dispatches
        // through it directly. Must short-circuit BEFORE the
        // unconditional `lhs = children[0]->generateCode(module)` below
        // — otherwise the LHS's ArrayIndexExpression would call
        // `operator[]` (the read form) for its side effects, only to
        // discard the result. Match the BinaryOpExpression operator-
        // dispatch shape used for `+` / `==` / etc.
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
                        if (idxType && valType) {
                            // Generate values fresh — recv as the `this`
                            // pointer for the call, idx and val as the
                            // operator's two named parameters. l-value
                            // coercion mirrors the read-side dispatch
                            // in ArrayIndexExpression::generateCode.
                            llvm::Value* recvVal = recvAst->generateCode(module);
                            llvm::Value* idxVal  = idxAst->generateCode(module);
                            llvm::Value* valVal  = valAst->generateCode(module);
                            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(recvVal)) {
                                recvVal = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(idxVal)) {
                                idxVal = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(valVal)) {
                                valVal = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            std::vector<ParameterEntry> entries;
                            entries.push_back(ParameterEntry(idxType, "", idxVal));
                            entries.push_back(ParameterEntry(valType, "", valVal));
                            std::string opName = "operator[]=";
                            if (auto m = recvClass->resolveMethod(opName,
                                    entries, /*isConstructor=*/false,
                                    /*floatingParams=*/false)) {
                                recvClass->invokeMethod(opName, entries,
                                    /*isConstructor=*/false, recvVal,
                                    /*callerModule=*/module);
                                // The expression's value is the assigned r-value
                                // (C/Java convention) so `if ((x = m[k]) ...)`
                                // and `m[k] = n[k] = v` chain correctly.
                                return valVal;
                            }
                        }
                    }
                }
            }
        }

        // P3 — definite-assignment: for a bare-identifier LHS of an
        // assignment, mark the name assigned BEFORE evaluating the LHS.
        // The identifier's generateCode would otherwise trip the NYA
        // check on its way to producing the slot address — but the LHS
        // of an assignment is a write target, not a read. Marking
        // pre-eval lets the LHS slot fetch succeed; the assignment that
        // follows then writes the value into the slot.
        if (binaryOp == BINARY_OP_ASSIGN && !children.empty()) {
            if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (auto sc = module->getScopeStack().peek()) {
                    sc->markAssigned(lhsId->getTextValue());
                }
            }
        }

        llvm::Value* lhs = children[0]->generateCode(module);
        llvm::Value* rhs = children[1]->generateCode(module);
        ExpressionPtr lhsAst = dynamic_pointer_cast<Expression>(children[0]);
        ExpressionPtr rhsAst = dynamic_pointer_cast<Expression>(children[1]);

        // Operator overloading: if LHS resolves to a class type with an
        // `operator<sym>` method (e.g. `operator+`, `operator==`), dispatch
        // through it before the built-in arithmetic path. RHS is passed as
        // the single non-this argument; the method's return value is the
        // expression's value. The lookup falls back through hierarchy via
        // resolveMethod (same machinery dispatch uses), so an operator
        // defined on a base class is visible to its subclasses.
        const char* opSym = nullptr;
        switch (binaryOp) {
            case BINARY_OP_ADD: opSym = "+"; break;
            case BINARY_OP_SUB: opSym = "-"; break;
            case BINARY_OP_MUL: opSym = "*"; break;
            case BINARY_OP_DIV: opSym = "/"; break;
            case BINARY_OP_MOD: opSym = "%"; break;
            case BINARY_OP_EQ:  opSym = "=="; break;
            case BINARY_OP_NE:  opSym = "!="; break;
            case BINARY_OP_LT:  opSym = "<";  break;
            case BINARY_OP_GT:  opSym = ">";  break;
            case BINARY_OP_LE:  opSym = "<="; break;
            case BINARY_OP_GE:  opSym = ">="; break;
            case BINARY_OP_BITAND: opSym = "&"; break;
            case BINARY_OP_BITOR:  opSym = "|"; break;
            case BINARY_OP_BITXOR: opSym = "^"; break;
            default: break;
        }
        if (opSym && lhsAst) {
            if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
            auto lhsClass = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
            if (lhsClass && !lhsClass->isInterface()
                    && !(lhsClass->getTypeFlags() & PRIMITIVE_FLAG)) {
                string opName = string("operator") + opSym;
                const bool fp = false;
                if (rhsAst && !rhsAst->getResolvedType()) {
                    rhsAst->resolveTypes(module);
                }
                CajetaTypePtr rhsType = rhsAst ? rhsAst->getResolvedType() : nullptr;
                if (!rhsType) rhsType = CajetaType::of(rhs);
                // resolveMethod's canonical computation calls
                // `parameter.type->toCanonical()` which crashes on null —
                // bail out if we still don't have a usable type rather than
                // attempt the lookup.
                if (!rhsType) {
                    goto fallthrough_to_builtin;
                }
                // l-value coercion: identifier expressions evaluate to the
                // alloca holding the heap pointer, but invokeMethod expects
                // the actual instance pointer (so the called function
                // receives `this` as a Counter*, not a Counter**).
                llvm::Value* recvVal = lhs;
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(recvVal)) {
                    recvVal = builder->CreateLoad(a->getAllocatedType(), a);
                }
                llvm::Value* rhsVal = rhs;
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(rhsVal)) {
                    rhsVal = builder->CreateLoad(a->getAllocatedType(), a);
                }
                vector<ParameterEntry> entries;
                entries.push_back(ParameterEntry(rhsType, "", rhsVal));
                if (auto m = lhsClass->resolveMethod(opName, entries,
                        /*isConstructor=*/false, /*floatingParams=*/fp)) {
                    return lhsClass->invokeMethod(opName, entries,
                        /*isConstructor=*/false, recvVal,
                        /*callerModule=*/module);
                }
            }
        }
        fallthrough_to_builtin:;

        long lhsTypeFlags = CajetaType::getTypeFlagsOf(lhs);
        long rhsTypeFlags = CajetaType::getTypeFlagsOf(rhs);
        llvm::Value* result = nullptr;

        // Replace bare loadIfAlloca with loadIfLValue passing the relevant ast where useful.
        auto loadL = [&](llvm::Value* v) { return loadIfLValue(module, v, lhsAst); };
        auto loadR = [&](llvm::Value* v) { return loadIfLValue(module, v, rhsAst); };
        (void) loadL; (void) loadR; // used selectively below

        // For non-assignment ops both sides are r-values; for assignment forms lhs is the
        // target address and only rhs needs r-value coercion.
        switch (binaryOp) {
            case BINARY_OP_ASSIGN: {
                // Struct-to-struct assignment: both sides are addresses of struct allocas;
                // emit a memcpy sized by the struct's allocation size from the data layout.
                // STRUCT_FLAG (a real flag bit) is the right discriminator; STRUCT_TYPE_ID
                // is the composite ID value and would false-positive on bitwise AND.
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
                        // For struct assignment, the expression yields the destination
                        // address (lvalue convention).
                        result = lhs;
                        break;
                    }
                }
                llvm::Value* rhsVal = loadR(rhs);
                // Coerce rhs to the destination's element type. Two slot shapes need
                // this: (a) plain local-variable allocas, where the alloca carries the
                // type directly; (b) GEPs into an array element, where the AST gives
                // us the element type. Both matter — without the GEP path a wide
                // integer literal like `xs[0] = 10` (i64 by default) writes 8 bytes
                // into a 4-byte slot and clobbers neighboring memory.
                llvm::Type* slotTy = nullptr;
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                    slotTy = a->getAllocatedType();
                } else if (lhsAst && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (auto elemType = lhsAst->getResolvedType()) {
                        // Class-typed array elements (CajetaArray,
                        // CajetaStruct, plain CajetaClass) — slot stores
                        // a pointer to the heap instance. Primitive
                        // arrays hold the value directly.
                        if (dynamic_pointer_cast<CajetaClass>(elemType) ||
                            (elemType->getTypeFlags() & STRUCT_FLAG)) {
                            slotTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                        } else if (llvm::Type* lt = elemType->getLlvmType()) {
                            slotTy = lt;
                        }
                    }
                } else if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    // `obj.field = value` — slot type is the field's declared
                    // type. Walk the inheritance chain to find the field; an
                    // inherited field lives on an ancestor's properties map,
                    // not the subclass's own. Without this walk, slotTy stays
                    // null for inherited writes and a wide-default integer
                    // literal (i64) stores past the slot's width into the
                    // next field. (#208 follow-up.)
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
                                    // Reject writes to variable-size struct
                                    // fields — they can't be resized in place.
                                    // See Structs.md § Mutation rules.
                                    // ONLY applies to CajetaStruct (POD zero-
                                    // copy) types — a String-typed field on a
                                    // regular CajetaClass instance is a normal
                                    // owned-pointer field, freely writable.
                                    // CajetaStruct's String fields, by
                                    // contrast, live inline in a wire-format
                                    // buffer with a length prefix.
                                    bool isViewStruct =
                                        dynamic_pointer_cast<CajetaView>(klass) != nullptr;
                                    if (isViewStruct
                                            && CajetaView::isVariableSize(found)) {
                                        char buf[256];
                                        snprintf(buf, sizeof(buf),
                                            "cannot reassign variable-size struct field '%s'; "
                                            "build a new buffer instead",
                                            found->getName().c_str());
                                        throw Exception(buf,
                                            "CAJETA_ERROR_VARSIZE_FIELD_ASSIGN");
                                    }
                                    // Array fields are stored as pointers in
                                    // the class layout (see CajetaClass::
                                    // generatePrototype's fieldLayoutType
                                    // rule). The slot is `ptr`, not the
                                    // inline `{ size, [0 x T] }` struct, so
                                    // `slotTy` must reflect that or the
                                    // store coerces a heap pointer down to
                                    // the struct's first element type and
                                    // overwrites only those bytes.
                                    if (dynamic_pointer_cast<CajetaArray>(found->getType())) {
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
                // Bswap on store when writing into a struct field whose
                // struct carries a non-host endianness annotation. Run the
                // coercion below first (so the value is the right width),
                // then the bswap converts host order → declared order.
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
                // For struct-field writes with non-host endianness, bswap the
                // (now slot-typed) value so the bytes in the buffer match the
                // declared wire order.
                if (needsFieldBswap) {
                    rhsVal = DotExpression::maybeBswap(module, rhsVal, dotRecv);
                }
                builder->CreateStore(rhsVal, lhs);
                // P3 — definite-assignment: if the LHS is a bare identifier,
                // mark it assigned. Subsequent reads no longer trip the
                // CAJETA_ERROR_VARIABLE_NOT_ASSIGNED check. Compound LHS
                // forms (a.b, arr[i]) don't apply — those mutate through a
                // receiver that was itself already assigned.
                if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                    if (auto sc = module->getScopeStack().peek()) {
                        sc->markAssigned(lhsId->getTextValue());
                    }
                }
                // The expression's value is the assigned r-value (C/Java convention),
                // not the StoreInst — that way `x = 100` can be used inside a ternary
                // or other surrounding expression.
                result = rhsVal;
                break;
            }
            case BINARY_OP_ADD: {
                llvm::Value* l = loadL(lhs);
                llvm::Value* r = loadR(rhs);
                // String concatenation: if either operand evaluates to a pointer
                // and neither side is an array, lower to __cajeta_str_concat,
                // auto-stringifying primitive operands first.
                bool lIsArr = lhsAst && dynamic_pointer_cast<CajetaArray>(lhsAst->getResolvedType());
                bool rIsArr = rhsAst && dynamic_pointer_cast<CajetaArray>(rhsAst->getResolvedType());
                bool lIsPtr = l->getType()->isPointerTy() && !lIsArr;
                bool rIsPtr = r->getType()->isPointerTy() && !rIsArr;
                if (lIsPtr || rIsPtr) {
                    auto& llvmCtx = *module->getLlvmContext();
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                    auto stringify = [&](llvm::Value* v) -> llvm::Value* {
                        llvm::Type* t = v->getType();
                        if (t->isPointerTy()) return v;
                        if (t->isIntegerTy(1)) {
                            llvm::Value* widened = builder->CreateZExt(v, i32Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                            return builder->CreateCall(fn, {widened});
                        }
                        if (t->isIntegerTy()) {
                            v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        if (t->isFloatingPointTy()) {
                            if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                    };
                    llvm::Value* ls = stringify(l);
                    llvm::Value* rs = stringify(r);
                    llvm::Function* concat = module->getRuntimeFunction("__cajeta_str_concat");
                    result = builder->CreateCall(concat, {ls, rs});
                    break;
                }
                auto [pl, pr] = coerceArithPair(module, l, r);
                result = pl->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, pl, pr, llvm::Instruction::FAdd)
                    : builder->CreateAdd(pl, pr);
                break;
            }
            case BINARY_OP_SUB: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = l->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, l, r, llvm::Instruction::FSub)
                    : builder->CreateSub(l, r);
                break;
            }
            case BINARY_OP_MUL: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = l->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, l, r, llvm::Instruction::FMul)
                    : builder->CreateMul(l, r);
                break;
            }
            case BINARY_OP_DIV: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFloatingPointTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                } else if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                    result = builder->CreateSDiv(l, r);
                } else {
                    result = builder->CreateUDiv(l, r);
                }
                break;
            }
            case BINARY_OP_BITAND: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateAnd(l, r);
                break;
            }
            case BINARY_OP_BITOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateOr(l, r);
                break;
            }
            case BINARY_OP_BITXOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateXor(l, r);
                break;
            }
            case BINARY_OP_SHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateAShr(l, r);
                break;
            }
            case BINARY_OP_USHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateLShr(l, r);
                break;
            }
            case BINARY_OP_SHIFTLEFT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateShl(l, r);
                break;
            }
            case BINARY_OP_MOD: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFloatingPointTy()) {
                    result = builder->CreateFRem(l, r);
                } else if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                    result = builder->CreateSRem(l, r);
                } else {
                    result = builder->CreateURem(l, r);
                }
                break;
            }
            // Compound assignments: compute at the wider type, then truncate back to the
            // alloca's element type before storing.
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                llvm::Value* newVal = nullptr;
                bool isFp = l->getType()->isFloatingPointTy();
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0;
                switch (binaryOp) {
                    case BINARY_OP_ADD_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FAdd)
                                      : builder->CreateAdd(l, r);
                        break;
                    case BINARY_OP_SUB_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FSub)
                                      : builder->CreateSub(l, r);
                        break;
                    case BINARY_OP_MUL_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FMul)
                                      : builder->CreateMul(l, r);
                        break;
                    case BINARY_OP_DIV_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                        else if (isSigned) newVal = builder->CreateSDiv(l, r);
                        else newVal = builder->CreateUDiv(l, r);
                        break;
                    case BINARY_OP_BITAND_EQUALS:     newVal = builder->CreateAnd(l, r);  break;
                    case BINARY_OP_BITOR_EQUALS:      newVal = builder->CreateOr(l, r);   break;
                    case BINARY_OP_BITXOR_EQUALS:     newVal = builder->CreateXor(l, r);  break;
                    case BINARY_OP_SHIFTRIGHT_EQUALS: newVal = builder->CreateAShr(l, r); break;
                    case BINARY_OP_USHIFTRIGHT_EQUALS:newVal = builder->CreateLShr(l, r); break;
                    case BINARY_OP_SHIFTLEFT_EQUALS:  newVal = builder->CreateShl(l, r);  break;
                    case BINARY_OP_MOD_EQUALS:
                        newVal = isSigned ? builder->CreateSRem(l, r) : builder->CreateURem(l, r);
                        break;
                    default: break;
                }
                // Narrow back to the alloca element type if the wider arithmetic produced
                // a wider value than lhs's slot.
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                bool isFp = l->getType()->isFloatingPointTy();
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0;
                if (isFp) {
                    switch (binaryOp) {
                        case BINARY_OP_LT: result = builder->CreateFCmpOLT(l, r); break;
                        case BINARY_OP_LE: result = builder->CreateFCmpOLE(l, r); break;
                        case BINARY_OP_GT: result = builder->CreateFCmpOGT(l, r); break;
                        case BINARY_OP_GE: result = builder->CreateFCmpOGE(l, r); break;
                        case BINARY_OP_EQ: result = builder->CreateFCmpOEQ(l, r); break;
                        case BINARY_OP_NE: result = builder->CreateFCmpONE(l, r); break;
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
                // Handled at the top of the function via short-circuit branching.
                break;
        }
        return result;
    }

} // code