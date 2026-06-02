//
// Created by James Klappenbach on 4/8/23.
//

#include "BinaryOpExpression.h"
#include "../../error/CajetaExceptions.h"
#include "../../error/Exception.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaView.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaVector.h"
#include "../../type/VectorOps.h"
#include "../../util/MemoryManager.h"
#include "Expression.h"
#include "DotExpression.h"
#include "MethodCallExpression.h"
#include "Identifier.h"
#include "AggregateInitializerExpression.h"
#include "NewExpression.h"
#include "LiteralExpression.h"

#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Emit `if (condTrap) { llvm.trap; unreachable; }` followed by an
    // OK basic-block that the IRBuilder lands in. Used to guard
    // would-be UB (divide-by-zero, oversized-shift) when
    // CompilerFlags::ubTraps is on. Caller pre-computes the trap
    // predicate from operand values. Off-by-default in Release/Fast/
    // Minimal modes per CompilerModes.md; on by default in Debug.
    void emitUbTrap(CajetaModulePtr module,
                           llvm::IRBuilder<>& b,
                           llvm::Value* condTrap,
                           const std::string& label) {
        if (!condTrap) return;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::Function* curFn = b.GetInsertBlock()->getParent();
        auto* trapBB = llvm::BasicBlock::Create(ctx, label + ".trap", curFn);
        auto* okBB = llvm::BasicBlock::Create(ctx, label + ".ok", curFn);
        b.CreateCondBr(condTrap, trapBB, okBB);
        b.SetInsertPoint(trapBB);
        llvm::Function* trapFn = llvm::Intrinsic::getOrInsertDeclaration(
            lmod, llvm::Intrinsic::trap);
        b.CreateCall(trapFn);
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    // Emit a signed-overflow-checked binary op via the matching
    // llvm.s{add,sub,mul}.with.overflow intrinsic. The intrinsic
    // returns a {iN, i1}; we extract field 1 (the overflow bit),
    // route through emitUbTrap, and return field 0 (the wrapping
    // result). Operands must be the same integer type. Used by
    // ADD/SUB/MUL when CompilerFlags::overflowChecks is On AND the
    // operand type is signed (per OverflowChecks::On docs in
    // CompilerModes.md). Wrapping / Off modes bypass this helper
    // entirely and use the plain CreateAdd/Sub/Mul path.
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
                // Class-typed elements (CajetaArray, plain CajetaClass)
                // are stored as pointers in the array's data slot.
                // Loading the slot yields the heap reference, not the
                // instance contents. CajetaArray inherits from CajetaClass,
                // so the dynamic_cast catches both. Primitives load as
                // their value type.
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
        // fields (Views.md § Inline length-prefix layout), where
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
                // Reference-typed fields — arrays and plain class refs —
                // are stored in the parent struct as `ptr` (see
                // CajetaClass::generatePrototype's fieldLayoutType
                // rule), not as the inline header / body struct.
                // Loading the slot must use ptr, not the body type,
                // or (a) we'd read past the slot into neighbor bytes
                // for multi-pointer bodies, and (b) for template-
                // instantiation field types the loaded value would be
                // an inline struct (e.g. `%union.anon` for
                // `Stream<int32>`) that doesn't unify with `ptr` at
                // function-arg slots (verify failure). The CajetaArray
                // and parent-is-struct branches were the predecessors;
                // the unified rule below subsumes both. Interfaces
                // (24-byte fat pointer body) and CajetaView (zero-copy
                // overlays) keep inline storage and load as the body
                // type — handled by the catch-all `loadTy =
                // resolved->getLlvmType()` after the explicit rejects.
                llvm::Type* loadTy;
                bool fieldIsClassRef = dynamic_pointer_cast<CajetaClass>(resolved) != nullptr
                    && !dynamic_pointer_cast<CajetaView>(resolved)
                    && !dynamic_pointer_cast<CajetaArray>(resolved);
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
                    // Static fields land here too: DotExpression returns
                    // the GlobalVariable* for `Counter.total`, which is a
                    // pointer slot — load through it the same way as a
                    // GEP'd field slot. The bswap pass only fires for
                    // view-typed parent receivers; class-name receivers
                    // (statics) never carry an endianness annotation, so
                    // maybeBswap is a no-op there.
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
                // DotExpression returned something other than a slot —
                // e.g. `__cajeta_str_view_to_owned`'s call result for
                // a view-materialized String field, or the raw data
                // pointer for a `T[]` view field. That value IS the
                // language-level result; the GEP/GlobalVariable gate
                // above intentionally skips the load. Bail out before
                // the class-ref catch-all below tries to load through
                // it and hands back the vtable word.
                return v;
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
        // MethodCallExpression: the return value of a call IS the
        // language-level value. For class returns the callee returns
        // `ptr` (per Method::generatePrototype's pass-by-pointer
        // convention), and the pointer IS the instance reference —
        // not a slot to load through. Without this pre-empt the
        // catch-all `loadTy != v->getType()` branch below would see
        // resolvedType's body struct type, decide loadTy differs from
        // ptr, and load the entire body through the pointer — handing
        // back a struct value that won't pass as a `ptr` arg at a
        // ctor / method call site. NewExpression has the same carve-
        // out for the same reason.
        if (dynamic_pointer_cast<MethodCallExpression>(ast)) {
            return v;
        }
        // Phase 2b-β — string-literal value IS the global's address (a
        // class String instance materialized in static storage). Same
        // carve-out shape as NewExpression / MethodCallExpression: the
        // pointer IS the language-level value, not a slot to load
        // through. The class-ref catch-all below would otherwise load
        // through `@.str.inst` and hand back the vtable word as if it
        // were the instance pointer, which corrupts every downstream
        // use of the literal as a class String.
        if (auto tle = dynamic_pointer_cast<TextLiteralExpression>(ast)) {
            if (auto rt = tle->getResolvedType()) {
                if (dynamic_pointer_cast<CajetaClass>(rt)) {
                    return v;
                }
            }
        }
        // Phase 2b-γ — String concat result. BinaryOp `+` on String operands
        // returns a freshly allocated class String pointer (BINARY_OP_ADD
        // wraps `__cajeta_str_concat`'s char* output back into a class
        // String shell). The pointer IS the language-level value, same
        // shape as NewExpression and MethodCallExpression returns. Without
        // this pre-empt the class-ref catch-all below would load through
        // the malloc'd struct address and hand back the vtable word (the
        // first 8 bytes of the struct) instead of the instance reference.
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
        // Any LLVM CallInst result. Methods returning class types return
        // `ptr` per the class-pass-by-pointer convention; the pointer IS
        // the language-level value, NOT a slot to load through. The
        // MethodCallExpression carve-out above covers direct calls (`obj
        // .method()`), but operator overloads dispatch through a
        // BinaryOpExpression / ArrayIndexExpression that wraps the same
        // call — those wrappers ARE the ast and don't match the
        // MethodCallExpression check. Catch the CallInst itself instead;
        // cajeta methods always return values, not slots, so loading
        // through a CallInst result is always wrong.
        //
        // Symptom before this fix: `Vec r = a + b - c` where `+`/`-`
        // are mutating operators returning `this` (a borrow of acc)
        // produced IR that loaded `acc.vtable` and used it as `-`'s
        // receiver — segfault in AOT, accidental "pass" in JIT
        // because LLJIT places globals on writable pages.
        if (llvm::isa<llvm::CallInst>(v)) {
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
                    // Plain class ref: per the class-pass-by-pointer
                    // rule (Method.cpp:537 + ParameterField+
                    // LocalVariableDeclaration's slot-shape choice),
                    // a class-typed slot holds a `ptr`. Loading the
                    // slot should use `ptr`, not the inline struct
                    // shape that resolved->getLlvmType() returns —
                    // otherwise we'd read the struct's first word out
                    // through the heap pointer and hand that back as
                    // if it were the instance reference. The catch-
                    // all branch below loads with `loadTy = struct`,
                    // which trips downstream call sites that expect
                    // a `ptr` arg (`Call parameter type does not
                    // match function signature!`). Carve out the
                    // class-ref case explicitly so the catch-all
                    // doesn't mis-fire.
                    llvm::Type* ptrTy = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                    if (v->getType() == ptrTy) {
                        // ThisExpression's alloca holds a ptr; loading
                        // gives back the ptr. Same for any class-typed
                        // local or field slot.
                        return builder->CreateLoad(ptrTy, v);
                    }
                    // If v isn't even a ptr, fall through — something
                    // weirder is going on; let the catch-all handle it
                    // (or fail loudly via the verifier).
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

        // Vector broadcast: a `vec op scalar` / `scalar op vec` splats the
        // scalar to the vector's shape so the element-wise op sees two
        // same-shape vectors. Same-shape vectors hit the `lt == rt` fast path
        // above. (Vector<T,N> is the Item-8-follow-on value type.)
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
        if (std::getenv("CAJETA_TRACE_BINOP") || std::getenv("CAJETA_DEBUG_ICMP")) {
            llvm::errs() << "BinaryOpExpression::generateCode binaryOp=" << binaryOp
                         << " children.size=" << children.size() << "\n";
        }

        // Short-circuit ops need to evaluate rhs only conditionally — handle before the
        // upfront-evaluate path the other ops use.
        if (binaryOp == BINARY_OP_LOGAND || binaryOp == BINARY_OP_LOGOR) {
            // loadIfLValue (rather than the bare loadIfAlloca) so an
            // ArrayIndex / DotExpression operand load-throughs to its
            // element value before the i1 coercion. Without this,
            // `boolean[] a; ... if (a[0] && a[1])` evaluates the LHS
            // to the GEP-ptr and the icmpNE-with-zero below tries to
            // emit `icmp ne ptr, integer 0`, which trips LLVM's
            // same-type-operands assertion.
            auto lhsAst = dynamic_pointer_cast<Expression>(children[0]);
            llvm::Value* lhsVal = loadIfLValue(
                module, children[0]->generateCode(module), lhsAst);
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
            // Same lvalue load-through as the LHS path — see comment above.
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

        // Gap 4 alias-mutation guard — REMOVED.
        //
        // This used to reject `node.field = ...` whenever an earlier
        // `local = node.field` had recorded a live read-borrow, on the
        // theory that overwriting the slot would dangle `local`. But a
        // field reassignment emits NO eager drop (there is no free/drop
        // in this BINARY_OP_ASSIGN path) — a node is freed only at the
        // scope boundary, via its own drop-chain entry, not by the act of
        // reassigning a reference to it. So the write never frees the
        // borrowed node, `local` cannot dangle, and the guard was a false
        // positive that rejected valid ownership transfers (e.g. tree
        // rotations, `y = x.right; x.right = y.left;`) and intentional
        // replacement of an owned field. Drop-exactly-once soundness is
        // the scope-exit drop chain's responsibility, not this write site.
        //
        // Genuine use-after-`#`-move reads are still caught in
        // Identifier.cpp / DotExpression.cpp via movedNames / movedPaths.

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
                CajetaTypePtr lhsType = lhsAst->getResolvedType();
                // resolveMethod's canonical computation calls
                // `parameter.type->toCanonical()` which crashes on null —
                // bail out if we still don't have a usable type rather than
                // attempt the lookup.
                if (!rhsType || !lhsType) {
                    goto fallthrough_to_builtin;
                }
                // l-value coercion (same shape that single-arg dispatch
                // used): identifier expressions evaluate to the alloca
                // holding the heap pointer; array-index / dot-field
                // expressions evaluate to a GEP whose slot holds the
                // heap pointer. Static `operator+(LHS, RHS)` expects the
                // language-level instance values; route both through
                // loadIfLValue so neither operand passes through as a
                // slot pointer.
                llvm::Value* lhsVal = loadIfLValue(module, lhs, lhsAst);
                llvm::Value* rhsVal = loadIfLValue(module, rhs, rhsAst);
                // Cajeta binary operator overloads are static (per
                // cajeta-docs/OperatorOverloading.md §2): both operands
                // are explicit parameters, no `this`. Build the entries
                // vector with LHS first, RHS second, and invoke with a
                // null receiver — invokeMethod's `isStatic` branch
                // (CajetaClass.cpp ~line 3863) skips the implicit
                // `this` prepend when the resolved method carries the
                // STATIC modifier.
                vector<ParameterEntry> entries;
                entries.push_back(ParameterEntry(lhsType, "", lhsVal));
                entries.push_back(ParameterEntry(rhsType, "", rhsVal));
                if (auto m = lhsClass->resolveMethod(opName, entries,
                        /*isConstructor=*/false, /*floatingParams=*/fp)) {
                    return lhsClass->invokeMethod(opName, entries,
                        /*isConstructor=*/false,
                        /*thisInstance=*/nullptr,
                        /*callerModule=*/module);
                }

                // Comparison derivations — when the direct lookup misses,
                // synthesize the result from a related operator. Per
                // cajeta-docs/OperatorOverloading.md §7:
                //   - `a != b`  ≡  !(a == b)
                //   - `a >  b`  ≡  (b <  a)            [swap operands]
                //   - `a >= b`  ≡  !(a <  b)
                //   - `a <= b`  ≡  !(b <  a)
                // The < / > / <= / >= derivations assume the user's `<`
                // defines a total order; users with partial orderings
                // declare each operator explicitly. (Same assumption
                // C++ and Rust make for std::less / Ord.)
                auto tryDerivation = [&](const char* baseSym,
                                         bool swapOperands,
                                         bool negateResult) -> llvm::Value* {
                    std::string baseName = std::string("operator") + baseSym;
                    vector<ParameterEntry> ents;
                    if (swapOperands) {
                        ents.push_back(ParameterEntry(rhsType, "", rhsVal));
                        ents.push_back(ParameterEntry(lhsType, "", lhsVal));
                    } else {
                        ents.push_back(ParameterEntry(lhsType, "", lhsVal));
                        ents.push_back(ParameterEntry(rhsType, "", rhsVal));
                    }
                    if (!lhsClass->resolveMethod(baseName, ents,
                            /*isConstructor=*/false, /*floatingParams=*/fp)) {
                        return nullptr;
                    }
                    llvm::Value* baseResult = lhsClass->invokeMethod(
                        baseName, ents,
                        /*isConstructor=*/false,
                        /*thisInstance=*/nullptr,
                        /*callerModule=*/module);
                    if (!baseResult) return nullptr;
                    if (negateResult) {
                        // The base operator returns boolean (i1).
                        // CreateNot on i1 is the boolean negation.
                        return builder->CreateNot(baseResult,
                            std::string("derived.") + opSym);
                    }
                    return baseResult;
                };
                if (binaryOp == BINARY_OP_NE) {
                    if (auto* v = tryDerivation("==", /*swap=*/false, /*neg=*/true)) return v;
                } else if (binaryOp == BINARY_OP_GT) {
                    if (auto* v = tryDerivation("<",  /*swap=*/true,  /*neg=*/false)) return v;
                } else if (binaryOp == BINARY_OP_GE) {
                    if (auto* v = tryDerivation("<",  /*swap=*/false, /*neg=*/true)) return v;
                } else if (binaryOp == BINARY_OP_LE) {
                    if (auto* v = tryDerivation("<",  /*swap=*/true,  /*neg=*/true)) return v;
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
                // Vector component/index assignment: `v.x = e` / `v[i] = e`.
                // The vector lives in a slot reached via the base sub-expression
                // (an l-value: a local alloca or a field GEP). Load the `<N x T>`,
                // insertelement at the lane, store back. lhsAst's own generateCode
                // yields the extracted element value, not a slot, so we bypass it
                // and re-evaluate the base as an l-value.
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
                // Interface-typed FIELD assignment (`this.enc = e`). An
                // interface field stores the 24-byte {data, vtable, kind}
                // body INLINE in the parent struct (S9.5.4 / CajetaClass::
                // generatePrototype), whereas an interface VALUE — a param,
                // local, or field read — is represented as a *pointer* to
                // such a body (loadR returns that pointer). A plain store
                // would write only the 8-byte data word into the field and
                // leave its vtable/kind zero, so every later dispatch through
                // the field reads a null vtable and segfaults. memcpy the
                // whole body instead. Only fields (DotExpression LHS) are
                // inline; interface LOCALS are pointer slots and take the
                // normal pointer store below, so this is gated on DotExpression.
                // `x = null` clears the body (memset 0) — supports the
                // assignment-based drop idiom for interface fields.
                if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsCls = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
                    if (lhsCls && lhsCls->isInterface()) {
                        llvm::Type* ifaceTy = lhsAst->getResolvedType()->getLlvmType();
                        if (ifaceTy && ifaceTy->isStructTy()) {
                            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                            llvm::Value* size = llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                dl.getTypeAllocSize(ifaceTy));
                            llvm::Align align(dl.getABITypeAlign(ifaceTy));
                            llvm::Value* srcBody = loadR(rhs);
                            if (llvm::isa<llvm::ConstantPointerNull>(srcBody)) {
                                builder->CreateMemSet(lhs,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt8Ty(*module->getLlvmContext()), 0),
                                    size, align);
                            } else {
                                builder->CreateMemCpy(lhs, align, srcBody, align, size);
                            }
                            result = lhs;
                            break;
                        }
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
                        // Class-typed array elements (CajetaArray, plain
                        // CajetaClass) — slot stores a pointer to the
                        // heap instance. Primitive arrays hold the
                        // value directly.
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
                                    // Reject writes to variable-size view
                                    // fields — they can't be resized in place.
                                    // See Views.md § Mutation rules. Only
                                    // applies to CajetaView (wire-format
                                    // overlay) — a String-typed field on a
                                    // plain CajetaClass instance is a normal
                                    // owned-pointer field, freely writable.
                                    // View String fields, by contrast, live
                                    // inline in the byte buffer with a
                                    // length prefix.
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
                                    // Reference-typed fields — arrays and
                                    // plain class refs — are stored as
                                    // pointers in the class layout (see
                                    // CajetaClass::generatePrototype's
                                    // fieldLayoutType rule). The slot is
                                    // `ptr`, not the inline `{ size, [0 x T] }`
                                    // (array) / class body, so `slotTy`
                                    // must reflect that or the store coerces
                                    // the heap pointer down to the body's
                                    // first element type and overwrites
                                    // only those bytes. Views (zero-copy
                                    // overlays) and interfaces (24-byte
                                    // fat pointers) keep inline storage.
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
                // L-03 polymorphism / MI upcast at assignment site. When
                // the LHS slot's static type is an ancestor of the RHS
                // expression's class type AND the ancestor's sub-object
                // sits at a non-zero offset inside the descendant's
                // layout (non-first-parent path), shift the stored
                // pointer to that sub-object's start so subsequent
                // virtual dispatch through the LHS-typed binding lands
                // on the right secondary vtable + correct field offsets.
                // Mirrors the LocalVariableDeclaration upcast (Phase 1
                // poly-MI) — same helper, applied at the write site.
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
                builder->CreateStore(rhsVal, lhs);
                // Ownership transfer into a class-typed array slot.
                // When `arr[i] = local` stores a heap-owned class
                // pointer, the slot now owns the reference; the
                // source local's drop must NOT fire at end-of-scope
                // or end-of-iteration (which would free the instance
                // whose pointer arr[i] still holds — leaving it
                // dangling). Mirrors AggregateInitializer's
                // ownership-into-field move and the lambda
                // #-capture transfer paths.
                if (lhsAst && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)
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
                    if (elemStoresAsPointer) {
                        if (auto idExpr =
                                dynamic_pointer_cast<IdentifierExpression>(rhsAst)) {
                            if (auto sc = module->getScopeStack().peek()) {
                                FieldPtr srcField = sc->getField(
                                    idExpr->getTextValue());
                                if (srcField) {
                                    if (llvm::Value* entry =
                                            srcField->getDropEntry()) {
                                        if (llvm::Function* mark =
                                                module->getRuntimeFunction(
                                                    "__cajeta_drop_mark_inactive")) {
                                            builder->CreateCall(mark, {entry});
                                        }
                                    }
                                    // Intentionally NOT calling
                                    // sc->markMoved here. The source
                                    // local often goes on to be
                                    // reassigned in the next loop
                                    // iteration (e.g. `piece = source
                                    // .trySplit()` in reduceParallel)
                                    // or is a parameter that gets
                                    // read again in unrelated paths
                                    // (HashMap.put's `this.keys[i] =
                                    // key` followed by future probes
                                    // against `key`'s field). The
                                    // dropEntry deactivation is the
                                    // necessary half; markMoved is
                                    // too aggressive for the
                                    // ownership-transfer-into-array
                                    // shape.
                                }
                            }
                        }
                    }
                }
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
                // auto-stringifying primitive operands first. After Phase 2b-γ,
                // class String operands are unwrapped to their underlying C-string
                // (bytes.data, which the literal codegen guarantees is
                // null-terminated) for the concat call, and the malloc'd char*
                // result is re-wrapped in a fresh class String instance — so the
                // expression's value type matches the LHS slot type at the
                // assignment site without a hidden coercion. The wrap allocates
                // both a CajetaArray byte buffer (copy of the concat result) and
                // the class String header, then frees the intermediate char*;
                // class String follows the never-drop rule so neither shows up
                // in the drop chain.
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

                    // Class String setup — used both for unwrap (read-side) and
                    // wrap (result-side). When the class isn't loaded yet
                    // (bootstrap window during runtime parse), fall back to the
                    // legacy raw-pointer path: stringify produces char* and the
                    // concat returns char* as before. User code post-bootstrap
                    // always sees the class form.
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
                    // Unwrap class String → char* via the bytes field's data
                    // region. The bytes field at struct slot 1 points at a
                    // CajetaArray-shaped header { i64 count, [N x i8] data };
                    // skip 8 bytes past the count to land on the data pointer.
                    // Literal codegen guarantees null termination so any
                    // strlen-based concat helper sees the right end.
                    auto extractCStr = [&](llvm::Value* sptr) -> llvm::Value* {
                        if (!stringStructTy) return sptr;
                        llvm::Value* bytesSlot = builder->CreateStructGEP(
                            stringStructTy, sptr, 1, "concat.bytes_slot");
                        llvm::Value* bytesPtr = builder->CreateLoad(
                            ptrTy, bytesSlot, "concat.bytes_ptr");
                        return builder->CreateInBoundsGEP(i8Ty, bytesPtr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "concat.cstr");
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

                    // Ensure both sides have resolved types before
                    // dispatching to stringify. The operator-method
                    // path above (~line 564) only runs when an opSym
                    // / opMethod hits — for a primitive-plus-String
                    // shape (no user-defined operator+), neither side
                    // gets resolveTypes called proactively, and the
                    // class-String unwrap below misses (`isClassStringType`
                    // sees a null resolvedType).
                    if (lhsAst && !lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    if (rhsAst && !rhsAst->getResolvedType()) rhsAst->resolveTypes(module);
                    CajetaTypePtr lhsRT = lhsAst ? lhsAst->getResolvedType() : nullptr;
                    CajetaTypePtr rhsRT = rhsAst ? rhsAst->getResolvedType() : nullptr;
                    llvm::Value* ls = stringify(l, lhsRT);
                    llvm::Value* rs = stringify(r, rhsRT);
                    llvm::Function* concat = module->getRuntimeFunction("__cajeta_str_concat");
                    llvm::Value* concatResult = builder->CreateCall(concat, {ls, rs});

                    if (!stringStructTy || !stringKlass) {
                        // Bootstrap fallback (class String not loaded yet).
                        result = concatResult;
                        break;
                    }

                    // Wrap concatResult (malloc'd char*) into a fresh class
                    // String. Layout matches generatePrototype's embed order:
                    //   { ptr vtable, ptr bytes, i32 byteLength, i32 mode, i32 cachedCpLength }
                    llvm::Function* strlenFn = module->getRuntimeFunction("__cajeta_str_len");
                    llvm::Value* lenI64 = builder->CreateCall(
                        strlenFn, {concatResult}, "concat.len");
                    llvm::Value* lenI32 = builder->CreateIntCast(
                        lenI64, i32Ty, /*isSigned=*/true, "concat.len32");

                    // CajetaArray header sized 8 + len + 1 (count word + bytes
                    // + null terminator for forward compatibility with any
                    // strlen-reader that hits the buffer through `.bytes.data`).
                    // Direct call to __cajeta_alloc (the runtime live-set-aware
                    // allocator) because MemoryManager::createMallocInstruction
                    // requires a Constant size; here the size is computed at
                    // runtime from the concat result's strlen.
                    llvm::Value* arrSize = builder->CreateAdd(lenI64,
                        llvm::ConstantInt::get(i64Ty, 9), "concat.arr_size");
                    llvm::FunctionType* allocTy = llvm::FunctionType::get(
                        ptrTy, {i64Ty}, false);
                    llvm::FunctionCallee allocFn =
                        module->getLlvmModule()->getOrInsertFunction(
                            "__cajeta_alloc", allocTy);
                    llvm::Value* arrPtr = builder->CreateCall(
                        allocFn, {arrSize}, "concat.arr_alloc");
                    builder->CreateMemSet(arrPtr,
                        llvm::ConstantInt::get(i8Ty, 0),
                        arrSize, llvm::MaybeAlign(8));
                    builder->CreateStore(lenI64, arrPtr);
                    llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                        i8Ty, arrPtr,
                        llvm::ConstantInt::get(i64Ty, 8),
                        "concat.arr_data");
                    builder->CreateMemCpy(dataPtr, llvm::MaybeAlign(1),
                        concatResult, llvm::MaybeAlign(1), lenI64);

                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Constant* sSize = llvm::ConstantInt::get(
                        i64Ty, dl.getTypeAllocSize(stringStructTy));
                    llvm::Value* sPtr = MemoryManager::createMallocInstruction(
                        module, sSize, builder->GetInsertBlock());

                    llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = stringKlass->getVirtualTableGlobal()) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    }
                    builder->CreateStore(vtableRef,
                        builder->CreateStructGEP(stringStructTy, sPtr, 0,
                            "concat.s_vtable"));
                    builder->CreateStore(arrPtr,
                        builder->CreateStructGEP(stringStructTy, sPtr, 1,
                            "concat.s_bytes"));
                    builder->CreateStore(lenI32,
                        builder->CreateStructGEP(stringStructTy, sPtr, 2,
                            "concat.s_byteLength"));
                    builder->CreateStore(llvm::ConstantInt::get(i32Ty, 0),
                        builder->CreateStructGEP(stringStructTy, sPtr, 3,
                            "concat.s_mode"));
                    builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1),
                        builder->CreateStructGEP(stringStructTy, sPtr, 4,
                            "concat.s_cachedCpLength"));

                    // Free the malloc'd intermediate char* — we copied its
                    // bytes into the CajetaArray header above. The class
                    // String now owns the byte buffer.
                    if (llvm::Function* freeFn =
                            module->getRuntimeFunction("__cajeta_free")) {
                        builder->CreateCall(freeFn, {concatResult});
                    }

                    // Pin resolvedType so a caller using this concat as a
                    // ctor / method argument can recover the class type
                    // instead of the generic `pointer` fallback (mirrors
                    // NewExpression and MethodCallExpression).
                    resolvedType = stringTy;
                    result = sPtr;
                    break;
                }
                // Operand-side signed-ness from the AST's resolvedType
                // (not getTypeFlagsOf, which keys on the LLVM type and
                // can't distinguish int32 from uint32 — both lower to
                // i32). Used by the +/-/* overflow-checked path below.
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    auto pick = [](ExpressionPtr e) -> long {
                        if (!e) return 0;
                        auto t = e->getResolvedType();
                        return t ? (long) t->getTypeFlags() : 0;
                    };
                    return ((pick(a) | pick(b)) & SIGNED_FLAG) != 0;
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    auto pick = [](ExpressionPtr e) -> long {
                        if (!e) return 0;
                        auto t = e->getResolvedType();
                        return t ? (long) t->getTypeFlags() : 0;
                    };
                    return ((pick(a) | pick(b)) & SIGNED_FLAG) != 0;
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    auto pick = [](ExpressionPtr e) -> long {
                        if (!e) return 0;
                        auto t = e->getResolvedType();
                        return t ? (long) t->getTypeFlags() : 0;
                    };
                    return ((pick(a) | pick(b)) & SIGNED_FLAG) != 0;
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                } else {
                    if (module->getFlags().ubTraps) {
                        llvm::Value* zero = llvm::Constant::getNullValue(r->getType());
                        llvm::Value* isZero = builder->CreateICmpEQ(r, zero, "ubt.div.z");
                        emitUbTrap(module, *builder, isZero, "div");
                    }
                    if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                        result = builder->CreateSDiv(l, r);
                    } else {
                        result = builder->CreateUDiv(l, r);
                    }
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
                if (module->getFlags().ubTraps) {
                    unsigned width = l->getType()->getScalarSizeInBits();
                    llvm::Value* widthC = llvm::ConstantInt::get(r->getType(), width);
                    // Unsigned cmp catches both r >= width and (signed) r < 0
                    // since negative i32 becomes a huge unsigned value.
                    llvm::Value* bad = builder->CreateICmpUGE(r, widthC, "ubt.shr.over");
                    emitUbTrap(module, *builder, bad, "shr");
                }
                result = builder->CreateAShr(l, r);
                break;
            }
            case BINARY_OP_USHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFPOrFPVectorTy()) {
                    result = builder->CreateFRem(l, r);
                } else {
                    if (module->getFlags().ubTraps) {
                        llvm::Value* zero = llvm::Constant::getNullValue(r->getType());
                        llvm::Value* isZero = builder->CreateICmpEQ(r, zero, "ubt.mod.z");
                        emitUbTrap(module, *builder, isZero, "mod");
                    }
                    if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                        result = builder->CreateSRem(l, r);
                    } else {
                        result = builder->CreateURem(l, r);
                    }
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
                // Operator-overload dispatch for compound assignment on
                // class-typed LHS (cajeta-docs/OperatorOverloading.md §6).
                // Lookup order:
                //   1. Explicit instance `operator+=` on LHS class —
                //      mutates `this` in place.
                //   2. Else derive from the static binary form:
                //      `a += b` → `a = T.operator+(a, b)`; the static
                //      binary returns a fresh value which we store back
                //      into the LHS slot.
                //   3. Else fall through to the primitive arithmetic
                //      path below (which the class lookup miss leaves
                //      well-defined for non-class operands).
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
                            // (1) Explicit instance form: lhs.operator+=(rhs)
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
                            // (2) Derive from binary: lhs = T.operator+(lhs, rhs)
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
                                    // Store the binary op's result back
                                    // into the LHS slot. `lhs` is the slot
                                    // (alloca / GEP / field address) per
                                    // the assignment-expression contract.
                                    builder->CreateStore(newVal, lhs);
                                    result = newVal;
                                    break;
                                }
                            }
                        }
                    }
                }
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                llvm::Value* newVal = nullptr;
                bool isFp = l->getType()->isFloatingPointTy();
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0;
                // Signed-overflow check for the arithmetic compound ops
                // mirrors the standalone +/-/× path. Sign is read from
                // the AST's resolvedType so a uint*-typed lhs/rhs skips
                // the check (modular wrap is well-defined for unsigned).
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    auto pick = [](ExpressionPtr e) -> long {
                        if (!e) return 0;
                        auto t = e->getResolvedType();
                        return t ? (long) t->getTypeFlags() : 0;
                    };
                    return ((pick(a) | pick(b)) & SIGNED_FLAG) != 0;
                };
                bool emitOfTrap = !isFp && l->getType()->isIntegerTy()
                    && module->getFlags().overflowChecks == OverflowChecks::On
                    && signedFromAst(lhsAst, rhsAst);
                // For the compound-arith case, narrow operands to the
                // lhs slot's width BEFORE the op so the overflow check
                // fires at the destination type's edge. coerceArithPair
                // widens both to the larger integer type, which would
                // miss e.g. `int32 a; a += 1;` — `1` is i64-literal,
                // both get widened to i64, no i64 overflow at INT32_MAX
                // + 1, then the result truncates back to i32 silently.
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