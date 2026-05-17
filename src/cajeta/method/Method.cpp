//
// Created by James Klappenbach on 2/19/22.
//

#include "Method.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaStruct.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../compile/Compiler.h"
#include "../error/VariableAssignmentException.h"
#include "../error/Exception.h"
#include "../asn/DefaultBlock.h"
#include "../type/FormalParameter.h"
#include "../field/ParameterField.h"
#include "../util/Printer.h"

using namespace std;

#define ERROR_CAUSE_ASSIGNMENT_FINAL        "the field is declared final"
#define ERROR_CAUSE_VARIABLE_NOT_FOUND      "the field was not declared"
#define ERROR_CAUSE_VARIABLE_DUPLICATE      "the field name already exists"
#define ERROR_ID_ASSIGNMENT_FINAL           "CAJETA_ERROR_FINAL_ASSIGNMENT"
#define ERROR_ID_VARIABLE_NOT_FOUND         "CAJETA_ERROR_VARIABLE_NOT_FOUND"
#define ERROR_ID_VARIABLE_DUPLICATE         "CAJETA_ERROR_VARIABLE_DUPLICATE"

namespace cajeta {
    map<string, MethodPtr> Method::archive;

    Method::Method(CajetaModulePtr module,
        string& name,
        CajetaTypePtr returnType,
        vector<FormalParameterPtr> parameterList,
        BlockPtr block,
        CajetaClassPtr parent) {
        this->module = module;
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        this->parameterList = parameterList;
        this->block = block;
        // Constructor detection: name matches parent's type name. For a
        // template instantiation (e.g. parent = "Container<int32>") the
        // source-parsed ctor is named after the unparameterized template
        // ("Container") — fall through to the template-origin's name so
        // it still gets recognized as a ctor. Without this, the source
        // ctor gets registered as a regular method, ensureDefaultConstructor
        // adds an empty default, and `new Container<int32>()` calls the
        // empty default instead of the source body.
        constructor = parent->getQName()->getTypeName() == name;
        if (!constructor && parent->getTemplateOrigin()) {
            constructor = parent->getTemplateOrigin()->getQName()->getTypeName() == name;
        }
        llvmBasicBlock = nullptr;
    }

    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    Method::Method(CajetaModulePtr module,
        string name,
        CajetaTypePtr returnType,
        CajetaClassPtr parent) {
        this->module = module;
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = make_shared<DefaultBlock>();
        llvmBasicBlock = nullptr;
    }

    map<string, MethodPtr>& Method::getArchive() { return archive; }

    void Method::setBlock(BlockPtr block) {
        this->block = block;
    }

    // Emit the body of an @Native-annotated method: a thin wrapper that
    // forwards all parameters to the named C runtime symbol. The wrapper
    // function (this method's llvmFunction) keeps its cajeta-mangled name
    // so user-side calls resolve normally; the body is `return targetFn(
    // args...)`. LLVM's optimizer inlines this single-call wrapper at any
    // optimization level, so the cost after passes is identical to
    // calling the runtime function directly. Signature mismatch between
    // the cajeta declaration and the runtime symbol falls on the user —
    // the declaration must mirror the runtime function's types exactly.
    void Method::emitNativeForwardingBody(const std::string& symbol) {
        llvm::Module* lmod = module->getLlvmModule();
        // Insert (or reuse) the extern declaration of the runtime symbol.
        llvm::Function* targetFn = lmod->getFunction(symbol);
        if (!targetFn) {
            targetFn = llvm::Function::Create(
                llvmFunctionType,
                llvm::Function::ExternalLinkage,
                symbol,
                lmod);
        }

        llvmBasicBlock = llvm::BasicBlock::Create(
            *module->getLlvmContext(), "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        std::vector<llvm::Value*> args;
        args.reserve(llvmFunction->arg_size());
        for (auto& arg : llvmFunction->args()) {
            args.push_back(&arg);
        }

        if (llvmFunction->getReturnType()->isVoidTy()) {
            b.CreateCall(targetFn, args);
            b.CreateRetVoid();
        } else {
            llvm::Value* result = b.CreateCall(targetFn, args);
            b.CreateRet(result);
        }
    }

    void Method::emitTopFrameDrops(CajetaModulePtr module) {
        if (dropFrameStack.empty() || dropFrameStack.back().empty()) return;
        llvm::Function* popRun = module->getRuntimeFunction("__cajeta_drop_pop_run");
        if (!popRun) return;
        auto* b = module->getBuilder();
        auto& frame = dropFrameStack.back();
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            b->CreateCall(popRun, {*it});
        }
    }

    void Method::emitOwnerDrops(CajetaModulePtr module) {
        if (dropFrameStack.empty()) return;
        llvm::Function* popRun = module->getRuntimeFunction("__cajeta_drop_pop_run");
        if (!popRun) return;
        auto* b = module->getBuilder();
        // Fire each frame inner→outer (the LIFO direction). Within each
        // frame, fire entries in reverse declaration order. Frames stay
        // on the stack — enclosing Block::generateCode will observe the
        // terminator and skip its own emitTopFrameDrops on the way out.
        for (auto fit = dropFrameStack.rbegin(); fit != dropFrameStack.rend(); ++fit) {
            for (auto eit = fit->rbegin(); eit != fit->rend(); ++eit) {
                b->CreateCall(popRun, {*eit});
            }
        }
    }

    // Emit one advice call. Validates v1 constraints (static, no
    // params, void return) and skips advice methods that don't meet
    // them — A12 will add diagnostics, but for A4 the simplest forms
    // route is: match the v1 shape or be silently skipped at codegen.
    // The advice method's LLVM Function pointer is the one
    // Method::generatePrototype installed during Phase 1, so it's
    // available here regardless of which module owns the aspect.
    static void emitOneAdviceCall(CajetaModulePtr module, const MethodPtr& advice) {
        if (!advice) return;
        llvm::Function* fn = advice->getLlvmFunction();
        if (!fn) return;
        // v1 constraint: zero non-implicit parameters. Static methods
        // have no `this` slot; non-static methods have one. v1 advice
        // is static-only, so the LLVM signature should have zero
        // params (and any non-zero signature means the user wrote a
        // shape we don't support yet — JoinPoint / annotation
        // instance args ship in A5+). Skip without emitting rather
        // than emit a verifier-rejecting call with garbage args.
        if (fn->getFunctionType()->getNumParams() != 0) return;
        module->getBuilder()->CreateCall(fn, {});
    }

    void Method::emitBeforeAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::Before) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitAfterAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::After) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitAfterReturningAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::AfterReturning) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    void Method::emitAfterThrowingAdvice(CajetaModulePtr module) {
        for (auto& m : matchingAdvice) {
            if (m.kind != AdviceKind::AfterThrowing) continue;
            emitOneAdviceCall(module, m.adviceMethod);
        }
    }

    bool Method::hasAfterThrowingAdvice() const {
        for (auto& m : matchingAdvice) {
            if (m.kind == AdviceKind::AfterThrowing) return true;
        }
        return false;
    }

    // Shared try-frame allocation. Mirrors TryStatement::generateCode's
    // shape: __cajeta_exc_push pushes a 512-byte frame onto the per-
    // thread exception chain, setjmp records the recovery point. On
    // normal flow setjmp returns 0 and we branch to tryBB. On a
    // longjmp (from __cajeta_throw inside the body), setjmp returns
    // non-zero and we branch to catchBB. The 512-byte size matches
    // what TryStatement uses and is documented at the runtime's
    // __cajeta_exc_frame_size() (cajeta_runtime.c).
    Method::TryFrameInfo Method::emitAfterThrowingTryEntry(
            CajetaModulePtr module, llvm::IRBuilder<>& wb,
            llvm::Function* parentFn) {
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        TryFrameInfo info{nullptr, nullptr, nullptr};
        llvm::Function* push = module->getRuntimeFunction("__cajeta_exc_push");
        if (!push) return info;
        llvm::Function* setjmpFn = lmod->getFunction("setjmp");
        if (!setjmpFn) {
            llvm::FunctionType* sjt = llvm::FunctionType::get(
                i32Ty, {ptrTy}, false);
            setjmpFn = llvm::Function::Create(
                sjt, llvm::Function::ExternalLinkage, "setjmp", lmod);
            setjmpFn->addFnAttr(llvm::Attribute::ReturnsTwice);
        }

        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> entryBuilder(
            &parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        info.framePtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes));

        info.tryBB = llvm::BasicBlock::Create(ctx, "afterthrow_try", parentFn);
        info.catchBB = llvm::BasicBlock::Create(ctx, "afterthrow_catch", parentFn);

        wb.CreateCall(push, {info.framePtr});
        llvm::Value* sjResult = wb.CreateCall(setjmpFn, {info.framePtr});
        llvm::Value* threw = wb.CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        wb.CreateCondBr(threw, info.catchBB, info.tryBB);
        wb.SetInsertPoint(info.tryBB);
        return info;
    }

    void Method::emitAfterThrowingTryPop(CajetaModulePtr module) {
        if (!hasAfterThrowingAdvice()) return;
        // Only valid when we're inside the BODY-level try frame.
        // @Around-wrapped methods emit the body into
        // llvmOriginalFunction without a frame at that level — the
        // wrapper owns the try frame and pops it in
        // emitAroundWrapper. Without this guard, ReturnStatement
        // inside the original body would emit a stray exc_pop that
        // unbalances the chain.
        if (llvmOriginalFunction) return;
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        if (!pop) return;
        module->getBuilder()->CreateCall(pop, {});
    }

    void Method::emitAfterThrowingCatchArm(
            CajetaModulePtr module, llvm::IRBuilder<>& wb) {
        llvm::Function* getThrown = module->getRuntimeFunction("__cajeta_get_thrown");
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!getThrown || !pop || !throwFn) return;

        llvm::Value* thrownVal = wb.CreateCall(getThrown, {}, "afterthrow_value");
        wb.CreateCall(pop, {});
        // Swap the module-level builder so the emitAdvice helpers
        // target the catch arm. Restore afterward — the caller may
        // expect its own builder state.
        auto* prevBuilder = builder;
        auto* modPrev = module->getBuilder();
        builder = &wb;
        module->setBuilder(&wb);
        emitAfterThrowingAdvice(module);
        // @After fires on every exit path per the spec; the throw
        // path is one of them. (A4 deferred this; A6 closes the
        // gap as part of landing the try/catch wrapping.)
        emitAfterAdvice(module);
        builder = prevBuilder;
        module->setBuilder(modPrev);
        wb.CreateCall(throwFn, {thrownVal});
        wb.CreateUnreachable();
    }

    void Method::createLocalVariable(CajetaModulePtr module, FieldPtr field) {
        ScopePtr scope = module->getScopeStack().peek();
        if (scope->getField(field->getName()) != nullptr) {
            throw VariableAssignmentException(field->getName(),
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_DUPLICATE,
                ERROR_ID_VARIABLE_DUPLICATE);
        }
        field->getOrCreateAllocation();
        scope->putField(field);
    }

    void Method::setLocalVariable(CajetaModulePtr module, string name, llvm::Value* value) {
        ScopePtr scope = module->getScopeStack().peek();
        FieldPtr field = scope->getField(name);
        if (field == nullptr) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_VARIABLE_NOT_FOUND,
                ERROR_ID_VARIABLE_NOT_FOUND);
        }
        if (field->getModifiers().find(FINAL) != field->getModifiers().end()) {
            throw VariableAssignmentException(name,
                field->getType()->getQName()->toCanonical(),
                ERROR_CAUSE_ASSIGNMENT_FINAL,
                ERROR_ID_ASSIGNMENT_FINAL);
        }
        module->getBuilder()->CreateStore(value, field->getOrCreateAllocation());
    }

    void Method::generatePrototype() {
        // S8.4 — refresh returnType and parameter types from canonicalMap.
        //
        // At parse time, a method declared inside `struct Foo` whose
        // return type or parameter type references Foo gets the type
        // resolved to a placeholder CajetaClass (per CajetaType::fromContext's
        // forward-reference handling): the real CajetaStruct hasn't been
        // registered yet because we're still inside buildStructOrViewNode's
        // visit-children pass when the method signature is parsed. When
        // the struct's generatePrototype runs later and overwrites
        // canonicalMap[canonical] with the real CajetaStruct (a fresh
        // shared_ptr — buildStructOrViewNode doesn't reuse the placeholder
        // the way visitClassDeclaration does via fillFromDeclaration), the
        // method still holds the stale placeholder. Downstream
        // dynamic_pointer_cast<CajetaStruct>(returnType) fails, the
        // function signature falls through to the `ptr` return convention,
        // and ReturnStatement emits a struct value — LLVM verify rejects
        // the mismatch. Refresh both sides here, just-in-time.
        auto refreshType = [](CajetaTypePtr t) -> CajetaTypePtr {
            if (!t || !t->getQName()) return t;
            const std::string& canonical = t->getQName()->toCanonical();
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canonical);
            if (it != cmap.end() && it->second && it->second != t) {
                return it->second;
            }
            return t;
        };
        returnType = refreshType(returnType);
        for (auto& p : parameterList) {
            if (p) p->setType(refreshType(p->getType()));
        }

        vector<llvm::Type*> llvmTypes;
        // The implicit `this` parameter is inserted at position 0 for non-
        // static methods. This function gets called multiple times in
        // normal compilation flow (class generatePrototype iterates,
        // visitClassBody re-iterates after the body walk, lazy
        // getLlvmFunctionType fallback, etc.), and each call would
        // re-insert another `this` — ballooning the signature
        // ("this:pointer, this:pointer, ...") and breaking call sites.
        // Skip the insertion if `this` is already at position 0. Other
        // work in this function (re-computing llvmTypes, re-creating
        // the LLVM Function) is idempotent enough that re-running it
        // is harmless.
        bool thisAlreadyInserted = !parameterList.empty()
            && parameterList.front()->getName() == "this";

        // Abstract method (declared on an interface, no body): build the
        // signature so callers can compute its canonical / vtable hash, but
        // don't emit an LLVM function — the concrete class's matching
        // implementation is what dispatch actually targets. Still need to
        // splice in the implicit `this` and build llvmFunctionType so that
        // any vtable-typed indirect call has the right function type.
        if (abstractFlag) {
            bool staticAbstract = modifiers.find(STATIC) != modifiers.end();
            if (!staticAbstract && !thisAlreadyInserted) {
                auto thisParam = make_shared<FormalParameter>(string("this"), CajetaType::of("pointer"));
                thisParam->setParent(shared_from_this());
                parameterList.insert(parameterList.begin(), thisParam);
                parameters[thisParam->getName()] = thisParam;
            }
            for (auto formalParameter: parameterList) {
                llvmTypes.push_back(formalParameter->getType()->getLlvmType());
            }
            llvmFunctionType = llvmTypes.empty()
                ? llvm::FunctionType::get(returnType->getLlvmType(), false)
                : llvm::FunctionType::get(returnType->getLlvmType(), llvmTypes, false);
            return;
        }

        bool staticMethod = modifiers.find(STATIC) != modifiers.end();

        // Static check (Session 3 / Step 3.5): a multi-parameter free function
        // can't return a borrow because there's no rule for picking which
        // parameter's lifetime the return inherits. See
        // MemoryModel.md § Function signatures § Free function, multiple parameters.
        //
        // Instance methods (non-static) are exempt — their borrow-return inherits
        // from `this` by elision, regardless of how many other parameters they take.
        if (staticMethod && !returnsOwnership && returnType
                && returnType->getLlvmType()
                && returnType->getLlvmType()->isPointerTy()
                && parameterList.size() > 1) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "multi-parameter free function '%s' cannot return a borrow; "
                "use `#%s` to return ownership, or reduce to a single parameter",
                name.c_str(),
                returnType->getQName() ? returnType->getQName()->getTypeName().c_str() : "T");
            throw Exception(buf, "CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM");
        }

        if (!staticMethod && !thisAlreadyInserted) {
            auto thisParam = make_shared<FormalParameter>(string("this"), CajetaType::of("pointer"));
            thisParam->setParent(shared_from_this());
            parameterList.insert(parameterList.begin(), thisParam);
            parameters[thisParam->getName()] = thisParam;
        }

        for (auto formalParameter: parameterList) {
            CajetaTypePtr pt = formalParameter->getType();
            llvm::Type* ptLlvm = pt->getLlvmType();
            // Class instances, arrays, AND structs pass by pointer.
            // Structs were pass-by-value historically, but that
            // contradicts their zero-copy view-mode design intent
            // (per cajeta-docs/Views.md): a struct IS a typed view over a
            // wire-format buffer; memcpying the bytes at every call
            // boundary defeats the whole point. Now the call site
            // pushes the struct's address; the callee accesses
            // fields by GEP from the param ptr (same code path
            // that already handled struct-fields-on-classes).
            //
            // Primitives stay by-value. CajetaArray's typeFlags
            // include PRIMITIVE_FLAG (arrays are reference-typed
            // but technically marked primitive), so we test for the
            // CajetaArray subtype explicitly rather than reading
            // the bit. Pass-by-pointer is true for any non-primitive
            // class-like type — and CajetaStruct, CajetaArray, and
            // plain CajetaClass are all CajetaClass subclasses.
            bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
            bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
            bool passByPointer = isClassLike && (isArr || !isPrim);
            if (passByPointer) {
                ptLlvm = llvm::PointerType::get(*module->getLlvmContext(), 0);
            }
            llvmTypes.push_back(ptLlvm);
        }
        // Apply the same pass-by-pointer rule to the return type so
        // class instances, arrays, AND views all travel as `ptr`. The
        // view case is the live one — a returned view is a typed pointer
        // into a buffer the caller owns; the view ctor's IR returns
        // a `ptr`.
        //
        // S6.7 — CajetaStruct returns are the exception: declared
        // return type is the struct's LLVM body type, not `ptr`. The
        // body alloca that ReturnStatement loaded from dies with the
        // callee's stack frame, so returning the pointer would dangle.
        // Returning the struct VALUE lets LLVM's small-struct ABI pack
        // it into a register (or fall back to sret transparently for
        // large structs); the caller alloca's a fresh body and stores
        // the returned value into it (handled in MethodCallExpression's
        // call-site repackaging).
        //
        // S9.5.5 — interface returns get the same treatment: a 24-byte
        // fat-pointer body returned by value via the small-struct ABI
        // (typically sret for 24 bytes on x86-64 SysV), with the caller
        // repackaging into a fresh body alloca. The body might point at
        // a class instance that outlives the call, but the body
        // STRUCTURE itself (the three-word tuple) is callee-local stack
        // for any synthesized interface value, so by-value return is
        // the only safe shape.
        llvm::Type* llvmRet;
        {
            CajetaTypePtr rt = returnType;
            bool isArrR = rt
                && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
            auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
            bool isClassLikeR = rtClass != nullptr;
            bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
            bool isStructR = rt
                && dynamic_pointer_cast<CajetaStruct>(rt) != nullptr;
            bool isInterfaceR = rtClass && rtClass->isInterface();
            bool returnByPointer = isClassLikeR && (isArrR || !isPrimR)
                && !isStructR && !isInterfaceR;
            if (returnByPointer) {
                llvmRet = llvm::PointerType::get(*module->getLlvmContext(), 0);
            } else {
                llvmRet = rt ? rt->getLlvmType() : nullptr;
            }
        }
        if (llvmTypes.size()) {
            llvmFunctionType = llvm::FunctionType::get(llvmRet, llvmTypes, false);
        } else {
            llvmFunctionType = llvm::FunctionType::get(llvmRet, false);
        }

        string canonical = Method::buildCanonical(parent, name, parameterList, true);
        // Generate-prototype runs multiple times on the same method
        // (CajetaClass::generatePrototype iterates, visitClassBody
        // re-iterates, lazy getLlvmFunctionType fallback) — without
        // this reuse, llvm::Function::Create produces a fresh
        // auto-renamed Function (`name.1`, `name.2`...) on each call,
        // orphaning the earlier ones as declarations with no body.
        // Vtable globals captured at the first emission point then
        // reference an orphan declaration — JIT-link unable to find
        // `name` at dispatch time. Reusing the existing Function*
        // keeps llvmFunction stable across reprototype calls.
        if (llvm::Function* existing = module->getLlvmModule()->getFunction(canonical)) {
            llvmFunction = existing;
        } else {
            llvmFunction = llvm::Function::Create(llvmFunctionType, llvm::Function::ExternalLinkage,
                canonical, module->getLlvmModule());
        }

        archive[canonical] = shared_from_this();

        module->getLlvmModule()->getOrInsertFunction(canonical, llvmFunctionType);
    }

    void Method::generateCode() {
        // Abstract methods carry no body — dispatch goes to a concrete
        // implementation via the vtable.
        if (abstractFlag) return;
        if (llvmBasicBlock != nullptr) {
            return;
        }

        // @Native("symbol") — the method's body is a forwarding call to
        // a C runtime function with the given symbol name. Used by
        // stdlib classes to bridge into runtime helpers (cajeta.hash.Hash.
        // processSeed -> __cajeta_hash_seed, cajeta.io.Buffer.allocate ->
        // __cajeta_alloc, etc.). The runtime function is declared extern
        // in this module; its body is provided by the C runtime bitcode
        // / object at link or JIT time. The wrapper is trivially
        // inlinable so call overhead is zero after LLVM passes run.
        if (auto nativeAnn = findAnnotation("Native")) {
            std::string symbol = nativeAnn->getString("value");
            if (symbol.empty()) {
                cerr << "@Native on " << buildCanonical(parent, name, parameterList, true)
                     << " requires a symbol-name argument" << std::endl;
                return;
            }
            emitNativeForwardingBody(symbol);
            return;
        }
        // A5: when @Around advice matched this method, lazily create
        // an extracted-body LLVM Function. matchingAdvice was
        // populated by A3 (CajetaModule::resolveAdviceMatches) which
        // runs between parse and Phase 1; generatePrototype itself
        // runs DURING parse, before A3, so this can't move earlier
        // without breaking the timing. The body emits into
        // llvmOriginalFunction below, leaving llvmFunction free to
        // host the wrapper. External callers' getLlvmFunction()
        // still resolves to llvmFunction (the wrapper), so the
        // advised behavior is the default entry point.
        if (!llvmOriginalFunction) {
            for (auto& m : matchingAdvice) {
                if (m.kind == AdviceKind::Around) {
                    std::string originalName =
                        Method::buildCanonical(parent, name, parameterList, true)
                        + "__original";
                    llvmOriginalFunction = llvm::Function::Create(
                        llvmFunctionType, llvm::Function::ExternalLinkage,
                        originalName, module->getLlvmModule());
                    break;
                }
            }
        }
        llvm::Function* bodyFn = llvmOriginalFunction
            ? llvmOriginalFunction : llvmFunction;
        bool hasAroundWrapper = (llvmOriginalFunction != nullptr);

        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), "entry", bodyFn);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        // Push the enclosing class onto the structure stack so bare
        // method/field references inside the body resolve against it
        // (MethodCallExpression and similar consume structureStack.back()
        // as the implicit target class). The parsing-time push lives
        // only for the duration of parsing; codegen runs later off the
        // already-built AST, so without this we'd return null targets
        // and crash on null-deref downstream. Popped after the body
        // finishes so we don't leak state into the next method's gen.
        bool pushedClass = false;
        if (parent) {
            module->getStructureStack().push_back(parent);
            pushedClass = true;
        }

        createScope();

        int i = 0;
        for (auto& parameter: parameterList) {
            FieldPtr parameterField = make_shared<ParameterField>(module, parameter, bodyFn, i++);
            module->getScopeStack().peek()->putField(parameterField);
        }

        // R5-A' implicit function-body scope: capture the scope_top from the
        // caller's perspective into an alloca, then push the function-body
        // frame. Every return path (synthetic, explicit ReturnStatement)
        // calls __cajeta_scope_exit_to(watermark) so all frames pushed by
        // this method — its implicit frame plus any explicit `scope { }`
        // nested inside — get waited and popped, regardless of how the
        // function exits. Watermark lives in an alloca so multiple return
        // paths share the same value without code duplication.
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        scopeWatermark = builder->CreateAlloca(ptrTy, nullptr, "scope_watermark");
        if (llvm::Function* saveFn = module->getRuntimeFunction(
                "__cajeta_scope_save_top")) {
            llvm::Value* mark = builder->CreateCall(saveFn, {});
            builder->CreateStore(mark, scopeWatermark);
        }
        if (llvm::Function* enterFn = module->getRuntimeFunction(
                "__cajeta_scope_enter")) {
            builder->CreateCall(enterFn, {});
        }

        // A6: when @AfterThrowing matches (and this method isn't
        // @Around-wrapped — the wrapper sets up its own try/catch
        // around the advice call), wrap the body in a try frame.
        // setjmp here records the recovery point; the body executes
        // inside tryBB; on throw, control lands in catchBB which
        // fires @AfterThrowing + @After then re-raises.
        TryFrameInfo bodyTryFrame{nullptr, nullptr, nullptr};
        bool wrapBodyForAfterThrowing =
            !hasAroundWrapper && hasAfterThrowingAdvice();
        if (wrapBodyForAfterThrowing) {
            bodyTryFrame = emitAfterThrowingTryEntry(
                module, *builder, bodyFn);
        }

        // A4: fire @Before advice right after scope_enter (and
        // inside the try frame if any), before the body emits.
        // Sequenced this way so the advice sees the same scope
        // frame the body will populate — useful for advice bodies
        // that might themselves spawn (when A4 relaxes to allow
        // that). Empty matchingAdvice → no-op.
        //
        // A5: when an @Around wrapper exists, @Before fires in the
        // WRAPPER (around the call to advice), not in the extracted
        // original body. The spec's "Multiple aspects on one method"
        // section calls out the flatten-into-wrapper composition.
        if (!hasAroundWrapper) emitBeforeAdvice(module);

        // Implicit super-constructor call. Java semantics: a subclass
        // ctor's body is preceded by `super()` unless the user wrote
        // an explicit super-call (still unsupported — `super` is
        // UnsupportedExpression today). Skip when the parent has no
        // no-arg ctor; classes whose only ctor takes args (e.g.
        // stdlib Throwable(String)) keep the current "manually
        // assign inherited fields" pattern until explicit super(...)
        // lands. Pass `this` (bodyFn arg 0) as the receiver.
        if (constructor && parent && bodyFn->arg_size() > 0) {
            for (auto& sup : parent->getSuperClasses()) {
                if (!sup) continue;
                std::vector<ParameterEntry> noArgs;
                std::string supCtorName = sup->getQName()->getTypeName();
                if (sup->resolveMethod(supCtorName, noArgs,
                        /*isConstructor=*/true, /*floatingParams=*/false)) {
                    sup->invokeMethod(supCtorName, noArgs,
                        /*isConstructor=*/true,
                        bodyFn->getArg(0),
                        /*callerModule=*/module);
                }
                break;  // single inheritance chain; only the first parent
            }
        }

        // Type-resolver pre-pass: populates Expression::resolvedType so codegen can
        // distinguish e.g. fp8 from i8 when they share an LLVM type.
        if (block) {
            block->resolveTypes(module);
            block->generateCode(module);
        }

        // Emit a terminator only if the body didn't (i.e. no explicit return). For void
        // methods this is the conventional "fall through to ret"; for non-void methods
        // a missing return is undefined in Cajeta semantics, but we emit a zero-value
        // ret so the IR remains well-formed.
        if (!builder->GetInsertBlock()->getTerminator()) {
            // A4: fire @After advice BEFORE scope_exit + drops. The
            // advice runs in the method-body lifetime; cleanup
            // happens afterward so the advice can read state still
            // owned by the function. With an @Around wrapper,
            // @After fires in the wrapper instead (same composition
            // rule the @Before hook follows above).
            // A6: @AfterReturning fires on the same hook (normal-
            // return only). Ordered after @After to match the
            // spec's "finally arm" semantics for @After.
            if (!hasAroundWrapper) {
                emitAfterAdvice(module);
                emitAfterReturningAdvice(module);
                // A6: pop the try frame so the caller's exception
                // chain is restored. The throw path pops inside
                // emitAfterThrowingCatchArm; this is the normal-
                // return counterpart.
                if (wrapBodyForAfterThrowing) {
                    emitAfterThrowingTryPop(module);
                }
            }
            // Pop every scope frame this method pushed (function-body + any
            // explicit scopes still open at fall-through) by walking down
            // to the captured watermark. ScopeStatement-managed frames
            // normally pop themselves at their own `}` — they only remain
            // here if the body fell through with one still open, which
            // would be a structural malformation we accept as a defensive
            // cleanup.
            if (llvm::Function* exitToFn = module->getRuntimeFunction(
                    "__cajeta_scope_exit_to")) {
                llvm::Value* mark = builder->CreateLoad(ptrTy, scopeWatermark);
                builder->CreateCall(exitToFn, {mark});
            }
            // Fire scope-end drops before the synthetic return so the chain is
            // unwound the same way an explicit `return` would do it.
            emitOwnerDrops(module);
            llvm::Type* retLlvmTy = returnType ? returnType->getLlvmType() : nullptr;
            if (!retLlvmTy || retLlvmTy->isVoidTy()) {
                builder->CreateRetVoid();
            } else if (retLlvmTy->isFloatingPointTy()) {
                builder->CreateRet(llvm::ConstantFP::getZero(retLlvmTy));
            } else if (retLlvmTy->isPointerTy()) {
                builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(retLlvmTy)));
            } else if (retLlvmTy->isIntegerTy()) {
                builder->CreateRet(llvm::ConstantInt::get(retLlvmTy, 0));
            } else {
                // Aggregate/other — best-effort poison value.
                builder->CreateRet(llvm::PoisonValue::get(retLlvmTy));
            }
        }

        destroyScope();
        if (pushedClass) {
            module->getStructureStack().pop_back();
        }

        // A6 catch-arm emission for the body-level try frame, if
        // we set one up. Reached only via longjmp from a throw
        // inside the body; fires @AfterThrowing + @After advice
        // and re-raises so the throw still propagates outward.
        if (wrapBodyForAfterThrowing && bodyTryFrame.catchBB) {
            llvm::IRBuilder<> catchBuilder(bodyTryFrame.catchBB);
            emitAfterThrowingCatchArm(module, catchBuilder);
        }

        // A5 wrapper emission. If the user body went into
        // llvmOriginalFunction (because @Around matched), emit the
        // wrapper body into llvmFunction now. The wrapper:
        //   1. Fires @Before advice (any kinds in matchingAdvice).
        //   2. Calls the @Around advice, passing llvmOriginalFunction
        //      as the @Original Function<...> proceed argument, then
        //      forwarding all of llvmFunction's own arguments.
        //   3. Fires @After advice.
        //   4. Returns the @Around advice's result.
        // v1 takes the FIRST @Around match only (the spec's
        // "Multiple aspects on one method" defers @Order-driven
        // chaining to A7); additional @Around matches are silently
        // ignored. Empty wrapper is impossible by construction —
        // llvmOriginalFunction is only created when @Around matched.
        if (hasAroundWrapper) {
            emitAroundWrapper();
        }
    }

    void Method::emitAroundWrapper() {
        // A7: collect every @Around match in matchingAdvice order
        // (already sorted by @Order during A3's resolveAdviceMatches
        // pass). aroundChain[0] is the outermost; aroundChain[N-1]
        // wraps the original. Single-Around is just N=1 of the
        // general chain.
        std::vector<MethodPtr> aroundChain;
        for (auto& m : matchingAdvice) {
            if (m.kind == AdviceKind::Around && m.adviceMethod
                    && m.adviceMethod->getLlvmFunction()) {
                aroundChain.push_back(m.adviceMethod);
            }
        }
        if (aroundChain.empty()) {
            // Advice missing or not yet codegen-ready — leave the
            // wrapper unimplemented. The LLVM verifier will flag
            // it; that's actionable feedback even without explicit
            // diagnostics (which join A12).
            return;
        }

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        const llvm::DataLayout& dl = lmod->getDataLayout();
        const size_t N = aroundChain.size();

        // The advice's @Original parameter is typed as a Cajeta
        // function value (`(int32) -> int32 proceed`). At the LLVM
        // level that's a pointer to a closure record
        // `{ ptr fn, ptr captures, ptr drop_fn }`; the call site
        // loads fn, loads captures, calls `fn(captures, args...)`.
        // See LambdaExpression's call emission for the same shape.
        //
        // For N=1: build 1 adapter that calls llvmOriginalFunction,
        // 1 closure wrapping it; wrapper passes the closure to
        // aroundChain[0].
        //
        // For N>1: build N adapters and N closures. closure[k]
        // wraps adapter[k]; closure[k].captures = closure[k+1] so
        // each adapter can forward to the next inner advice via
        // its captures pointer. adapter[N-1] (innermost) calls
        // llvmOriginalFunction. adapter[k] for k<N-1 calls
        // aroundChain[k+1].advice(captures, args) — captures is
        // the next-inner closure, threaded through as the proceed
        // argument the advice's call site expects.

        // Adapter signature: same as llvmFunctionType plus a
        // leading captures ptr.
        std::vector<llvm::Type*> adapterParamTys;
        adapterParamTys.push_back(ptrTy);   // captures
        for (auto* pt : llvmFunctionType->params()) {
            adapterParamTys.push_back(pt);
        }
        llvm::FunctionType* adapterFnTy = llvm::FunctionType::get(
            llvmFunctionType->getReturnType(), adapterParamTys, false);

        std::string baseName =
            Method::buildCanonical(parent, name, parameterList, true);

        // Build adapter[N-1] first (innermost — calls original).
        std::vector<llvm::Function*> adapters(N, nullptr);
        for (size_t k = 0; k < N; ++k) {
            std::string adapterName = baseName
                + "__around_adapter_" + std::to_string(k);
            adapters[k] = llvm::Function::Create(
                adapterFnTy, llvm::Function::ExternalLinkage,
                adapterName, lmod);
            llvm::BasicBlock* adBB = llvm::BasicBlock::Create(
                ctx, "entry", adapters[k]);
            llvm::IRBuilder<> adBuilder(adBB);
            // Collect the forwarded args (skip the captures slot
            // at position 0).
            llvm::Value* capturesArg = nullptr;
            std::vector<llvm::Value*> fwd;
            int i = 0;
            for (auto& a : adapters[k]->args()) {
                if (i++ == 0) { capturesArg = &a; continue; }
                fwd.push_back(&a);
            }
            llvm::CallInst* inner;
            if (k == N - 1) {
                // Innermost: call the original.
                inner = adBuilder.CreateCall(
                    llvmFunctionType, llvmOriginalFunction, fwd);
            } else {
                // Forward to the next inner advice. The next
                // closure is what captures points at; advice's
                // first parameter is its own proceed closure.
                std::vector<llvm::Value*> nextArgs;
                nextArgs.push_back(capturesArg);
                for (auto* v : fwd) nextArgs.push_back(v);
                inner = adBuilder.CreateCall(
                    aroundChain[k + 1]->getLlvmFunctionType(),
                    aroundChain[k + 1]->getLlvmFunction(),
                    nextArgs);
            }
            if (llvmFunctionType->getReturnType()->isVoidTy()) {
                adBuilder.CreateRetVoid();
            } else {
                adBuilder.CreateRet(inner);
            }
        }

        // Wrapper body. Each closure is heap-alloc'd via
        // __cajeta_alloc — matches the lambda-call site's
        // expectations.
        llvm::BasicBlock* wrapperBB = llvm::BasicBlock::Create(
            ctx, "wrapper", llvmFunction);
        llvm::IRBuilder<> wrapBuilder(wrapperBB);

        llvm::StructType* closureTy = llvm::StructType::get(ctx,
            { (llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy });
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            // No __cajeta_alloc — runtime didn't link. Bail; the
            // verifier will flag the missing terminator.
            return;
        }

        // Allocate N closures innermost → outermost. closures[N-1]
        // has captures=null (no next); closures[k<N-1] points its
        // captures slot at closures[k+1].
        std::vector<llvm::Value*> closures(N, nullptr);
        for (size_t k = N; k-- > 0;) {
            llvm::Value* closure = wrapBuilder.CreateCall(allocFn, {
                llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx),
                    dl.getTypeAllocSize(closureTy)),
            }, "around_closure_" + std::to_string(k));
            llvm::Value* fnSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 0, "closure.fn");
            wrapBuilder.CreateStore(adapters[k], fnSlot);
            llvm::Value* capSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 1, "closure.captures");
            llvm::Value* capValue = (k + 1 < N)
                ? closures[k + 1]
                : (llvm::Value*) llvm::ConstantPointerNull::get(ptrTy);
            wrapBuilder.CreateStore(capValue, capSlot);
            llvm::Value* dropSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 2, "closure.drop_fn");
            wrapBuilder.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy), dropSlot);
            closures[k] = closure;
        }

        // Args for the outermost advice call: proceed=closures[0],
        // then the wrapper's forwarded arguments in order.
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(closures[0]);
        for (auto& arg : llvmFunction->args()) {
            callArgs.push_back(&arg);
        }

        // @Before / @After / @AfterReturning emit at the wrapper
        // level (flatten — spec's "Multiple aspects on one method").
        // emitBeforeAdvice/etc. call through module->getBuilder();
        // swap the module-level builder to our wrapper-local one.
        auto* prevBuilder = builder;
        llvm::IRBuilder<>* wb = &wrapBuilder;
        builder = wb;
        module->setBuilder(wb);
        emitBeforeAdvice(module);

        // A6: when @AfterThrowing matched, wrap the OUTERMOST advice
        // call in a try frame. A throw anywhere under the chain
        // (advice bodies, intermediate adapters, or the original)
        // lands in the wrapper's catchBB.
        TryFrameInfo wrapTryFrame{nullptr, nullptr, nullptr};
        if (hasAfterThrowingAdvice()) {
            wrapTryFrame = emitAfterThrowingTryEntry(
                module, *wb, llvmFunction);
        }

        llvm::CallInst* result = wb->CreateCall(
            aroundChain[0]->getLlvmFunctionType(),
            aroundChain[0]->getLlvmFunction(),
            callArgs);

        if (wrapTryFrame.catchBB) {
            llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
            if (pop) wb->CreateCall(pop, {});
        }

        emitAfterAdvice(module);
        emitAfterReturningAdvice(module);

        builder = prevBuilder;
        module->setBuilder(prevBuilder);

        llvm::Type* retTy = llvmFunctionType->getReturnType();
        if (retTy->isVoidTy()) {
            wb->CreateRetVoid();
        } else {
            wb->CreateRet(result);
        }

        if (wrapTryFrame.catchBB) {
            llvm::IRBuilder<> catchBuilder(wrapTryFrame.catchBB);
            emitAfterThrowingCatchArm(module, catchBuilder);
        }
    }

    void Method::createScope() {
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));
    }

    void Method::destroyScope() {
        module->getScopeStack().pop();
    }

    FieldPtr Method::getVariable(string name) {
        ScopePtr scope = module->getScopeStack().peek();
        return scope->getField(name);
    }


    string Method::buildCanonical(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](FormalParameterPtr a, FormalParameterPtr b) {
                return a->getName() > b->getName();
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(parameter->getType()->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildCanonical(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](ParameterEntry a, ParameterEntry b) {
                return a.label > b.label;
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter.label).append(":");
                }
                canonical.append(parameter.type->toCanonical());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildGeneric(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](FormalParameterPtr a, FormalParameterPtr b) {
                return a->getName() > b->getName();
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(parameter->getType()->toGeneric());
            }
        }

        canonical.append(")");
        return canonical;
    }

    string Method::buildGeneric(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled) {
        string canonical;
        canonical.append(parent->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(), [](ParameterEntry a, ParameterEntry b) {
                return a.label > b.label;
            });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (const ParameterEntry& parameter: parameters) {
                if (first) {
                    first = false;
                } else {
                    canonical.append(",");
                }
                if (labeled) {
                    canonical.append(parameter.label).append(":");
                }

                canonical.append(parameter.type->toGeneric());
            }
        }

        canonical.append(")");
        return canonical;
    }

    MethodPtr Method::create(CajetaModulePtr module,
        string& name,
        CajetaTypePtr returnType,
        vector<FormalParameterPtr> parameters,
        BlockPtr block,
        CajetaClassPtr parent) {
        MethodPtr result = make_shared<Method>(module, name, returnType, parameters, block, parent);
        for (auto& parameter: result->parameterList) {
            parameter->setParent(result);
            result->parameters[parameter->getName()] = parameter;
        }
        return result;
    }

    MethodPtr Method::create(CajetaModulePtr module,
        string name,
        CajetaTypePtr returnType,
        CajetaClassPtr parent) {
            return make_shared<Method>(module, name, returnType, parent);
    }

}