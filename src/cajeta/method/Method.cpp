//
// Created by James Klappenbach on 2/19/22.
//

#include "Method.h"
#include "../type/CajetaClass.h"
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
        constructor = parent->getQName()->getTypeName() == name;
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
        vector<llvm::Type*> llvmTypes;

        // Abstract method (declared on an interface, no body): build the
        // signature so callers can compute its canonical / vtable hash, but
        // don't emit an LLVM function — the concrete class's matching
        // implementation is what dispatch actually targets. Still need to
        // splice in the implicit `this` and build llvmFunctionType so that
        // any vtable-typed indirect call has the right function type.
        if (abstractFlag) {
            bool staticAbstract = modifiers.find(STATIC) != modifiers.end();
            if (!staticAbstract) {
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

        if (!staticMethod) {
            auto thisParam = make_shared<FormalParameter>(string("this"), CajetaType::of("pointer"));
            thisParam->setParent(shared_from_this());
            parameterList.insert(parameterList.begin(), thisParam);
            parameters[thisParam->getName()] = thisParam;
        }

        for (auto formalParameter: parameterList) {
            CajetaTypePtr pt = formalParameter->getType();
            llvm::Type* ptLlvm = pt->getLlvmType();
            // Class instances and arrays pass by pointer, not by value.
            // CajetaArray::getTypeFlags() returns ARRAY_TYPE_ID which happens
            // to include PRIMITIVE_FLAG (because arrays are heap-allocated
            // and act like reference types at parameter slots), so we test
            // for CajetaArray and CajetaClass explicitly rather than
            // relying on the primitive bit. Structs (CajetaStruct, declared
            // with `struct`) DO pass by value per WireFormats.md.
            bool isStruct = dynamic_pointer_cast<CajetaStruct>(pt) != nullptr;
            bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
            bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
            bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim);
            if (passByPointer) {
                ptLlvm = llvm::PointerType::get(*module->getLlvmContext(), 0);
            }
            llvmTypes.push_back(ptLlvm);
        }
        // Apply the same pass-by-pointer rule to the return type: a
        // class-like (non-struct) return travels as `ptr`, matching the
        // calling convention the param coercion above uses for class
        // params. Without this the indirect-call type mismatches when a
        // method returns a class instance and the caller stores it into
        // a class-typed local.
        llvm::Type* llvmRet;
        {
            CajetaTypePtr rt = returnType;
            bool isStructR = rt
                && dynamic_pointer_cast<CajetaStruct>(rt) != nullptr;
            bool isArrR = rt
                && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
            bool isClassLikeR = rt
                && dynamic_pointer_cast<CajetaClass>(rt) != nullptr;
            bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
            bool returnByPointer = (isClassLikeR && !isStructR)
                && (isArrR || !isPrimR);
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
        llvmFunction = llvm::Function::Create(llvmFunctionType, llvm::Function::ExternalLinkage,
            canonical, module->getLlvmModule());
        string all;
        for (auto& fn: module->getLlvmModule()->getFunctionList()) {
            cout << fn.getName().str();
            all.append(fn.getName().str()).append(",");
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

        // A4: fire @Before advice right after scope_enter, before the
        // body emits. Sequenced this way so the advice sees the same
        // scope frame the body will populate — useful for advice
        // bodies that might themselves spawn (when A4 relaxes to
        // allow that). Empty matchingAdvice → no-op.
        //
        // A5: when an @Around wrapper exists, @Before fires in the
        // WRAPPER (around the call to advice), not in the extracted
        // original body. The spec's "Multiple aspects on one method"
        // section calls out the flatten-into-wrapper composition.
        if (!hasAroundWrapper) emitBeforeAdvice(module);

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
            if (!hasAroundWrapper) emitAfterAdvice(module);
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
        // Find the first @Around match. v1 supports only one;
        // @Order-driven chaining of multiple Arounds joins A7.
        MethodPtr aroundAdvice;
        for (auto& m : matchingAdvice) {
            if (m.kind == AdviceKind::Around) {
                aroundAdvice = m.adviceMethod;
                break;
            }
        }
        if (!aroundAdvice || !aroundAdvice->getLlvmFunction()) {
            // Advice missing or not yet codegen-ready — leave the
            // wrapper unimplemented. The LLVM verifier will flag
            // it; that's actionable feedback even without explicit
            // diagnostics (which join A12).
            return;
        }

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        // The advice's @Original parameter is typed as a Cajeta
        // function value (`(int32) -> int32 proceed` in source).
        // At the LLVM level that's a pointer to a closure record
        // `{ ptr fn, ptr captures, ptr drop_fn }`, and the call
        // site loads fn, loads captures, then calls
        // `fn(captures, args...)` — the captures ptr is always the
        // first synthetic argument. See LambdaExpression's call
        // emission for the same shape.
        //
        // Two pieces to synthesize:
        //   1. An adapter fn matching the closure-call signature
        //      (captures-ptr + original args). Its body ignores
        //      captures and forwards args to llvmOriginalFunction.
        //   2. A heap-allocated closure record pointing at the
        //      adapter, with captures=null, drop_fn=null. The
        //      wrapper passes a pointer to this record as the
        //      proceed argument.
        std::vector<llvm::Type*> adapterParamTys;
        adapterParamTys.push_back(ptrTy);   // captures (unused)
        for (auto* pt : llvmFunctionType->params()) {
            adapterParamTys.push_back(pt);
        }
        llvm::FunctionType* adapterFnTy = llvm::FunctionType::get(
            llvmFunctionType->getReturnType(), adapterParamTys, false);
        std::string adapterName =
            Method::buildCanonical(parent, name, parameterList, true)
            + "__original_adapter";
        llvm::Function* adapterFn = llvm::Function::Create(
            adapterFnTy, llvm::Function::ExternalLinkage,
            adapterName, lmod);
        {
            llvm::BasicBlock* adBB = llvm::BasicBlock::Create(
                ctx, "entry", adapterFn);
            llvm::IRBuilder<> adBuilder(adBB);
            std::vector<llvm::Value*> fwd;
            int i = 0;
            for (auto& a : adapterFn->args()) {
                if (i++ == 0) continue;  // skip captures
                fwd.push_back(&a);
            }
            llvm::CallInst* inner = adBuilder.CreateCall(
                llvmFunctionType, llvmOriginalFunction, fwd);
            if (llvmFunctionType->getReturnType()->isVoidTy()) {
                adBuilder.CreateRetVoid();
            } else {
                adBuilder.CreateRet(inner);
            }
        }

        // Build the wrapper body now. The closure record alloc
        // happens here so it can be heap-resident for the duration
        // of the advice call. (Stack-allocating would be safe too
        // since the advice can't store the closure anywhere that
        // outlives this frame, but the existing closure shape uses
        // __cajeta_alloc and the lambda-call site doesn't care.)
        llvm::BasicBlock* wrapperBB = llvm::BasicBlock::Create(
            ctx, "wrapper", llvmFunction);
        llvm::IRBuilder<> wrapBuilder(wrapperBB);

        llvm::StructType* closureTy = llvm::StructType::get(ctx,
            { (llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy });
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        llvm::Value* closure = nullptr;
        if (allocFn) {
            closure = wrapBuilder.CreateCall(allocFn, {
                llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx),
                    dl.getTypeAllocSize(closureTy)),
            }, "around_closure");
            llvm::Value* fnSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 0, "closure.fn");
            wrapBuilder.CreateStore(adapterFn, fnSlot);
            llvm::Value* capSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 1, "closure.captures");
            wrapBuilder.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy), capSlot);
            llvm::Value* dropSlot = wrapBuilder.CreateStructGEP(
                closureTy, closure, 2, "closure.drop_fn");
            wrapBuilder.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy), dropSlot);
        } else {
            // No __cajeta_alloc means the runtime didn't link.
            // Without a closure record there's nothing to pass as
            // proceed; abandon the wrapper and let the verifier
            // flag the missing terminator.
            return;
        }

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(closure);
        for (auto& arg : llvmFunction->args()) {
            callArgs.push_back(&arg);
        }

        // A4-style @Before / @After emit on the wrapper's IR.
        // emitBeforeAdvice / emitAfterAdvice call through
        // module->getBuilder() — swap the module-level builder to
        // our wrapper-local one for the duration.
        auto* prevBuilder = builder;
        llvm::IRBuilder<>* wb = &wrapBuilder;
        builder = wb;
        module->setBuilder(wb);
        emitBeforeAdvice(module);

        llvm::CallInst* result = wb->CreateCall(
            aroundAdvice->getLlvmFunctionType(),
            aroundAdvice->getLlvmFunction(),
            callArgs);

        emitAfterAdvice(module);

        builder = prevBuilder;
        module->setBuilder(prevBuilder);

        llvm::Type* retTy = llvmFunctionType->getReturnType();
        if (retTy->isVoidTy()) {
            wb->CreateRetVoid();
        } else {
            wb->CreateRet(result);
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