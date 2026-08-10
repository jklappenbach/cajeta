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
#include "../../type/VectorOps.h"
#include "../../type/CajetaMatrix.h"
#include "../../type/CajetaQuaternion.h"
#include "../../type/QuaternionOps.h"
#include "../../type/MatrixOps.h"
#include "../../util/MemoryManager.h"
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
        // A per-lane vector trap condition (a `Vector<T,N>` shift/mod/div bound
        // check yields `<N x i1>`) can't drive a branch — reduce it to a scalar
        // "any lane traps" i1 first. Without this the verifier rejects
        // `br <N x i1> ...` (the chained-Vector codegen crash under ub-traps).
        if (condTrap->getType()->isVectorTy()) {
            condTrap = b.CreateOrReduce(condTrap);
        }
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

    // Store an interface value into an INLINE 24-byte fat-pointer body slot — an
    // interface-typed array element (`arr[i] = ...`) whose slot IS the body, not a
    // pointer to it. A plain `CreateStore(rhsVal, slot)` would write only the first
    // word (the data pointer), leaving vtable + kind unset, so a later dispatch
    // reads a garbage vtable and crashes. Two RHS shapes:
    //   - concrete class instance → build the fat body in place
    //     (data = instance ptr, vtable = the per-(class, iface) global, kind =
    //     BORROWED_CLASS). Mirrors LocalVariableDeclaration § S9.5.4 and the
    //     class→interface param-passing upcast in CajetaClass.cpp.
    //   - interface value (rhsVal is already a ptr to a 24-byte body, e.g.
    //     `ArrayList.add(backend)` storing its interface param) → memcpy the body in.
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
                // The implementer hasn't synthesized this per-(class,
                // iface) vtable (an unresolved implements edge, or a pair
                // never dispatched). The codegen fixed-point's flagged
                // completion pass re-synthesizes classes whose interface
                // was a lazy-package placeholder BEFORE body codegen runs,
                // so by the time a real dispatch site compiles the lookup
                // above succeeds; a null here only reaches dead slots.
                vtableRef = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrTy));
            }
            builder->CreateStore(vtableRef, vtSlot);
            // `#=` / `#expr` RHS transfers ownership into the slot — the
            // MoveExpression already deactivated the source's drop entry,
            // so the fat body's kind must record OWNED or the instance
            // leaks (and an owned store dropped as BORROWED would never
            // free). Mirrors LocalVariableDeclaration's kind selection.
            bool rhsIsMove =
                std::dynamic_pointer_cast<MoveExpression>(rhsAst) != nullptr;
            builder->CreateStore(
                llvm::ConstantInt::get(i64Ty, (uint64_t) (rhsIsMove
                    ? IFACE_KIND_OWNED_CLASS : IFACE_KIND_BORROWED_CLASS)),
                kindSlot);
        } else if (llvm::isa<llvm::ConstantPointerNull>(rhsVal)) {
            // `arr[i] = null` (the assignment-based drop idiom): zero the
            // 24-byte fat-pointer body (null vtable / borrowed) — do NOT memcpy
            // from the null source, which faults. Mirrors the interface-FIELD
            // null store.
            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
            uint64_t bodyBytes = dl.getTypeAllocSize(bodyTy);
            builder->CreateMemSet(slot, builder->getInt8(0), bodyBytes,
                llvm::MaybeAlign(8));
        } else {
            // Interface → interface: rhsVal points at a 24-byte body; copy it in.
            const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
            uint64_t bodyBytes = dl.getTypeAllocSize(bodyTy);
            builder->CreateMemCpy(slot, llvm::MaybeAlign(8),
                rhsVal, llvm::MaybeAlign(8), bodyBytes);
            // A plain (non-move) copy is a BORROW: copying the source's
            // OWNED kind verbatim would make two owners of one instance —
            // both drops would free it. Downgrade the copy's kind unless
            // the RHS transferred (`#=`).
            if (!std::dynamic_pointer_cast<MoveExpression>(rhsAst)) {
                llvm::Value* kindSlot = builder->CreateStructGEP(
                    bodyTy, slot, 2, "iface_kind");
                llvm::Value* dataSlot0 = builder->CreateStructGEP(
                    bodyTy, slot, 0, "iface_data");
                llvm::Value* dataVal = builder->CreateLoad(ptrTy, dataSlot0);
                llvm::Value* isNull = builder->CreateIsNull(dataVal);
                llvm::Value* kindVal = builder->CreateSelect(isNull,
                    llvm::ConstantInt::get(i64Ty, 0),
                    llvm::ConstantInt::get(i64Ty,
                        (uint64_t) IFACE_KIND_BORROWED_CLASS));
                builder->CreateStore(kindVal, kindSlot);
            }
        }
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
    // Decide whether an integer binary op (+, -, *, +=, -=) should use a
    // SIGNED overflow check. The signedness must follow the operation's
    // result type, which tracks the TYPED operand: a bare integer literal
    // adapts to its sibling (`u - 1` is unsigned when `u` is unsigned). The
    // literal's own default-signed type must NOT force a signed check —
    // otherwise valid unsigned arithmetic (e.g. the SWAR `m = m & (m - 1)`
    // popcount idiom, where m can legitimately reach 0x8000…0) traps on a
    // spurious signed overflow. A literal only counts when BOTH operands are
    // literals (no typed operand to defer to). See String.indexOfFrom.
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

    // The AllocaInst → load mapping is gated on the AST being an
    // IdentifierExpression (or absent — legacy callers without an AST). Other
    // expression types that legitimately produce AllocaInst values that are
    // NOT slot pointers (LambdaExpression's closure record, NewExpression's
    // stack allocations, etc.) must pass through unchanged, because the
    // alloca address IS the value being yielded.
    llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v, ExpressionPtr ast) {
        if (!v) return v;
        // A constant null pointer is an r-value (the `null` literal, or a
        // typed null cast like `(TcpStream) null` passed as a borrowed
        // class/interface arg), never a slot to load through. The class-ref
        // and pointer-with-different-type branches below would otherwise
        // emit `load ptr, null` — a guaranteed segfault — treating the null
        // value as if it were the address of a class slot. Hand back the
        // null reference itself.
        if (llvm::isa<llvm::ConstantPointerNull>(v)) {
            return v;
        }
        // matrix-element-callarg — a NON-POINTER is already an r-value, so
        // there is nothing to load through. Every branch below emits a load,
        // and a load whose operand is not a pointer is not merely wrong but
        // unrepresentable: LLVM rejects it at verify.
        //
        // The shape that hit this is a Matrix element in ARGUMENT position,
        // `f(m[1][1])`. Element access lowers per container: a plain array
        // hands back the element GEP (a pointer, which the caller must load),
        // while Matrix/Vector extract the lane and materialize it, handing
        // back the float VALUE. The 2026-08-01 fix that made `f(arr[i])` work
        // applies loadIfLValue to every ArrayIndexExpression argument, so the
        // already-materialized Matrix element got loaded a second time:
        //
        //     %5 = load float, ptr %vec.idx.slot   ; the element's value
        //     %6 = load float, float %5            ; "Load operand must be a pointer"
        //
        // Guarding on the VALUE rather than on the container type fixes every
        // such source at once — the same defect was reported against Matrix
        // `m[r][c]`, plain arrays, and `ArrayList.operator[]`, with three
        // different failure signatures, because the position was always the
        // variable and the lowering never was.
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
            // A bare static-field identifier (`TAG` for `Main.TAG`) returns the
            // GlobalVariable slot, same shape as a local's alloca — load through
            // it too, or the read yields the slot ADDRESS instead of the value.
            if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
                return builder->CreateLoad(g->getValueType(), g);
            }
        }
        if (ast && dynamic_pointer_cast<ArrayIndexExpression>(ast)) {
            CajetaTypePtr elemType = ast->getResolvedType();
            if (elemType) {
                // S9.5.4 — interface elements are 24-byte fat-pointer bodies
                // stored INLINE in the array's data slot, so the element GEP
                // already points AT the body (unlike a plain class-ref element,
                // whose slot holds a `ptr` to a heap instance). Dispatch and
                // assignment alike want a pointer to the body, not a loaded
                // word — hand the GEP back as the interface value, exactly as
                // the DotExpression branch does for interface fields. Loading
                // here would yield the body's first word (the data ptr); a
                // later dispatch dereferences that as a body → garbage vtable.
                auto elemClass = dynamic_pointer_cast<CajetaClass>(elemType);
                if (elemClass && elemClass->isInterface()
                        && llvm::isa<llvm::GetElementPtrInst>(v)) {
                    return v;
                }
                llvm::Type* loadTy;
                // Value-type (record / @ValueType) elements are stored INLINE in
                // the array's data slot (their body sub-aggregate), so load the
                // body struct by value — NOT as a pointer. Value types carry
                // STRUCT_FLAG, so this must precede the class-ref/STRUCT_FLAG
                // rule below or `Point p = pts[i]` loads {x,y} as a pointer and
                // the caller dereferences it.
                bool elemIsValueType = elemClass && elemClass->isValueType();
                // Class-typed elements (CajetaArray, plain CajetaClass)
                // are stored as pointers in the array's data slot.
                // Loading the slot yields the heap reference, not the
                // instance contents. CajetaArray inherits from CajetaClass,
                // so the dynamic_cast catches both. Primitives load as
                // their value type.
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
                // A value-type (record / @ValueType) field is stored INLINE in
                // the parent struct (its body sub-aggregate), NOT as a `ptr` to
                // a heap instance — same as a view/interface field. So it is not
                // a class-ref: load it as its body type (a struct-value load,
                // matching how a value-type identifier is read), never as a
                // pointer. Without this exclusion the first 8 bytes of the inline
                // value get loaded AS a pointer and the caller dereferences them
                // (e.g. `HashMap<K, value-type V>.get` returning `slots[i].val`).
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
        // A ternary (`cond ? a : b`) has ALREADY produced its r-value: the
        // merge `phi` of the two arms (each loaded to an r-value inside
        // BooleanSwitchExpression::generateCode). For a class/String-typed
        // ternary the phi IS the instance pointer — the class-ref catch-all
        // below would otherwise load through it and hand back the vtable word,
        // so `"x" + (cond ? "a" : "b")` rendered the operand empty.
        if (dynamic_pointer_cast<BooleanSwitchExpression>(ast)) {
            return v;
        }
        // A `#x` MoveExpression has ALREADY loaded its operand to the
        // r-value (the owned heap pointer) — see MoveExpression::
        // generateCode, which loads through the source alloca before
        // returning. Without this carve-out the class-ref catch-all
        // below loads through that pointer AGAIN and hands back the
        // vtable word (the struct's first 8 bytes) instead of the
        // instance reference, so `this.field = #param` stores the
        // vtable address and every later dispatch / field read through
        // the field reads garbage. Same shape as the NewExpression /
        // MethodCallExpression carve-outs.
        if (dynamic_pointer_cast<MoveExpression>(ast)) {
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
        // A cast's result is ALWAYS an r-value: primitive casts return the
        // converted value, class casts return the (possibly guard-checked)
        // instance pointer, and a record upcast returns the freshly sliced
        // aggregate's alloca — the pointer IS the value (aggregate-init
        // shape). The class-ref catch-all below would load through it and
        // hand back the first field's bytes as a "pointer".
        if (dynamic_pointer_cast<CastExpression>(ast)) {
            return v;
        }
        // `base[a:b]` yields a freshly built Slice<T> VALUE — the returned
        // alloca IS the aggregate (same shape as the record upcast above).
        // The class-ref catch-all would load its first word (the store
        // pointer — the backing array header) and treat THAT as the value:
        // the exact double-deref that handed count() the header bytes.
        if (dynamic_pointer_cast<ArraySliceExpression>(ast)) {
            return v;
        }
        // REFL-1.5 — `T.class` returns the address of the type's #ClassObject
        // global, which IS the Class<T> reference (a process-lifetime constant
        // { Class<?>#VTable, rtti }). Same carve-out shape as NewExpression /
        // MethodCallExpression: the pointer IS the language-level value. The
        // class-ref catch-all below would otherwise load through the global and
        // hand back the vtable word (its first field) as if it were the Class
        // instance pointer, corrupting every downstream dispatch on it.
        if (dynamic_pointer_cast<ClassLiteralExpression>(ast)) {
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
    // Arithmetic on two POINTER operands: the wildcard monomorph of a class
    // template (`Column<?>::mean`) lowers T-typed values as opaque pointers,
    // and its body is DEAD code — a wildcard receiver never executes it
    // (objects are always concrete monomorphs; calls are direct) — but the
    // body still lands in the module and must pass LLVM verify. `add ptr`
    // has historically escaped through the string-concat fallback; `-`,
    // `*`, `/` reached CreateSub/Mul/UDiv with pointer operands, which
    // verify rejects. Round-trip through i64 — legal IR, never executed.
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
            // Division: guard the divisor so even a stray execution cannot
            // trap the process on udiv-by-zero.
            llvm::Value* zero = llvm::ConstantInt::get(i64Ty, 0);
            llvm::Value* isZero = builder->CreateICmpEQ(ri, zero);
            llvm::Value* safe = builder->CreateSelect(isZero,
                llvm::ConstantInt::get(i64Ty, 1), ri);
            v = builder->CreateSDiv(li, safe);
        }
        return builder->CreateIntToPtr(v, l->getType(), "deadptr.res");
    }

    // `lhsSigned`/`rhsSigned` decide the FILL BIT when one operand is
    // narrower and has to widen. They default to signed, which preserves the
    // historical behaviour for the callers that have no type flags to hand;
    // the binary-op path passes the AST-derived truth, so a uint32 widens
    // zero-extended instead of turning into a 64-bit run of ones.
    static std::pair<llvm::Value*, llvm::Value*> coerceArithPair(
            CajetaModulePtr module, llvm::Value* l, llvm::Value* r,
            bool lhsSigned = true, bool rhsSigned = true) {
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
            if (lb < rb) l = builder->CreateIntCast(l, rt, lhsSigned);
            else if (rb < lb) r = builder->CreateIntCast(r, lt, rhsSigned);
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
    // Locate the hidden ownership word for a `recv.field` store, so the store
    // can record whether the field holds a title.
    //
    // The word lives at a FIXED byte delta from the field slot — both sit in
    // the declaring class's sub-object region, so the delta measured on the
    // standalone layout holds inside any descendant. Returns false (leaving
    // the outputs untouched) whenever the field carries no bit: a bit-less
    // field type, an unresolvable receiver, or an opaque/incomplete layout.
    //
    // Shared by the String field-store path and the general field-ownership
    // ("fob") block below. They diverge afterwards — the general path also
    // classifies the store's spelling and releases a displaced owned value,
    // while a String field always owns its wrapper — but both need this same
    // word, and a second copy of the layout math is a place for the two to
    // disagree.
    static bool locateFieldOwnershipBit(
            const CajetaModulePtr& module, llvm::IRBuilder<>* builder,
            const std::shared_ptr<DotExpression>& dot, llvm::Value* slotPtr,
            llvm::Value** wordPtrOut, int* bitIdxOut,
            StructurePropertyPtr* propOut = nullptr) {
        if (!dot || !slotPtr || !slotPtr->getType()->isPointerTy()) return false;
        auto& ch = dot->getChildren();
        auto recv = ch.empty() ? nullptr
            : std::dynamic_pointer_cast<Expression>(ch[0]);
        // title-stores §3.3.2 (Unit 4) — ARRAY-ELEMENT receivers participate
        // only when the element struct carries per-member bits; elements
        // without them keep the 6.2.6b exemption.
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

    // B1 S5: `*` on a matrix LHS. `*` is MATRIX MULTIPLY (not element-wise):
    //   Matrix<T,R,K> * Matrix<T,K,C> -> Matrix<T,R,C>  (inner dim K checked)
    //   Matrix<T,R,C> * Vector<T,C>   -> Vector<T,R>
    //   Matrix<T,R,C> * scalar        -> Matrix<T,R,C>  (element-wise scale)
    // The element-wise (Hadamard) product is the `hadamard()` method, not `*`.
    // Returns nullptr for any non-`*` op or unhandled RHS so the caller falls
    // through. This is the operator the current overloading mechanism cannot
    // express as a single non-templated declaration (it is K/shape-generic) —
    // the motivating case for the method-templated-operator follow-on; here it
    // is a codegen interception, like Vector's intrinsics.
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

        // Matrix * Matrix -> matmul (inner dim K = lhs.cols must equal rhs.rows).
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
        // Matrix * Vector -> matVec (vector length must equal lhs.cols).
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
        // Matrix * scalar -> element-wise scale.
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
        // Pre-pass: operand types not yet known — do not stamp; the caller's
        // later `if (!resolvedType) resolveTypes(...)` recomputes with them.
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
                            // loadIfLValue, not a bare AllocaInst unwrap: a
                            // FIELD receiver (`this.sessions[id] = v`)
                            // generates a GEP address, which the alloca cast
                            // missed — the field's ADDRESS went out as `this`
                            // and the callee's vtable load found garbage
                            // (null fn-pointer call at dispatch). Mirrors the
                            // read-side ArrayIndexExpression lowering.
                            recvVal = loadIfLValue(module, recvVal, recvAst);
                            idxVal = loadIfLValue(module, idxVal, idxAst);
                            valVal = loadIfLValue(module, valVal, valAst);
                            std::vector<ParameterEntry> entries;
                            entries.push_back(ParameterEntry(idxType, "", idxVal));
                            entries.push_back(ParameterEntry(valType, "", valVal));
                            std::string opName = "operator[]=";
                            // uniform-transfer 2.3 — the `#T` contract check.
                            // This store lowers STRAIGHT to invokeMethod, so
                            // MethodCallExpression's check never sees it, and
                            // `m[k] = v` was the one container write that could
                            // still lend into an owning entry (found 2026-08-03
                            // by ContainerTitleTests.indexedLendIsRejected —
                            // `HashMap.operator[]=` takes `#K, #V` and the
                            // subscript form compiled clean anyway).
                            //
                            // Same rule as the general call path, in the two
                            // positions this form has: a `#`-declared formal
                            // whose argument is a bare identifier naming an
                            // owner must be surrendered explicitly. Fresh
                            // constructions and `#x` (a MoveExpression) pass
                            // through, as do borrows and primitives.
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
                                    auto oa = opArgs[a];
                                    if (dynamic_pointer_cast<MoveExpression>(oa))
                                        continue;
                                    if (dynamic_pointer_cast<NewExpression>(oa))
                                        continue;
                                    auto oid = dynamic_pointer_cast<
                                        IdentifierExpression>(oa);
                                    if (!oid) continue;
                                    auto oscope = module->getScopeStack().peek();
                                    if (!oscope) continue;
                                    FieldPtr ofield =
                                        oscope->getField(oid->getTextValue());
                                    if (!ofield) continue;
                                    bool owns = ofield->getDropEntry() != nullptr;
                                    if (!owns) {
                                        if (auto cm = module->getCurrentMethod()) {
                                            owns = cm->isArenaEligibleLocal(
                                                oid->getTextValue());
                                        }
                                    }
                                    if (!owns) continue;
                                    throw Exception(
                                        "method `" + opName + "` declares "
                                        "parameter `" + ofp->getName()
                                        + "` as `#T` (ownership transfer "
                                          "required); write `#"
                                        + oid->getTextValue() + "` at the call "
                                          "site to surrender ownership of the "
                                          "source local, or pass a fresh "
                                          "`heap T(...)` / `stack T(...)` "
                                          "construction. See "
                                          "docs/specification/lang/"
                                          "OwnershipTransfer.md.",
                                        "CAJETA_ERROR_TRANSFER_REQUIRED",
                                        module->getSourcePath(),
                                        (int) getSourceLine(), -1);
                                }
                            }
                            // title-tracking 6.2.1 — the transfer word for the
                            // lowered call. `m[k] = #v` previously passed NO
                            // word: the MoveExpression deactivated the source
                            // while the callee read all-borrow 0 and recorded
                            // the entry BORROWED — a silent leak. Bit 0 = the
                            // index arg, bit 1 = the value arg (the operator's
                            // two user params); a runtime-owner source (`#s`
                            // where s is a formal) forwards its captured flag,
                            // per the MCE word-composition rule.
                            llvm::Value* opxWord = builder->getInt64(0);
                            {
                                int64_t constBits = 0;
                                llvm::Value* dynWord = nullptr;
                                auto wordBit = [&](ExpressionPtr ast, int bit) {
                                    auto mv = dynamic_pointer_cast<
                                        MoveExpression>(ast);
                                    if (!mv) return;
                                    if (llvm::Value* rf =
                                            mv->getRuntimeTitleFlag()) {
                                        llvm::Value* b = builder->CreateShl(
                                            builder->CreateAnd(rf,
                                                builder->getInt64(1)),
                                            builder->getInt64((uint64_t) bit));
                                        dynWord = dynWord
                                            ? builder->CreateOr(dynWord, b) : b;
                                    } else {
                                        constBits |= ((int64_t) 1) << bit;
                                    }
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

        // title-stores 6.2.1 — the loud-plain-store diagnostic (spec §2.4).
        // A plain `=` retaining store (field or slot destination) whose RHS
        // names a runtime-conditional owner (armed-capable formal, flagged
        // local) borrows a value whose armed entry frees it at scope exit.
        // WARNING at introduction; Phase 3 promotes to error. Sigil
        // spellings are quiet by construction: their RHS is a
        // MoveExpression, not a bare identifier.
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

        // P3 — definite-assignment: for a bare-identifier LHS of an
        // assignment, mark the name assigned BEFORE evaluating the LHS.
        // The identifier's generateCode would otherwise trip the NYA
        // check on its way to producing the slot address — but the LHS
        // of an assignment is a write target, not a read. Marking
        // pre-eval lets the LHS slot fetch succeed; the assignment that
        // follows then writes the value into the slot.
        bool lhsWasMoved = false;
        if (binaryOp == BINARY_OP_ASSIGN && !children.empty()) {
            if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (auto sc = module->getScopeStack().peek()) {
                    sc->markAssigned(lhsId->getTextValue());
                    // title-tracking §3.1.4 — reassignment re-arms a moved-out
                    // binding. Clear before the LHS slot fetch (a write target,
                    // same rationale as markAssigned above); the fresh-owner
                    // entry retarget below keys on lhsWasMoved.
                    lhsWasMoved = sc->isBorrow(lhsId->getTextValue());
                    if (lhsWasMoved) sc->restoreOwnership(lhsId->getTextValue());
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
        // Identifier.cpp / DotExpression.cpp via borrowedBindings / borrowedPaths.

        // title-stores §2.1 — fuse `dst[i] #= src[j]` into a FORWARDING slot
        // move before the RHS extraction codegen runs: the source bit
        // transfers verbatim and the borrowed-take panic is suppressed. This
        // is the container author's shift/sift primitive.
        //
        // uniform-transfer-semantics Unit 3: the mark never required the
        // DOUBLE sharp, so the primitive is unaffected by that unit rejecting
        // `#= #`. The walk below is kept because the `#=` desugar wraps its
        // RHS in a Move of its own and an explicit legacy `dst[i] = #src[j]`
        // must reach the same innermost node.
        if (binaryOp == BINARY_OP_ASSIGN && children.size() >= 2) {
            auto fwdLhs = dynamic_pointer_cast<ArrayIndexExpression>(children[0]);
            auto fwdMv = dynamic_pointer_cast<MoveExpression>(children[1]);
            while (fwdMv && !fwdMv->getChildren().empty()) {
                auto deeper = dynamic_pointer_cast<MoveExpression>(
                    fwdMv->getChildren()[0]);
                if (!deeper) break;
                fwdMv = deeper;
            }
            if (fwdLhs && fwdMv && !fwdMv->getChildren().empty()) {
                if (auto fwdSrc = dynamic_pointer_cast<ArrayIndexExpression>(
                        fwdMv->getChildren()[0])) {
                    auto fwdL = dynamic_pointer_cast<Expression>(children[0]);
                    auto fwdS = dynamic_pointer_cast<Expression>(
                        fwdMv->getChildren()[0]);
                    if (fwdL && !fwdL->getResolvedType()) fwdL->resolveTypes(module);
                    if (fwdS && !fwdS->getResolvedType()) fwdS->resolveTypes(module);
                    if (fwdL && fwdS
                            && CajetaClass::arrayElementCarriesSlotBits(
                                   fwdL->getResolvedType())
                            && CajetaClass::arrayElementCarriesSlotBits(
                                   fwdS->getResolvedType())) {
                        fwdMv->setForwardingSlotMove(true);
                    }
                }
            }
            // title-stores §3.3.2 — member-to-member forwarding
            // (`dst[i].m #= #src[j].m`, the HashMap rehash shape) is GONE as
            // of uniform-transfer-semantics Unit 3. It was reachable only
            // through a double Move, which only `#= #` produced, and that
            // spelling is now rejected outright. The capability retires with
            // its one caller: the rehash forwarded a member's bit verbatim
            // because a slot might hold a borrow — which is STILL true
            // (collections do not own by default). What retired the capability
            // is the rejection of the `#= #` spelling in Unit 3, not the
            // disappearance of borrowed slots; the SINGLE-sharp fwdLhs/fwdSrc
            // arm above is now the only forwarding primitive.
            // `grep '#= #' runtime/src` is 0.
        }
        // array-literals §3.2 — for `lhs = [...]`, target-type the RHS literal
        // from the LHS array element type BEFORE the operands generate below
        // (both are generated up-front here), so `int64[] xs; xs = [1,2,3];`
        // widens per the LHS rather than the narrower unify result.
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
                        // collection-literals §2 — `xs = [...]` on a collection
                        // local rewrites to a from-array ctor call.
                        children[1] = ctor;
                    }
                }
            } else if (auto agg = dynamic_pointer_cast<
                           AggregateInitializerExpression>(children[1])) {
                // collection-literals §4 — `p = {…}` infers the LHS type (only
                // a class LHS; a primitive leaves it uninferred → NO_TYPE).
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
                // collection-literals §3 — `m = [k: v]` infers the LHS map type.
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

        // A binary operator requires both operands to produce a value. When an
        // operand's codegen returns null, some sub-expression failed to yield
        // one — e.g. a member access that doesn't resolve on the receiver's
        // type, such as `.length` on an array (the size accessor is `count()`).
        // Without this guard the null flows into getTypeFlagsOf / loadIfLValue
        // below and SIGSEGVs the compiler; surface a clean diagnostic instead.
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
        // A `void` call is PRESENT but valueless, so the null guard above misses
        // it — an assignment RHS then hung the compiler rather than diagnosing (1.2.3).
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

        // B1: Matrix binary ops, intercepted before the built-in/vector path.
        // `+ - /` are element-wise over same-shape matrices -> Matrix<T,R,C>;
        // `== !=` are element-wise equality reduced to a boolean. `*` (matmul /
        // matVec / scalar scale) is intercepted in generateMatrixMul (S5). The
        // matrix value lives in a `<R*C x T>` register, so the ops are flat
        // vector arithmetic; only the result SHAPE/type bookkeeping is special.
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
                // Comparisons (`== != < <= > >=`) -> a per-lane `<R*C x i1>`
                // mask typed Matrix<boolean,R,C> (the value-type comparison
                // rule). RHS is a same-shape matrix or a broadcast scalar.
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

        // Vector comparisons -> a per-lane `<N x i1>` mask, typed
        // Vector<boolean,N> (the value-type comparison rule). `== != < <= > >=`
        // all produce masks; reduce with `.all()` / `.any()`, blend with
        // `.select(a, b)`. The generic scalar comparison path below mis-checks
        // FP-ness on a vector (isFloatingPointTy() is false for `<N x T>`), so
        // vectors are handled here first.
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
                    // A scalar RHS broadcasts to the lane count.
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

        // Quaternion operators: `*` is the Hamilton product (Quaternion *
        // Quaternion) or vector rotation (Quaternion * Vector<T,3>); `+ -` are
        // element-wise (lerp setup). All lower as quatops intrinsics.
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

        // Operator overloading: if LHS resolves to a class type with an
        // `operator<sym>` method (e.g. `operator+`, `operator==`), dispatch
        // through it before the built-in arithmetic path. RHS is passed as
        // the single non-this argument; the method's return value is the
        // expression's value. The lookup falls back through hierarchy via
        // resolveMethod (same machinery dispatch uses), so an operator
        // defined on a base class is visible to its subclasses.
        // S6: the op→symbol map lives in OperatorDispatch.h (shared with device
        // lowering). nullptr ⇒ no overloadable static form → skip to built-in.
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
                // resolveMethod's canonical computation calls
                // `parameter.type->toCanonical()` which crashes on null —
                // bail out if we still don't have a usable type rather than
                // attempt the lookup.
                if (!rhsType || !lhsType) {
                    goto fallthrough_to_builtin;
                }
                {
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
                    // S6: direct lookup + comparison derivation (!=/>/>=/<=) is
                    // the shared opdispatch policy. The host supplies the two
                    // representation-specific callbacks: resolve+invoke a static
                    // operator (Cajeta binary overloads are static per
                    // OperatorOverloading.md §2 — both operands explicit, no
                    // `this`, so invokeMethod gets a null receiver and its
                    // `isStatic` branch skips the implicit prepend), and boolean
                    // negation. Device lowering (S8) reuses dispatchBinaryOperator
                    // with its own callbacks.
                    // `name` by value (mutable) — resolveMethod/invokeMethod take
                    // a non-const string&.
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
                        // The base comparison returns boolean (i1); CreateNot is
                        // the boolean negation.
                        return builder->CreateNot(v,
                            std::string("derived.") + opSym);
                    };
                    std::pair<bool, llvm::Value*> disp =
                        opdispatch::dispatchBinaryOperator(
                            binaryOp, tryInvoke, negate);
                    if (disp.first) {
                        return disp.second;
                    }
                    // nucleo-frame U1 — an ORDERING comparison between a
                    // CLASS and a NUMERIC primitive with no matching operator
                    // override has no meaning: the builtin path would
                    // silently compare a POINTER against the number. Fail
                    // loud, naming the type and operator (`ColStr > 0.0` is
                    // the canonical mis-typed-DSL case). Class-vs-class
                    // ordering (String < String rides a downstream builtin)
                    // and EQ/NE (pointer identity) fall through unchanged.
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

        // SIGNEDNESS CANNOT BE RECOVERED FROM AN llvm::Value. `getTypeFlagsOf`
        // looks up `llvmTypeIdMap`, which is keyed by llvm::Type::TypeID — and
        // every integer width shares `IntegerTyID`, so the map holds ONE entry
        // for all of int8..uint64 and whichever integer type registered last
        // decides what every integer value appears to be. Reading SIGNED_FLAG
        // from it therefore answers a question about the type system with a
        // fact about LLVM's type IDs, and the ops that branch on it (`/`, `%`,
        // `>>`) silently pick the wrong instruction — signed `int64 / int64`
        // was emitting `udiv`, so -8 / 2 evaluated to 9223372036854775804.
        //
        // The AST's resolved type does know int64 from uint64, so ask it first
        // and keep the value-derived flags only as a fallback for operands
        // with no resolvable AST (synthesized temporaries).
        // READ-ONLY: consult the resolved type only if it is ALREADY there.
        // Forcing `resolveTypes` here would re-enter type resolution from the
        // middle of codegen for every binary op — a far broader change than
        // the question being asked, and it destabilised generic member stores
        // (a HashMap<String,Int64> put began faulting).
        auto typeFlagsFor = [&](const ExpressionPtr& ast,
                                llvm::Value* v) -> long {
            if (ast) {
                if (CajetaTypePtr rt = ast->getResolvedType()) {
                    if ((rt->getTypeFlags() & PRIMITIVE_FLAG) != 0) {
                        return (long) rt->getTypeFlags();
                    }
                }
                // A bare identifier usually has NO resolved type here, and
                // that is the common shape — `int64 a = -8; a / b`. Falling
                // back to the value-derived flags for it left signed division
                // emitting `udiv` (the literal-operand form happened to work,
                // which is why a constants-only probe passed and the
                // locals-based pin failed). Read the DECLARED type out of the
                // scope instead: a plain lookup, no re-entry into type
                // resolution — forcing `resolveTypes` here is what previously
                // destabilised generic member stores.
                // A CAST names its target type outright, and `getDestType`
                // is available without running resolution. This matters for
                // `/` and `%`, which OR the two operands' flags: one operand
                // falling back to the lossy value-derived path drags the whole
                // operation to signed, so `someUint64 / (uint64) 2` emitted
                // `sdiv` and 2^64-2 / 2 evaluated to 2^64-1 (i.e. -2 / 2 = -1)
                // even though the same division with a uint64 LOCAL divisor
                // was correct.
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

        // KNOWN signedness, tri-state: 1 signed, 0 unsigned, -1 undetermined.
        // The distinction matters because the value-derived fallback cannot
        // tell int64 from uint64 and reports SIGNED_FLAG clear — i.e. it
        // LOOKS unsigned. Under a plain "unsigned wins" rule an undetermined
        // operand would therefore drag `int64Local / 2` to `udiv`, which is
        // the very bug this work started from, with the sign reversed. So an
        // operand only votes when its type was actually determined.
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
            // A bare integer literal is a SIGNED int by default. Saying so is
            // what makes `someUint64 / 2` come out unsigned under the rule
            // below rather than falling into the undetermined branch.
            if (dynamic_pointer_cast<IntegerLiteralExpression>(ast)) return 1;
            return -1;
        };
        const int lhsSignKnown = knownSignedness(lhsAst);
        const int rhsSignKnown = knownSignedness(rhsAst);

        // USUAL ARITHMETIC CONVERSIONS at equal rank: UNSIGNED WINS. `uint64`
        // is an unsigned type, so `someUint64 / 2` is an unsigned division —
        // the previous rule ("either operand signed => signed") had it exactly
        // backwards and made that expression `sdiv`. Operands are already
        // width-coerced by coerceArithPair, so rank is settled and sign is all
        // that is left to decide. With neither operand determined, keep the
        // historical flag-OR so nothing silently changes shape.
        const bool signednessDetermined =
            (lhsSignKnown >= 0) || (rhsSignKnown >= 0);
        const bool intOpIsSigned = signednessDetermined
            ? !(lhsSignKnown == 0 || rhsSignKnown == 0)
            : (((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0);

        llvm::Value* result = nullptr;

        // Replace bare loadIfAlloca with loadIfLValue passing the relevant ast where useful.
        auto loadL = [&](llvm::Value* v) { return loadIfLValue(module, v, lhsAst); };
        auto loadR = [&](llvm::Value* v) { return loadIfLValue(module, v, rhsAst); };
        (void) loadL; (void) loadR; // used selectively below

        // For non-assignment ops both sides are r-values; for assignment forms lhs is the
        // target address and only rhs needs r-value coercion.
        switch (binaryOp) {
            case BINARY_OP_ASSIGN: {
                // Record immutability (records-spec §3.2 / plan 3.2.1): a
                // record field only initializes inside the record's own
                // constructor (`this.f = v`); every other field write is a
                // compile error. Statics stay assignable (immutability is a
                // per-instance value property). Assigning TO a class field
                // that merely HOLDS a record stays legal — the receiver
                // there is the class, not the record.
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
                            // Per-field mutation opt-in (records-spec §3.4):
                            // a `mut` field accepts in-place writes.
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
                // Matrix element assignment: `m[r][c] = e` (B1). The LHS is
                // ArrayIndex(ArrayIndex(m, r), c) with m a CajetaMatrix. Writing
                // through the row temporary (the vector path below) would drop the
                // store — the row m[r] is a fresh value, not an l-value into m —
                // so write directly into m's slot at flat lane r*C+c. Must run
                // BEFORE the vector path, which would otherwise match the
                // Vector<T,C>-typed outer base m[r].
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
                // Value-type (record / @ValueType) FIELD assignment
                // (`this.p = e`). The field stores the aggregate INLINE
                // (CajetaClass::buildInstanceStructBody), while the RHS is
                // usually an ADDRESS (aggregate-init alloca, value local
                // slot, field-read GEP) — a plain store would write the
                // pointer bits over the first field. Copy the whole body;
                // a first-class struct value (by-value return) stores
                // directly.
                // An ARRAY ELEMENT takes the same copy path: a value-type element
                // keeps its own layout inline (CajetaArray::getElementLlvmType), so
                // `a[i] = stack Rec(..)` must copy the body — a plain pointer store
                // wrote the address bits over the first field and the element read
                // back uninitialized.
                if (dynamic_pointer_cast<DotExpression>(lhsAst)
                        || dynamic_pointer_cast<IdentifierExpression>(lhsAst)
                        || dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (!lhsAst->getResolvedType()) lhsAst->resolveTypes(module);
                    auto lhsCls = dynamic_pointer_cast<CajetaClass>(lhsAst->getResolvedType());
                    // Identifier LHS: only an inline value slot (alloca of the
                    // body type) takes the copy path — a ptr-typed slot is a
                    // reference and keeps the pointer store below.
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
                            // slice-spec §6.1 copy/drop hooks: overwriting a
                            // shared-capable value releases the OLD value's
                            // stakes first; a copy from an LVALUE (identifier /
                            // field / element / slice-cast) then retains the
                            // new ones. Rvalue RHS (call result, aggregate-
                            // init) carries its stakes with the bytes — no
                            // retain. Syntactic self-assign (`v = v`) skips
                            // the pair (release could free at rc==1 and the
                            // retain would resurrect a dead buffer).
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
                            // Slice<T> FIELD stores resolve in place per the
                            // §4.2 table (slice-spec §7.2): arena / <=256 B
                            // payload copies into a fresh field-owned root,
                            // larger windows take a stake — for BOTH lvalue
                            // and rvalue RHS (a fresh `arr[a:b]` is a borrow
                            // of the array too). Local Slice assigns keep
                            // borrow semantics (the generic retain arm's
                            // sign-bit gate makes copies of resolved values
                            // retain and borrow copies free).
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
                            // storeInterfaceInlineBody handles all three RHS
                            // shapes: a concrete class instance ASSEMBLES the
                            // fat body in place (data/vtable/kind — a memcpy
                            // from an 8-byte class object read 16 bytes of
                            // heap garbage as vtable+kind and dispatch
                            // jumped into it), an interface value copies its
                            // body, and null zeroes the field.
                            storeInterfaceInlineBody(module, lhs, loadR(rhs),
                                lhsCls, rhsAst);
                            result = lhs;
                            break;
                        }
                    }
                }
                // Escape resolution at the String field-store site (slice-spec
                // §4.2; slices plan Unit 4). A plain `obj.f = w` where w is a
                // scope-owned String LVALUE stored the BORROWED WRAPPER pointer
                // — the declaring scope frees that wrapper at exit and the
                // field dangles (UAF). Resolve instead: store a FRESH wrapper
                // the field owns (copy ≤ threshold / stake on a large heap
                // root / free alias of a static root — the runtime decides).
                // Rvalue RHS (call result, concat, `#`-move) transfers its
                // wrapper as before — no resolve.
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
                        // A string-literal RHS is a String even when its
                        // resolvedType is a null/placeholder (the unqualified
                        // CajetaType::of("String") path) — detect structurally
                        // so literal stores (`obj.f = "x"`) still old-drop.
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
                        // element-ownership §6.1.1 / plan 3.2.1 — gate the
                        // resolution on statically-known SOURCE ownership. A
                        // bare identifier bound to a `#`-transfer parameter is
                        // an OWNED source: storing it MOVES the wrapper into the
                        // field, exactly like an explicit `#`-move rvalue. It
                        // must NOT resolve — a fresh copy would strand the
                        // moved-in source (leak 15.12.1: `this.f = param` on a
                        // `#String param`). The `#` on the formal is the static
                        // proof of ownership; no runtime moveMask read is
                        // needed. Demote such an identifier out of the lvalue
                        // (borrow-resolve) class so it transfers as-is below.
                        // element-ownership 3C — a PLAIN formal source may
                        // still be a runtime transfer (`m.put(#k, v)` on a
                        // `K key` formal: the dual-role moveMask contract).
                        // Its mask bit (index among non-`this` formals)
                        // decides move-vs-resolve at RUNTIME — the ungated
                        // copy was the 15.12.1 strand (slot got a copy, the
                        // transferred original's drop was already
                        // deactivated at the call). Unit 4/8's static `#K`
                        // formals retire the runtime branch.
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
                            llvm::Function* resolveFn = rhsIsLvalue
                                ? module->getRuntimeFunction("__cajeta_string_resolve")
                                : nullptr;
                            // 7.2.2 — the mask is the enclosing
                            // function's ABI transfer word (TLS retired).
                            llvm::Value* maskWord = nullptr;
                            if (rhsIsLvalue && resolveFn && maskBit >= 0) {
                                if (auto cmw = module->getCurrentMethod()) {
                                    maskWord = cmw->getTransferWordArg();
                                }
                            }
                            llvm::Value* srcWrapper = loadR(rhs);
                            // Lvalue RHS: the source keeps its wrapper — the
                            // field takes a FRESH resolved one (copy ≤
                            // threshold / stake on a large heap root / static
                            // alias). Rvalue RHS (literal, call result,
                            // concat, `#`-move): the wrapper transfers as-is.
                            // Plain-formal lvalue: runtime moveMask branch —
                            // move when the caller transferred, resolve
                            // otherwise.
                            llvm::Value* fresh = nullptr;
                            if (maskWord) {
                                auto& lctx = *module->getLlvmContext();
                                llvm::Value* bit = builder->CreateAnd(
                                    builder->CreateLShr(maskWord, maskBit),
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(lctx), 1));
                                llvm::Value* isMove = builder->CreateICmpNE(
                                    bit, llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(lctx), 0),
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
                                fresh = (rhsIsLvalue && resolveFn)
                                    ? builder->CreateCall(resolveFn, {srcWrapper},
                                                          "str_resolve")
                                    : srcWrapper;
                            }
                            // 4.2.3 visibility lint: each silent resolution
                            // is reported as a NOTE (never an error) so the
                            // copy/share cost is visible; `#`-transfer makes
                            // the store free. No-op without an active engine.
                            // Stdlib-internal stores (the cajeta.* namespace,
                            // which user code cannot extend) never lint into
                            // user diagnostics.
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
                            // The field OWNS its wrapper (both arms), so
                            // dropping the previous value on overwrite is safe
                            // and closes the String-field overwrite leak.
                            // SKIPPED inside constructors: stack-class bodies
                            // aren't zero-initialized — a ctor's first store
                            // would read garbage as the "old" wrapper (heap
                            // bodies are zeroed; the skip costs at most a leak
                            // on a ctor double-store).
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
                            // The field owns `fresh` on EVERY arm above — a
                            // resolved copy/stake, a transferred rvalue, or
                            // either side of the moveMask phi — so record the
                            // title in the hidden ownership word. Without
                            // this the word stays 0 (borrow) and the holder's
                            // drop skips a String it must free: the mirror of
                            // the double-free this bit was added to stop.
                            //
                            // This path `break`s out of the assign switch
                            // before the general fob block below, so it must
                            // write the bit itself rather than fall through.
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
                // title-tracking §8.1 (plan 7.2.1) — the borrow→owning
                // materialize at owning-monomorph element stores was retired
                // with owning instantiations.
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
                // title-tracking §5 (Unit 3) — field ownership bits. A store
                // into a bit-carrying field (class-ref / heap-array slot)
                // resolves its declaring class, locates the hidden ownership
                // word at a FIXED byte delta from the field slot (both live
                // in the declaring class's sub-object region, so the delta
                // from the standalone layout holds inside any descendant),
                // classifies the store's spelling, and — BEFORE the store —
                // releases a displaced owned value (§5.1.3: the overwrite is
                // its scope exit). The bit itself is written after the store.
                llvm::Value* fobWordPtr = nullptr;
                int fobBitIdx = -1;
                bool fobOwnedSpelling = false;
                bool fobFieldIsArray = false;
                // 5.2.2 — non-null when the `#x` source is a runtime owner
                // (formal): the bit written below is this flag, not const 1.
                llvm::Value* fobRuntimeFlag = nullptr;
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
                            // Spelling → title in: `= #x`, a fresh heap
                            // construction, a `#`-returning call, or a
                            // `#`-formal source (the Exception-ctor idiom —
                            // statically owned, 7.1.3's move gate).
                            if (auto mv = dynamic_pointer_cast<
                                    MoveExpression>(rhsAst)) {
                                fobOwnedSpelling = true;
                                // RHS codegen already ran; a formal source
                                // stashed its pre-deactivation flag there.
                                fobRuntimeFlag = mv->getRuntimeTitleFlag();
                            } else if (auto rn = dynamic_pointer_cast<
                                           NewExpression>(rhsAst)) {
                                fobOwnedSpelling = !rn->getStackAlloc();
                            } else if (auto rc = dynamic_pointer_cast<
                                           MethodCallExpression>(rhsAst)) {
                                fobOwnedSpelling =
                                    rc->isResolvedReturnsOwnership();
                            } else if (auto ri = dynamic_pointer_cast<
                                           IdentifierExpression>(rhsAst)) {
                                if (auto sc = module->getScopeStack().peek()) {
                                    auto pf = std::dynamic_pointer_cast<
                                        ParameterField>(
                                            sc->getField(ri->getTextValue()));
                                    if (pf && pf->getFormalParameter()
                                            && pf->getFormalParameter()
                                                   ->isTransferred()) {
                                        fobOwnedSpelling = true;
                                    }
                                }
                            }
                            // Displaced-value release (pre-store).
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
                            llvm::Function* relFn = module->getRuntimeFunction(
                                fobFieldIsArray ? "__cajeta_free_array"
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
                                // title-stores Unit 5 — element walk on a
                                // displaced owned array, RE-ENABLED: the
                                // stdlib is pure-tail now (the 3.2.5 audit's
                                // mixed-owned[] grow loops respelled to
                                // forwarding moves, whose takes clear the
                                // source bits — the walk then finds only
                                // slots the old array still genuinely owns).
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
                // Interface-typed array element: the slot is an INLINE 24-byte
                // fat-pointer body, so a plain store would write only the data
                // word and leave the vtable garbage. Build/copy the body in place
                // instead. (Class-ref elements store an 8-byte pointer and fall
                // through to the normal store below.)
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
                // slices 9.2.1 — String-element store into an OWNING local
                // array routes through the sidecar helpers so the slot's
                // per-element ownership tracks the store shape: sources the
                // array TAKES (identifier lvalue — the deactivation block
                // below strips its entry; `#`-move; fresh owned temp) mark
                // the slot; alias sources (field/element reads, literals,
                // borrow-returning calls) leave it unmarked so the walk
                // never steals a value owned elsewhere. Both helpers
                // release a previously-owned occupant (the overwrite leak).
                bool storedViaElemOwn = false;
                bool storedViaClassElem = false;
                if (!storedInterfaceInline && lhsAst && rhsAst
                        && lhs && lhs->getType()->isPointerTy()
                        && rhsVal && rhsVal->getType()->isPointerTy()) {
                    if (auto aixLhs = dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
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
                        // title-stores §3.2 — bit-capable class elements
                        // route through the TAIL BITMAP (header-addressed:
                        // one mechanism for field arrays and locals; the
                        // local sidecar retires for class elements). The
                        // slot bit records the store's actual title: a
                        // MoveExpression's captured runtime flag forwards
                        // verbatim (`#=` of a formal stores what the caller
                        // did); static spellings classify as before.
                        bool elemTitled =
                            CajetaClass::arrayElementCarriesSlotBits(
                                lhsAst->getResolvedType());
                        // The tail helper needs the array HEADER. Regenerate
                        // the receiver only for side-effect-free shapes
                        // (identifier / field path) — the container-author
                        // shapes; anything else conservatively keeps the
                        // plain store.
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
                            llvm::Value* ownedVal = nullptr;
                            if (auto mv = dynamic_pointer_cast<MoveExpression>(
                                    rhsAst)) {
                                ownedVal = mv->getRuntimeTitleFlag()
                                    ? mv->getRuntimeTitleFlag()
                                    : (llvm::Value*) llvm::ConstantInt::get(
                                          tsI64, 1);
                            } else if (auto rn = dynamic_pointer_cast<
                                           NewExpression>(rhsAst)) {
                                ownedVal = llvm::ConstantInt::get(tsI64,
                                    rn->getStackAlloc() ? 0 : 1);
                            } else if (auto rc = dynamic_pointer_cast<
                                           MethodCallExpression>(rhsAst)) {
                                ownedVal = llvm::ConstantInt::get(tsI64,
                                    rc->isResolvedReturnsOwnership() ? 1 : 0);
                            } else {
                                ownedVal = llvm::ConstantInt::get(tsI64, 0);
                            }
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
                            // idx = (slot - hdr - hs) / es, from the already-
                            // emitted slot address (no index re-generation).
                            llvm::Value* slotInt = builder->CreatePtrToInt(
                                lhs, tsI64);
                            llvm::Value* hdrInt = builder->CreatePtrToInt(
                                tailHdr, tsI64);
                            llvm::Value* tsIdx = builder->CreateSDiv(
                                builder->CreateSub(
                                    builder->CreateSub(slotInt, hdrInt),
                                    llvm::ConstantInt::get(tsI64, tsHs)),
                                llvm::ConstantInt::get(tsI64, tsEs));
                            if (llvm::Function* storeFn =
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
                            // An identifier RHS hands its wrapper over only
                            // when the local actually HOLDS TITLE (it has a
                            // drop entry the deactivation block below will
                            // strip). A borrow-holding local (`String n =
                            // s.outNameOf(); arr[i] = n;`) owns nothing to
                            // hand over — taking its wrapper double-titles
                            // the source's string and the array teardown
                            // frees it under the real owner (the nucleo-frame
                            // U7 Exec.apply schema UAF). Those route to the
                            // alias/resolve-copy store instead.
                            bool takesOwnership =
                                dynamic_pointer_cast<MoveExpression>(rhsAst)
                                || MethodCallExpression::freshOwnedStringTemp(rhsAst);
                            // Mode-carrying `#=`: a move of a plain formal
                            // captured the CALLER's word bit — decide owned
                            // (take the wrapper) vs alias (resolve a copy) at
                            // runtime, exactly as the field-store path does.
                            // A null flag keeps the static classification
                            // (declared-`#` formals, fresh temps, elem takes).
                            llvm::Value* takesRt = nullptr;
                            if (auto rtMv = dynamic_pointer_cast<MoveExpression>(
                                    rhsAst)) {
                                takesRt = rtMv->getRuntimeTitleFlag();
                            }
                            if (!takesOwnership) {
                                if (auto rid = dynamic_pointer_cast<
                                        IdentifierExpression>(rhsAst)) {
                                    if (auto sc2 = module->getScopeStack().peek()) {
                                        FieldPtr srcF = sc2->getField(
                                            rid->getTextValue());
                                        takesOwnership = srcF
                                            && srcF->getDropEntry() != nullptr;
                                    }
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
                            // 6.3.2 — field-held / parameter String arrays
                            // (no local sidecar): the raw store aliased the
                            // wrapper, violating the always-own slot
                            // contract (teardown + displacement walks free
                            // every resident wrapper — Headers.grow UAF).
                            // Same title gate as the sidecar branch above: an
                            // identifier hands over its wrapper only when it
                            // holds title; borrow-holding locals alias-copy.
                            bool seTakes =
                                dynamic_pointer_cast<MoveExpression>(rhsAst)
                                || MethodCallExpression::freshOwnedStringTemp(rhsAst);
                            // Mode-carrying `#=` (see the sidecar branch): a
                            // plain formal's captured word bit IS `takes` —
                            // the native already branches move-vs-resolve on
                            // it. Null flag → static classification.
                            llvm::Value* seTakesRt = nullptr;
                            if (auto seMv = dynamic_pointer_cast<MoveExpression>(
                                    rhsAst)) {
                                seTakesRt = seMv->getRuntimeTitleFlag();
                            }
                            if (!seTakes) {
                                if (auto rid = dynamic_pointer_cast<
                                        IdentifierExpression>(rhsAst)) {
                                    if (auto sc2 = module->getScopeStack().peek()) {
                                        FieldPtr srcF = sc2->getField(
                                            rid->getTextValue());
                                        seTakes = srcF
                                            && srcF->getDropEntry() != nullptr;
                                    }
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
                // Ownership transfer into a class-typed array slot.
                // When `arr[i] = local` stores a heap-owned class
                // pointer, the slot now owns the reference; the
                // source local's drop must NOT fire at end-of-scope
                // or end-of-iteration (which would free the instance
                // whose pointer arr[i] still holds — leaving it
                // dangling). Mirrors AggregateInitializer's
                // ownership-into-field move and the lambda
                // #-capture transfer paths.
                // title-tracking §5.1.2 (Unit 4): a slot-bit store already
                // recorded the spelling — a plain store there is a BORROW
                // and the source keeps its books, so this implicit
                // deactivation is skipped. Field-held / param arrays (no
                // sidecar) keep the legacy transfer until Unit 6 sweeps
                // the containers.
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
                                    // sc->demoteToBorrow here. The source
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
                                    // necessary half; demoteToBorrow is
                                    // too aggressive for the
                                    // ownership-transfer-into-array
                                    // shape.
                                }
                            }
                        }
                    }
                }
                // title-tracking Unit 4 close-out discovery — STATIC fields
                // sit outside the bit system (fieldHasOwnershipBit excludes
                // them: there is no per-instance word for a global), so a
                // plain store into `Reg.shared` records no spelling; without
                // this block the source local's drop fires at scope exit and
                // the singleton `instance()` shape returns a freed object
                // (the IfxRegistry/Ws/Https SIGSEGVs — poisoned reads
                // through the static). Restore the pre-3A implicit transfer
                // for exactly the STATIC-field shape. Non-static bit-less /
                // fob-disengaged stores keep 3A borrow semantics — a broader
                // !fobWordPtr gate deactivated borrow sources inside stream
                // template monomorphs (opaque layouts disengage the fob
                // machinery) and crashed ParallelStreamP1. Statics get a
                // real owner story with the Unit 5 `#?` ABI.
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
                                    if (llvm::Value* entry =
                                            srcField->getDropEntry()) {
                                        if (llvm::Function* mark =
                                                module->getRuntimeFunction(
                                                    "__cajeta_drop_mark_inactive")) {
                                            builder->CreateCall(mark, {entry});
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // title-tracking §5.1.2 (Unit 3) — the field bit records the
                // store's spelling. The pre-store block above computed the
                // word address, bit index, spelling, and released a displaced
                // owned value; here the new bit lands. The IMPLICIT transfer
                // on plain field stores (the old deactivate-the-source-entry
                // block) is REMOVED for BIT-CARRYING fields: a plain store
                // there is a borrow store — the source keeps its books (spec
                // §1.2 problem 2). Owned spellings carry their own bookkeeping
                // (`#x` deactivates via MoveExpression; fresh rvalues have no
                // local entry). Bit-less fields keep legacy transfer (block
                // above).
                if (fobWordPtr && fobBitIdx >= 0) {
                    auto& fobCtx2 = *module->getLlvmContext();
                    llvm::Type* i64Ty2 = llvm::Type::getInt64Ty(fobCtx2);
                    llvm::Value* w = builder->CreateLoad(
                        i64Ty2, fobWordPtr, "own_bits");
                    uint64_t mask = 1ULL << fobBitIdx;
                    if (fobOwnedSpelling && fobRuntimeFlag) {
                        // 5.2.2 — `#formal` source: the field's bit is the
                        // formal's flag (surrendered → owned, lent → borrow).
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
                    builder->CreateStore(w, fobWordPtr);
                    // 5.2.7 — a PLAIN store LENDS: the field borrows, the
                    // source local keeps the title and dies at scope exit. If
                    // the receiver later escapes, it carries a dangling
                    // pointer. Record the edge (receiver -> lent local); the
                    // escape sites check it. `#x` spellings own -> no edge.
                    if (!fobOwnedSpelling) {
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
                    // An owned spelling from a `#`-formal source is the
                    // Exception-ctor idiom: the formal's title moves into
                    // the field. The formal has no local drop entry, so no
                    // deactivation is needed; a `#x` local source already
                    // deactivated in MoveExpression.
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
                // 9.3.1 — a move-assign transfers ownership INTO the local:
                // `String d; d = #t;` needs the value dropped at d's scope
                // exit. The runtime drop chain is strict LIFO (pop-mismatch
                // aborts), so pushing a NEW entry here would land it in the
                // ASSIGNING block's frame — freed too early when d is
                // declared in an outer scope (`String keep; { keep = #t; }`,
                // the substringOutlivesSource shape). Instead
                // LocalVariableDeclaration registers a null-obj entry at the
                // DECLARATION (drop of null no-ops), and the move RETARGETS
                // it in place: e->obj = moved ptr, e->active = 1. No chain
                // mutation, so LIFO and frame membership hold. A repeated
                // move-assign orphans the prior value (the documented
                // reassign-leak family); borrow-initialized locals have no
                // entry and keep today's behavior.
                // Retarget fires for: a move-assign RHS (`d = #t`, String per
                // 9.3.1 and class refs per title-tracking 2.2.4), or a FRESH-
                // owner RHS re-arming a moved-out binding (title-tracking
                // §3.1.4 — `piece = heap Cell(...)` after `take(#piece)`;
                // keyed on lhsWasMoved so a never-moved binding keeps its
                // entry on the displaced value, today's behavior).
                {
                    bool rhsIsMove =
                        (bool) dynamic_pointer_cast<MoveExpression>(rhsAst);
                    bool rhsIsFreshOwner = false;
                    if (auto rhsNew = dynamic_pointer_cast<NewExpression>(rhsAst)) {
                        rhsIsFreshOwner = !rhsNew->getStackAlloc();
                    } else if (auto rhsCall =
                            dynamic_pointer_cast<MethodCallExpression>(rhsAst)) {
                        rhsIsFreshOwner = rhsCall->isResolvedReturnsOwnership();
                    }
                    if (rhsIsMove || (lhsWasMoved && rhsIsFreshOwner)) {
                    if (auto lhsId = dynamic_pointer_cast<IdentifierExpression>(lhsAst)) {
                        auto lhsClass = dynamic_pointer_cast<CajetaClass>(
                            lhsAst->getResolvedType());
                        bool lhsIsString = lhsClass && lhsClass->getQName()
                            && lhsClass->getQName()->getTypeName() == "String"
                            && lhsClass->getQName()->getPackageName() == "cajeta.lang";
                        bool lhsIsClassRef = lhsClass && !lhsIsString
                            && !lhsClass->isValueType()
                            && !lhsClass->isSharedCapableValue()
                            && !lhsClass->isInterface();
                        if (lhsIsString || lhsIsClassRef) {
                            if (auto sc = module->getScopeStack().peek()) {
                                FieldPtr dstField = sc->getField(lhsId->getTextValue());
                                if (dstField && dstField->getDropEntry()
                                        && rhsVal && rhsVal->getType()->isPointerTy()) {
                                    llvm::Value* entry = dstField->getDropEntry();
                                    auto& ectx = *module->getLlvmContext();
                                    builder->CreateStore(rhsVal, entry);
                                    llvm::Value* activeSlot =
                                        builder->CreateInBoundsGEP(
                                            llvm::Type::getInt8Ty(ectx), entry,
                                            llvm::ConstantInt::get(
                                                llvm::Type::getInt64Ty(ectx), 24),
                                            "mvassign.active");
                                    builder->CreateStore(
                                        llvm::ConstantInt::get(
                                            llvm::Type::getInt8Ty(ectx), 1),
                                        activeSlot);
                                }
                            }
                        }
                    }
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
                    // Unwrap class String → char* via the mode-aware runtime
                    // accessor (6.2.2 tagged core: Inline and windowed forms
                    // materialize into a per-thread ring scratch; full-window
                    // owned/static roots hand out their NUL-terminated data
                    // directly).
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
                            // Widen and format by the operand's OWN signedness:
                            // a uint64 past 2^63 has no signed rendering, and
                            // the int64 path silently prints it negative.
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
                    // Lever #1: reduce every operand to a (ptr, len) pair, then a
                    // single memcpy each into the result buffer. An integer operand
                    // is formatted ONCE into a per-site stack scratch buffer via
                    // __cajeta_i64_to_buf (no malloc, no snprintf, and — unlike the
                    // first cut — no separate length pass): the call returns the
                    // length, reused for both sizing and the copy. Non-integer
                    // operands (class String, char*, float, bool) go through
                    // `stringify` to a char* + strlen.
                    llvm::Function* strlenFn = module->getRuntimeFunction("__cajeta_str_len");
                    llvm::Function* i64BufFn = module->getRuntimeFunction("__cajeta_i64_to_buf");
                    llvm::Function* parentFn0 = builder->GetInsertBlock()->getParent();
                    struct ConcatOp { bool isInt; llvm::Value* iv; llvm::Value* ptr; llvm::Value* len; };
                    auto classify = [&](llvm::Value* v, CajetaTypePtr vt) -> ConcatOp {
                        if (!isClassStringType(vt)) {
                            llvm::Type* t = v->getType();
                            if (t->isIntegerTy() && !t->isIntegerTy(1)) {
                                // Same signedness rule as `stringify`: an
                                // unsigned operand widens zero-extended and
                                // formats through the u64 helper.
                                const bool uns = vt
                                    && (vt->getTypeFlags() & PRIMITIVE_FLAG) != 0
                                    && (vt->getTypeFlags() & SIGNED_FLAG) == 0;
                                llvm::Value* iv = builder->CreateIntCast(
                                    v, i64Ty, /*isSigned=*/!uns);
                                // Entry-block scratch (24 bytes: 20-digit i64 + sign +
                                // slack), hoisted + reused across loop iterations.
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
                            // 6.2.2 tagged core: Inline text lives at the aux
                            // slot's address (12 in-struct bytes); pointer
                            // forms read base + 8 + aux. Selects, no branches
                            // (the unselected arm's GEP is plain, not
                            // inbounds, so a garbage base can't poison).
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
                        // Class String: take byteLength (slot 2) — bytes.data is
                        // NUL-terminated only for literals; a String wrapped over
                        // a bare byte array (String(#arr, n)) has no terminator
                        // and strlen reads past its logical end.
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
                        // Bootstrap fallback (class String not loaded yet): keep
                        // the legacy char* concat. Int operands take the malloc
                        // stringify here (rare runtime-parse window only).
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

                    // Direct concat: write both operands straight into the result
                    // String's storage — no intermediate __cajeta_str_concat
                    // malloc/copy/free. Results <= 12 B build the INLINE form
                    // (6.2.2 tagged core: the text bytes live in the aux+base
                    // slots, no buffer at all — the small-string fast path that
                    // keeps `"key" + i` off the heap; the old 23-byte SSO region
                    // is gone). Longer results build an OWNED root
                    // {count word, text, NUL} with lenTag = total (no tag bits).
                    const int64_t SSO_CAP = 12;   // Inline capacity
                    // Arena routing (frame-arena-plan U2): when the escape pre-pass
                    // proved this concat's result is a non-escaping local, allocate
                    // the wrapper (and any heap byte buffer) from the frame arena —
                    // no malloc, no live-set; reclaimed by the scope's arena reset.
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

                    // Allocate the wrapper (size includes the inline SSO region).
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

                    // Inline vs heap branch.
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

                    // Inline: the text destination IS the aux slot's address
                    // (12 in-struct bytes spanning aux + base); no buffer.
                    builder->SetInsertPoint(ssoBB);
                    llvm::Value* inlDst = builder->CreateStructGEP(
                        stringStructTy, sPtr, 2, "concat.inl_dst");
                    builder->CreateBr(copyBB);

                    // Heap: alloc { i64 count; data; NUL }; count = total; the
                    // wrapper adopts it as its OWNED root (aux = 0, base = hdr).
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

                    // Merge on the text destination.
                    builder->SetInsertPoint(copyBB);
                    llvm::PHINode* dataPtr = builder->CreatePHI(ptrTy, 2, "concat.data");
                    dataPtr->addIncoming(inlDst, ssoBB);
                    dataPtr->addIncoming(heapData, heapBB);
                    llvm::PHINode* isInlPhi = builder->CreatePHI(
                        llvm::Type::getInt1Ty(llvmCtx), 2, "concat.was_inl");
                    isInlPhi->addIncoming(llvm::ConstantInt::getTrue(llvmCtx), ssoBB);
                    isInlPhi->addIncoming(llvm::ConstantInt::getFalse(llvmCtx), heapBB);

                    // Copy both operands (already reduced to (ptr,len), ints
                    // pre-formatted into stack scratch), then NUL-terminate.
                    // For the Inline arm total <= 12, so the NUL at data[total]
                    // can land at most on cachedCpLength's first byte — which
                    // the unconditional ccp store below rewrites.
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

                    // lenTag = total (Inline <= 12 and OWNED roots both carry
                    // no tag bits); cachedCpLength = -1.
                    builder->CreateStore(totalI32,
                        builder->CreateStructGEP(stringStructTy, sPtr, 1,
                            "concat.s_lentag"));
                    builder->CreateStore(llvm::ConstantInt::get(i32Ty, -1),
                        builder->CreateStructGEP(stringStructTy, sPtr, 4,
                            "concat.s_cachedCpLength"));

                    // Pin resolvedType so a caller using this concat as a ctor /
                    // method argument recovers the class type (mirrors
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
                    // Unsigned cmp catches both r >= width and (signed) r < 0
                    // since negative i32 becomes a huge unsigned value.
                    llvm::Value* bad = builder->CreateICmpUGE(r, widthC, "ubt.shr.over");
                    emitUbTrap(module, *builder, bad, "shr");
                }
                // `>>` follows the SHIFTED operand's signedness: arithmetic
                // for a signed type, logical for an unsigned one. Only the
                // LEFT flags may be consulted — the right operand is a
                // shift COUNT, and its signedness says nothing about the
                // fill bit. (Folding in rhs the way `/` does would make
                // `someUint64 >> 33` arithmetic, because the literal count
                // is a signed int32 — which is exactly how a uint64 came
                // to shift in ones.) `>>>` stays unconditionally logical.
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
                // class-typed LHS (docs/OperatorOverloading.md §6).
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
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs),
                    (lhsTypeFlags & SIGNED_FLAG) != 0,
                    (rhsTypeFlags & SIGNED_FLAG) != 0);
                llvm::Value* newVal = nullptr;
                bool isFp = l->getType()->isFloatingPointTy();
                // Same usual-arithmetic-conversion rule as the standalone
                // `/` and `%`: unsigned wins once either operand's type is
                // actually known.
                bool isSigned = intOpIsSigned;
                // Signed-overflow check for the arithmetic compound ops
                // mirrors the standalone +/-/× path. Sign is read from
                // the AST's resolvedType so a uint*-typed lhs/rhs skips
                // the check (modular wrap is well-defined for unsigned).
                auto signedFromAst = [](ExpressionPtr a, ExpressionPtr b) -> bool {
                    return binaryOverflowIsSigned(a, b);
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
                    // As with the standalone `>>`: the fill bit follows the
                    // SHIFTED operand alone, never the count's signedness.
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
                // fat-aware interface `== null` (null-owned-interface-arg
                // follow-up). An interface value is a 24-byte
                // `{ ptr data, ptr vtable, i64 kind }`, and both shapes hand
                // back a pointer to that BODY — a field's GEP is the body
                // address, a local's slot loads to it. Comparing that pointer
                // against null therefore compared the body's ADDRESS, which is
                // never null, so a null interface was not observable at all:
                // `nullIntoOwnedInterfaceFormal` could prove one CONSTRUCTS,
                // STORES and DROPS without faulting, but `f == null` always
                // answered false. Compare the body's DATA word instead — the
                // null case memsets the body to zero, so data == null is
                // exactly "empty".
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
                    if (ifaceCls && bodyPtr && bodyPtr->getType()->isPointerTy()) {
                        llvm::Type* bodyTy = ifaceCls->getLlvmType();
                        if (bodyTy && bodyTy->isStructTy()) {
                            llvm::Type* ptrTy = llvm::PointerType::get(
                                *module->getLlvmContext(), 0);
                            llvm::Value* dataSlot = builder->CreateStructGEP(
                                bodyTy, bodyPtr, 0, "iface_null_data");
                            llvm::Value* data =
                                builder->CreateLoad(ptrTy, dataSlot);
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
                // Signedness from the AST's resolved types AS WELL AS getTypeFlagsOf.
                // getTypeFlagsOf keys on the LLVM value's type (i32 for BOTH int32
                // and uint32) and can lose the signed distinction — notably when an
                // operand is the int32 RESULT of an interface method dispatch. That
                // produced an UNSIGNED compare that breaks negatives: `7 > -1000`
                // became `7 > 0xFFFFFC18` → false (the ifx registry's priority
                // selection never beat the -1000 null floor). The +/-/* cases already
                // source signedness from the AST for the same reason; mirror them
                // here. ORing both sources only ever ADDS signed-ness for an operand
                // the AST knows is signed — a genuine uint32 carries no SIGNED_FLAG in
                // either source, so unsigned compares are unaffected.
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
                // Handled at the top of the function via short-circuit branching.
                break;
        }
        // Script units (script-units spec §4.3): assignment to a top-level
        // session binding REBINDS it — the runtime drops the previous
        // occupant and takes ownership of the stored value. Emitted after
        // whichever store arm ran, by reloading the local's slot, so every
        // assignment shape is covered uniformly. Stock assignment emits no
        // old-value drop for an owner local, so the runtime's rebind drop is
        // the only one — no double-drop.
        if (binaryOp == BINARY_OP_ASSIGN && module->isScriptUnit()) {
            if (auto lhsId =
                    dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                const std::string& name = lhsId->getTextValue();
                if (module->isScriptBindingName(name)) {
                    auto* builder = module->getBuilder();
                    llvm::Function* pfn =
                        builder->GetInsertBlock()->getParent();
                    if (pfn && pfn->getName().find(scriptEntryName())
                                   != llvm::StringRef::npos) {
                        FieldPtr f;
                        if (auto sc = module->getScopeStack().peek()) {
                            f = sc->getField(name);
                        }
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
                        llvm::Function* bindFn = module->getRuntimeFunction(
                            "__cajeta_session_bind");
                        if (f && dropFn && bindFn) {
                            auto& sctx = *module->getLlvmContext();
                            llvm::Value* cur = builder->CreateLoad(
                                llvm::PointerType::get(sctx, 0),
                                f->getOrCreateAllocation());
                            llvm::Value* nameStr =
                                builder->CreateGlobalString(name);
                            builder->CreateCall(bindFn,
                                {nameStr, cur, dropFn});
                        }
                    }
                }
            }
        }
        return result;
    }

} // code