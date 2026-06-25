//
// Created by James Klappenbach on 2/19/22.
//

#include "Method.h"
#include <functional>
#include <unordered_set>
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaFunctionType.h"
#include "../compile/CajetaModule.h"
#include "../compile/Compiler.h"
#include "../error/VariableAssignmentException.h"
#include "../error/Exception.h"
#include "../asn/DefaultBlock.h"
#include "../asn/Statement.h"
#include "../asn/expression/Expression.h"
#include "../asn/expression/MethodCallExpression.h"
#include "../asn/expression/NewExpression.h"
#include "../asn/expression/AggregateInitializerExpression.h"
#include "../asn/expression/BinaryOpExpression.h"
#include "../asn/expression/Identifier.h"
#include "../asn/LocalVariableDeclaration.h"
#include "../type/FormalParameter.h"
#include "../field/ParameterField.h"
#include "../field/BoundClosureField.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "../util/Printer.h"
#include "../xpu/core/KernelArgTrait.h"
#include "../xpu/core/XpuAttributes.h"

using namespace std;

#define ERROR_CAUSE_ASSIGNMENT_FINAL        "the field is declared final"
#define ERROR_CAUSE_VARIABLE_NOT_FOUND      "the field was not declared"
#define ERROR_CAUSE_VARIABLE_DUPLICATE      "the field name already exists"
#define ERROR_ID_ASSIGNMENT_FINAL           "CAJETA_ERROR_FINAL_ASSIGNMENT"
#define ERROR_ID_VARIABLE_NOT_FOUND         "CAJETA_ERROR_VARIABLE_NOT_FOUND"
#define ERROR_ID_VARIABLE_DUPLICATE         "CAJETA_ERROR_VARIABLE_DUPLICATE"

namespace cajeta {
    map<string, MethodPtr> Method::archive;

    // SIMD plan Phase 0.6: a method needs the implicit structured-concurrency
    // scope frame (save_top + scope_enter[HEAP-ALLOC] + exit_to) only when its
    // body creates/joins a child task — i.e. lexically contains a `spawn`,
    // `detach`, or `scope { }`. A spawn-free body owns no children and elides the
    // frame; that frame was the per-stripe overhead (vzeroupper + 3 runtime
    // calls, scope_enter mallocs) that made the xxhash3 SIMD loop slower than
    // native.
    //
    // We walk the body AST. Statements hold their sub-nodes in typed fields (NOT
    // `children`), so each container recurses its fields explicitly; expressions
    // and Block statements (incl. a spawn in a LocalVariableDeclaration / an
    // assignment ExpressionStatement) are reached via getChildren(). A nested
    // LambdaExpression is NOT descended — it is its own method with its own frame
    // decision. The few containers without sub-node accessors (try/switch/
    // synchronized/throw/yield) are treated CONSERVATIVELY as possibly hosting a
    // spawn: that only ever over-keeps the frame (always safe), and they are
    // absent from the value-type / SIMD hot loops this optimizes. Correctness is
    // pinned by ScopeFrameElisionTests (spawn nested in every common container)
    // and the structured-concurrency suite (Spawn*/Async*).
    static bool astHasSpawnSite(const AbstractSyntaxNodePtr& node);

    static bool anyHasSpawnSite(const vector<AbstractSyntaxNodePtr>& v) {
        for (auto& c : v) if (astHasSpawnSite(c)) return true;
        return false;
    }

    static bool astHasSpawnSite(const AbstractSyntaxNodePtr& node) {
        if (!node) return false;
        if (dynamic_pointer_cast<LambdaExpression>(node)) return false; // own method
        if (dynamic_pointer_cast<SpawnExpression>(node)) return true;
        if (dynamic_pointer_cast<DetachExpression>(node)) return true;
        if (dynamic_pointer_cast<ScopeStatement>(node)) return true;
        // Conservative: containers without sub-node accessors may host a spawn.
        if (dynamic_pointer_cast<TryStatement>(node)) return true;
        if (dynamic_pointer_cast<SwitchStatement>(node)) return true;
        if (dynamic_pointer_cast<SynchronizedStatement>(node)) return true;
        if (dynamic_pointer_cast<ThrowStatement>(node)) return true;
        if (dynamic_pointer_cast<YieldStatement>(node)) return true;
        // Expressions / Block statements / LVD initializers live in children.
        if (anyHasSpawnSite(node->getChildren())) return true;
        // Control-flow containers: recurse their typed sub-node fields.
        if (auto s = dynamic_pointer_cast<IfStatement>(node))
            return astHasSpawnSite(s->getCondition()) || astHasSpawnSite(s->getThenBranch())
                || astHasSpawnSite(s->getElseBranch());
        if (auto s = dynamic_pointer_cast<WhileStatement>(node))
            return astHasSpawnSite(s->getCondition()) || astHasSpawnSite(s->getBody());
        if (auto s = dynamic_pointer_cast<DoStatement>(node))
            return astHasSpawnSite(s->getBody()) || astHasSpawnSite(s->getCondition());
        if (auto s = dynamic_pointer_cast<ForStatement>(node)) {
            if (astHasSpawnSite(s->getInit()) || astHasSpawnSite(s->getCondition())
                    || astHasSpawnSite(s->getBody())) return true;
            for (auto& u : s->getUpdate()) if (astHasSpawnSite(u)) return true;
            return false;
        }
        if (auto s = dynamic_pointer_cast<EnhancedForStatement>(node))
            return astHasSpawnSite(s->getIterableExpr()) || astHasSpawnSite(s->getBody());
        if (auto s = dynamic_pointer_cast<ReturnStatement>(node))
            return astHasSpawnSite(s->getExpression());
        if (auto s = dynamic_pointer_cast<ExpressionStatement>(node))
            return astHasSpawnSite(s->getExpression());
        if (auto s = dynamic_pointer_cast<LabelStatement>(node))
            return astHasSpawnSite(s->getBlock());
        return false;
    }

    // True iff this method must emit the per-method scope frame.
    bool Method::bodyNeedsScopeFrame() {
        return astHasSpawnSite(block);
    }

    Method::Method(CajetaModulePtr module,
        string& name,
        CajetaTypePtr returnType,
        vector<FormalParameterPtr> parameterList,
        BlockPtr block,
        CajetaClassPtr parent) {
        this->module = module;
        this->parent = parent;
        // Inherit the parent class's emit target (test-reuse): a method of a
        // stdlib-template instantiation owned (emit-wise) by a user module must
        // emit there too. Adopted at construction so IR emitted DURING the
        // template body walk (prototype-on-reference, before any reparent point)
        // already targets the user module. No-op in production (emit==resolution).
        if (parent && parent->getEmitModule() != parent->getModule()) {
            this->emitModule = parent->getEmitModule();
        }
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
        // Inherit the parent class's emit target (test-reuse) — see the other
        // constructor. No-op in production (emit==resolution).
        if (parent && parent->getEmitModule() != parent->getModule()) {
            this->emitModule = parent->getEmitModule();
        }
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

    // --- Value-return determination (M1) -----------------------------------
    // A return expression is a `stack` construction when it's a `stack X(...)`
    // (NewExpression with stackAlloc) or a stack aggregate-initializer. Storage
    // class lives on the construction, not the type — so this is how the
    // compiler learns a method returns by copy.
    bool Method::exprIsStackConstruction(const ExpressionPtr& e) {
        if (!e) return false;
        if (auto ne = dynamic_pointer_cast<NewExpression>(e)) {
            return ne->getStackAlloc();
        }
        if (auto ai = dynamic_pointer_cast<AggregateInitializerExpression>(e)) {
            return ai->getStackAlloc();
        }
        return false;
    }

    bool Method::blockHasStackReturn(const BlockPtr& block) {
        if (!block) return false;
        for (auto& child : block->getChildren()) {
            if (nodeHasStackReturn(child)) return true;
        }
        return false;
    }

    // Walk statement containers looking for any `return stack X(...)`. The
    // branch/loop bodies hidden behind private fields (then/else, loop body,
    // nested scope/label blocks) aren't in `children`, so descend through their
    // accessors explicitly; everything else falls back to the generic child
    // walk. (try/switch bodies have no public accessors today — returns nested
    // directly inside a try/switch aren't detected; not needed for v1.)
    bool Method::nodeHasStackReturn(const AbstractSyntaxNodePtr& node) {
        if (!node) return false;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            return exprIsStackConstruction(ret->getExpression());
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return blockHasStackReturn(lbl->getBlock());
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return blockHasStackReturn(sc->getBlock());
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return nodeHasStackReturn(iff->getThenBranch())
                || nodeHasStackReturn(iff->getElseBranch());
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return nodeHasStackReturn(wh->getBody());
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return nodeHasStackReturn(fr->getBody());
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return nodeHasStackReturn(efr->getBody());
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return nodeHasStackReturn(dod->getBody());
        }
        for (auto& c : node->getChildren()) {
            if (nodeHasStackReturn(c)) return true;
        }
        return false;
    }

    // --- Lint helpers for [heap-optional-return] --------------------------
    // A return expression matches the "scope-bounded heap C(...)" pattern
    // when it's a `heap C(...)` (NewExpression with !stackAlloc) whose
    // constructed type's canonical class name matches the method's declared
    // return-type class (modulo `#`). The lint targets `#Optional<T>`
    // returns whose body's every return is `return heap Optional<...>(...)`
    // — the heap allocation immediately becomes the return value and the
    // caller's first observable use, so it could safely be a stack
    // construction returned by value (sret) instead.
    static std::string trimTemplateArgs(const std::string& canonical) {
        auto lt = canonical.find('<');
        if (lt == std::string::npos) return canonical;
        return canonical.substr(0, lt);
    }
    static bool exprIsHeapCtorOfClass(const ExpressionPtr& e,
            const std::string& targetClassCanonical) {
        if (!e) return false;
        auto ne = dynamic_pointer_cast<NewExpression>(e);
        if (!ne) return false;
        if (ne->getStackAlloc()) return false;
        auto rt = ne->getResolvedType();
        if (!rt || !rt->getQName()) return false;
        std::string ctorClass = trimTemplateArgs(
            rt->getQName()->toCanonical());
        return ctorClass == targetClassCanonical;
    }
    // Walk the method body's nested statement containers and visit every
    // return statement, calling `visit(retExpr)`. Returns false if any
    // visit returns false. Mirrors blockHasStackReturn's structural walk.
    static bool methodVisitReturns(const AbstractSyntaxNodePtr& node,
            const std::function<bool(const ExpressionPtr&)>& visit);
    static bool methodVisitReturnsBlock(const BlockPtr& block,
            const std::function<bool(const ExpressionPtr&)>& visit) {
        if (!block) return true;
        for (auto& c : block->getChildren()) {
            if (!methodVisitReturns(c, visit)) return false;
        }
        return true;
    }
    static bool methodVisitReturns(const AbstractSyntaxNodePtr& node,
            const std::function<bool(const ExpressionPtr&)>& visit) {
        if (!node) return true;
        if (auto ret = dynamic_pointer_cast<ReturnStatement>(node)) {
            return visit(ret->getExpression());
        }
        if (auto lbl = dynamic_pointer_cast<LabelStatement>(node)) {
            return methodVisitReturnsBlock(lbl->getBlock(), visit);
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            return methodVisitReturnsBlock(sc->getBlock(), visit);
        }
        if (auto iff = dynamic_pointer_cast<IfStatement>(node)) {
            return methodVisitReturns(iff->getThenBranch(), visit)
                && methodVisitReturns(iff->getElseBranch(), visit);
        }
        if (auto wh = dynamic_pointer_cast<WhileStatement>(node)) {
            return methodVisitReturns(wh->getBody(), visit);
        }
        if (auto fr = dynamic_pointer_cast<ForStatement>(node)) {
            return methodVisitReturns(fr->getBody(), visit);
        }
        if (auto efr = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return methodVisitReturns(efr->getBody(), visit);
        }
        if (auto dod = dynamic_pointer_cast<DoStatement>(node)) {
            return methodVisitReturns(dod->getBody(), visit);
        }
        for (auto& c : node->getChildren()) {
            if (!methodVisitReturns(c, visit)) return false;
        }
        return true;
    }

    void Method::lintHeapOptionalReturn() {
        // Gate: declared return must be `#Optional<...>` (heap ownership
        // transfer of an Optional). `@HeapReturn` is the escape hatch when
        // a caller really wants the heap allocation (the annotation may
        // not exist yet; the lookup is a no-op until it does).
        if (!returnsOwnership) return;
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass || !rtClass->getQName()) return;
        std::string rtCanonical = trimTemplateArgs(
            rtClass->getQName()->toCanonical());
        if (rtCanonical != "cajeta.lang.Optional") return;
        if (findAnnotation("HeapReturn") != nullptr) return;
        if (!block) return;
        // Per-method dedupe: with method-level templates the same Method
        // can be instantiated many times. Key on parent canonical +
        // method's labeled canonical so each declaration warns once.
        static std::unordered_set<std::string> warned;
        // Dedupe by source-canonical (template args trimmed): the same
        // source-level method warning shouldn't repeat for each
        // template instantiation (Stream<int32>.next, Stream<int64>.next
        // all share one declaration site).
        std::string parentCanonical = parent && parent->getQName()
            ? trimTemplateArgs(parent->getQName()->toCanonical())
            : std::string("");
        std::string key = parentCanonical + "::" + name;
        if (warned.count(key)) return;
        int returnCount = 0;
        bool allHeapOptional = methodVisitReturnsBlock(block,
            [&](const ExpressionPtr& e) -> bool {
                returnCount++;
                return exprIsHeapCtorOfClass(e, "cajeta.lang.Optional");
            });
        if (!allHeapOptional || returnCount == 0) return;
        warned.insert(key);
        std::cerr << "warning: [heap-optional-return] "
            << (parentCanonical.empty()
                ? std::string("") : parentCanonical + ".")
            << name
            << " returns #Optional<...> but every return is a "
            << "scope-bounded `heap Optional<...>(...)`. Consider "
            << "dropping `#` from the return type and switching to "
            << "`return stack Optional<...>(...)` — the value lands "
            << "in the caller's slot by copy (sret), avoiding a heap "
            << "allocation per call. Suppress with @HeapReturn when "
            << "the caller really needs heap ownership."
            << std::endl;
    }

    bool Method::returnsStackValue() {
        if (returnsStackValueCache != -1) {
            return returnsStackValueCache == 1;
        }
        returnsStackValueCache = 0;
        // A `#`-marked return is an explicit heap ownership transfer, never a
        // by-value copy — they're mutually exclusive.
        if (returnsOwnership) return false;
        // A @ValueType (Vec2, Matrix, ...) is a small POD aggregate returned
        // *by value* in registers (the isValueTypeR path in generatePrototype),
        // never via the sret hidden-pointer ABI. Exclude it here so every sret
        // call site (prototype, call emission, arg indexing) agrees that value
        // types are not sret. See value-type-overloading-plan.md.
        if (returnType && returnType->isValueType()) return false;
        // Only class (value) returns travel by copy. void/primitive returns
        // never do; interfaces already have their own by-value fat-pointer ABI
        // (Method::generatePrototype S9.5.5); arrays are reference-typed.
        auto rtClass = dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass) return false;
        if (rtClass->isInterface()) return false;
        if (dynamic_pointer_cast<CajetaArray>(returnType)) return false;
        if (blockHasStackReturn(block)) {
            returnsStackValueCache = 1;
            return true;
        }
        // #63: an interface method (abstract, no body) returning a value-shape
        // class (Optional<T>, etc.) has no body to scan, but the dispatch-site
        // ABI must match the impl's. Impls of such methods use `return stack
        // X(...)` (or `return o` for a local value-shape, forced sret via #66
        // off this very decl) and compile with sret; the interface decl's
        // prototype must too, or indirect calls via the vtable misalign args.
        // Force sret on the interface decl based on the signature alone.
        //
        // EXCEPTION — a REFERENCE return like cajeta.lang.String is returned by
        // its impls as a heap POINTER, never sret; forcing sret here mismatches
        // the impl ABI and crashes on dispatch (the ifx Backend.name() case:
        // NullWindowBackend::name returns a String pointer while the sret-forced
        // decl expected `void name(sret, this)`). No prior code dispatched a
        // String-returning interface method, so excluding it cannot regress the
        // value-shape (Optional) path the force exists for.
        if (parent && parent->isInterface()) {
            QualifiedNamePtr rtName = rtClass->getQName();
            bool isReferenceReturn = rtName
                && rtName->getTypeName() == "String"
                && rtName->getPackageName() == "cajeta.lang";
            if (!isReferenceReturn) {
                returnsStackValueCache = 1;
                return true;
            }
            // String reference return: fall through to a pointer return.
        }
        // #66: a class method whose body has no `return stack X(...)` (the
        // canonical PeekStream/SkipStream shape — body is just `return o;`
        // for a local Optional<T>) still needs sret if an ancestor class's
        // same-name method is sret. The vtable slot ABI was fixed by the
        // base; overrides must match or virtual dispatch misaligns args.
        // Walk the superclass chain for any method with the same name +
        // arg count whose own returnsStackValue() is true. Map key is the
        // full canonical signature, not the bare name — iterate and match
        // by name field + parameter count (the override discriminator the
        // vtable slot resolution uses).
        if (parent && !parent->isInterface()) {
            size_t myParamCount = parameterList.size();
            for (auto& ancestor : parent->getSuperClasses()) {
                CajetaClassPtr cls = ancestor;
                while (cls) {
                    for (auto& kv : cls->getMethods()) {
                        MethodPtr ancMethod = kv.second;
                        if (!ancMethod || ancMethod.get() == this) continue;
                        if (ancMethod->getName() != name) continue;
                        if (ancMethod->getParameterList().size() != myParamCount) continue;
                        if (ancMethod->returnsStackValue()) {
                            returnsStackValueCache = 1;
                            return true;
                        }
                    }
                    if (cls->getSuperClasses().empty()) break;
                    cls = cls->getSuperClasses().front();
                }
            }
        }
        return false;
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
    void Method::recordNativeRequirement(const std::string& lib,
                                         const std::string& symbol) {
        llvm::Module* lmod = getEmitModule()->getLlvmModule();
        llvm::LLVMContext& ctx = lmod->getContext();
        llvm::NamedMDNode* nmd =
            lmod->getOrInsertNamedMetadata("cajeta.native.reqs");
        // Dedup: skip an identical {lib, symbol} entry.
        for (unsigned i = 0; i < nmd->getNumOperands(); ++i) {
            llvm::MDNode* op = nmd->getOperand(i);
            if (op->getNumOperands() != 2) continue;
            auto* l = llvm::dyn_cast<llvm::MDString>(op->getOperand(0));
            auto* s = llvm::dyn_cast<llvm::MDString>(op->getOperand(1));
            if (l && s && l->getString() == lib && s->getString() == symbol)
                return;
        }
        llvm::Metadata* entry[] = {
            llvm::MDString::get(ctx, lib),
            llvm::MDString::get(ctx, symbol),
        };
        nmd->addOperand(llvm::MDNode::get(ctx, entry));
    }

    void Method::emitNativeForwardingBody(const std::string& symbol) {
        // The forwarding wrapper IS this method's llvmFunction (in the emit
        // module); its runtime-symbol extern must be co-resident there.
        llvm::Module* lmod = getEmitModule()->getLlvmModule();
        llvm::LLVMContext& ctx = *module->getLlvmContext();

        // Runtime-ABI coercion for 128-bit floats: a fp128 @Native parameter is
        // forwarded BY POINTER — the wrapper stores the fp128 to a stack slot
        // and passes its address; the C runtime reads 16 bytes from it (e.g.
        // Float128.hashBits -> __cajeta_hash_float128(const void*)). Earlier
        // this passed the bits as i128 by value (bitcast fp128 -> i128), which
        // matched the C `__uint128_t` param on x86-64/AArch64 — but NOT on
        // Win64 (mingw), where clang lowers a 128-bit integer param INDIRECTLY
        // (`i64(ptr dead_on_return)`). The i128-by-value call then mismatched
        // the embedded definition and JIT-verify rejected the whole module on
        // Windows ("Call parameter type does not match function signature"),
        // failing every test. Passing by pointer is `i64(ptr)` uniformly. (We
        // never spell the C float128 type — it doesn't exist on aarch64.)
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto coerceTy = [&](llvm::Type* t) -> llvm::Type* {
            return t->isFP128Ty() ? ptrTy : t;
        };

        // Build the runtime symbol's type from the wrapper's, mapping fp128
        // params to i128 so the extern matches the C definition exactly.
        std::vector<llvm::Type*> targetParams;
        targetParams.reserve(llvmFunctionType->getNumParams());
        for (llvm::Type* pt : llvmFunctionType->params()) {
            targetParams.push_back(coerceTy(pt));
        }
        llvm::FunctionType* targetFnType = llvm::FunctionType::get(
            llvmFunctionType->getReturnType(), targetParams,
            llvmFunctionType->isVarArg());

        // Insert (or reuse) the extern declaration of the runtime symbol.
        llvm::Function* targetFn = lmod->getFunction(symbol);
        if (!targetFn) {
            targetFn = llvm::Function::Create(
                targetFnType,
                llvm::Function::ExternalLinkage,
                symbol,
                lmod);
        }

        llvmBasicBlock = llvm::BasicBlock::Create(
            ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        std::vector<llvm::Value*> args;
        args.reserve(llvmFunction->arg_size());
        // @Native ABI convention: an `int8[]` (CajetaArray) argument is passed
        // as its HEADER pointer ({ i64 count, [N x i8] data }); the C bridge
        // skips the 8-byte length header itself (`hdr + 8`), as e.g.
        // __cajeta_sha256_update does. So the forwarder passes args verbatim,
        // except fp128 -> spill-to-stack-slot + pass pointer per the runtime-ABI
        // note above.
        for (auto& arg : llvmFunction->args()) {
            if (arg.getType()->isFP128Ty()) {
                // Spill the fp128 to a stack slot and pass its address. The
                // store is a plain 16-byte move (no soft-float helper).
                llvm::Value* slot = b.CreateAlloca(arg.getType());
                b.CreateStore(&arg, slot);
                args.push_back(slot);
            } else {
                args.push_back(&arg);
            }
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

    // @NonNull parameter check emission. For every @NonNull-annotated
    // parameter that has a pointer-storage type (class ref, array,
    // String, interface fat-pointer-treated-as-ptr — anything where
    // null is representable), emit an entry-point null-check that
    // throws CAJETA_ERROR_NULL_PARAM_ARG (integer code 2) on miss.
    //
    // Pattern mirrors what `throw 2` would lower to via
    // ThrowStatement::generateCode: __cajeta_throw(IntToPtr(2)). The
    // catch site recovers the int code via (int32) e per the Optional
    // unwrap pattern (Q11). Future structured exception types will swap
    // the code for a real NullPointerException instance.
    //
    // Skipped for:
    //   - non-reference parameter types (int / float / boolean cannot
    //     be null)
    //   - the implicit `this` parameter at position 0 (always a valid
    //     this-pointer at method entry; null `this` is a dispatch-side
    //     bug, not an arg-passing bug)
    void Method::emitNonNullParamChecks(CajetaModulePtr module) {
        if (parameterList.empty()) return;

        llvm::IRBuilder<>* b = module->getBuilder();
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!b || !throwFn) return;

        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        for (size_t i = 0; i < parameterList.size(); ++i) {
            auto& p = parameterList[i];
            if (!p) continue;
            if (p->getName() == "this") continue;
            // FormalParameter::fromContext uses the legacy annotations
            // set path (Annotatable::addAnnotation), NOT the args-aware
            // addAnnotationInstance, so findAnnotation (which walks
            // annotationInstances) misses parameter annotations. Walk
            // the legacy list directly. Future cleanup: extend
            // findAnnotation to also check annotationList, or migrate
            // parameter parsing to addAnnotationInstance.
            bool nn = false;
            for (auto& qn : p->getAnnotations()) {
                if (qn && qn->getTypeName() == "NonNull") { nn = true; break; }
            }
            if (!nn) continue;

            // Use the LLVM arg's actual type for the pointer check —
            // safer than going through CajetaType::getLlvmType() which
            // can return placeholder shapes for forward-declared / not-
            // yet-built types and trip "Invalid size request" / opaque-
            // pointer surprises. Only pointer-typed args can be null.
            if (i >= llvmFunction->arg_size()) continue;
            llvm::Value* argVal = llvmFunction->getArg((unsigned) i);
            if (!argVal->getType()->isPointerTy()) continue;

            llvm::Value* isNull = b->CreateICmpEQ(
                argVal,
                llvm::ConstantPointerNull::get(ptrTy),
                std::string("nn.isnull.") + p->getName());

            llvm::Function* curFn = b->GetInsertBlock()->getParent();
            llvm::BasicBlock* throwBB = llvm::BasicBlock::Create(ctx,
                std::string("nn.throw.") + p->getName(), curFn);
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx,
                std::string("nn.ok.") + p->getName(), curFn);
            b->CreateCondBr(isNull, throwBB, okBB);

            b->SetInsertPoint(throwBB);
            // Encode CAJETA_ERROR_NULL_PARAM_ARG = 2 as an IntToPtr.
            // Matches ThrowStatement::generateCode's integer-throw
            // shape (Statement.cpp:1582-1585).
            llvm::Value* code = llvm::ConstantInt::get(i64Ty,
                llvm::APInt(64, 2, false));
            llvm::Value* ptrCode = b->CreateIntToPtr(code, ptrTy);
            b->CreateCall(throwFn, {ptrCode});
            b->CreateUnreachable();

            b->SetInsertPoint(okBB);
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
        // Method-level template declarations don't get an LLVM prototype.
        // Their formals/return types include placeholder T-vars and the
        // actual function signature isn't known until a concrete
        // instantiation pins each T to a real type. See
        // docs/specification/lang/MethodLevelTemplate.md.
        if (isMethodTemplate()) return;
        // Emit-target swap (test-reuse): for a stdlib-template instantiation
        // owned (emit-wise) by a user module, point `module` at the emit module
        // for the duration of prototype emission so the Function — and every
        // runtime-extern decl it needs (getRuntimeFunction creates them in
        // `module`'s llvm module) — land in the user module, not the cached
        // stdlib. Resolution (canonicalMap/type refresh) is by global canonical
        // name, not module imports, so it is unaffected. No-op in production.
        CajetaModulePtr* moduleSlot = &module;
        CajetaModulePtr savedModule = module;
        if (emitModule && emitModule != module) module = emitModule;
        struct RestoreModule {
            CajetaModulePtr* slot; CajetaModulePtr saved;
            ~RestoreModule() { *slot = saved; }
        } restoreModule{moduleSlot, savedModule};
        // S8.4 — refresh returnType and parameter types from canonicalMap.
        //
        // At parse time, a method declared inside `struct Foo` whose
        // return type or parameter type references Foo gets the type
        // resolved to a placeholder CajetaClass (per CajetaType::fromContext's
        // forward-reference handling): the real CajetaClass hasn't been
        // registered yet because we're still inside the struct/view
        // builder's visit-children pass when the method signature is parsed.
        // When the class's generatePrototype runs later and overwrites
        // canonicalMap[canonical] with the real CajetaClass (a fresh
        // shared_ptr — the struct/view builder doesn't reuse the placeholder
        // the way visitClassDeclaration does via fillFromDeclaration), the
        // method still holds the stale placeholder. Downstream dispatch
        // picks the wrong return convention, ReturnStatement emits a
        // mismatched value, and LLVM verify rejects it. Refresh both
        // sides here, just-in-time.
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

        // @Native methods can be declared with `;` body (no explicit
        // `{ }`), which leaves abstractFlag true. They still need a
        // full LLVM Function — the @Native forwarding body emission
        // expects one. Demote the abstract flag here so the rest of
        // generatePrototype builds the function normally and
        // generateCode emits the forwarder.
        bool isNative = findAnnotation("Native") != nullptr;
        if (abstractFlag && isNative) {
            abstractFlag = false;
        }

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
            // Apply the same class-by-pointer / array-by-pointer rule
            // the non-abstract path uses below (line ~570). Without
            // this, a templated-interface method like `int32 encode(T v)`
            // where T → user-class lowers its T-arg to the inline
            // struct type; the concrete implementer's signature has
            // `ptr`, and the JIT verifier rejects the indirect call
            // through the iface vtable ("Call parameter type does not
            // match function signature").
            for (auto formalParameter: parameterList) {
                CajetaTypePtr pt = formalParameter->getType();
                llvm::Type* ptLlvm = pt->getLlvmType();
                bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
                bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
                bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
                // @ValueType params pass by value (aggregate) — Copy semantics,
                // matching the non-abstract path below.
                bool passByPointer = isClassLike && (isArr || !isPrim)
                    && !pt->isValueType();
                if (passByPointer) {
                    ptLlvm = llvm::PointerType::get(*module->getLlvmContext(), 0);
                }
                llvmTypes.push_back(ptLlvm);
            }
            // Mirror the non-abstract path's return-by-pointer rule so
            // the indirect call type through the iface vtable matches
            // the implementer's signature. Without this, a method like
            // `#Stream<T> trySplit()` on an interface lowers its return
            // to the Stream<T> body struct, while the concrete
            // implementer (ArrayStream<T>) emits `ret ptr`. The
            // mismatched call signature stores the struct return into
            // a ptr-sized alloca and overflows the stack.
            // #63: also mirror the sret path. An interface method
            // returning a value-shape class (e.g. `Optional<T> next()`)
            // must use the sret ABI on the interface prototype, because
            // every well-formed impl uses `return stack X(...)` and
            // compiles with sret — the indirect call via the vtable
            // would otherwise misalign args (sret slot becomes `this`).
            llvm::Type* llvmRetAbs;
            llvm::Type* sretStructTyAbs = nullptr;
            {
                CajetaTypePtr rt = returnType;
                bool isArrR = rt
                    && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
                auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
                bool isClassLikeR = rtClass != nullptr;
                bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
                bool isInterfaceR = rtClass && rtClass->isInterface();
                // @ValueType returns by value (the aggregate itself), like a POD
                // primitive — not by pointer and not via sret (returnsStackValue
                // is forced false for value types). See value-type-overloading-plan.md.
                bool isValueTypeR = rt && rt->isValueType();
                bool sretReturnAbs = returnsStackValue();
                bool returnByPointer = isClassLikeR
                    && (isArrR || !isPrimR) && !isInterfaceR && !isValueTypeR;
                if (sretReturnAbs) {
                    sretStructTyAbs = rt ? rt->getLlvmType() : nullptr;
                    llvmRetAbs = llvm::Type::getVoidTy(
                        *module->getLlvmContext());
                    llvmTypes.insert(llvmTypes.begin(),
                        llvm::PointerType::get(
                            *module->getLlvmContext(), 0));
                } else if (returnByPointer) {
                    llvmRetAbs = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                } else {
                    llvmRetAbs = rt ? rt->getLlvmType() : nullptr;
                }
            }
            llvmFunctionType = llvmTypes.empty()
                ? llvm::FunctionType::get(llvmRetAbs, false)
                : llvm::FunctionType::get(llvmRetAbs, llvmTypes, false);
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
        // "Returns a reference" classifier — the original implementation
        // tested `getLlvmType()->isPointerTy()`, which worked when
        // `String` was a `char*` typedef and class refs were already
        // ptr-typed. Post Phase 2b-β class String's LLVM type is the
        // body struct (NOT a pointer), so `isPointerTy()` returns
        // false and the check stopped firing for the canonical
        // example (`public static String pick(String a, String b)`).
        // Detect by the high-level CajetaType shape instead: a class
        // ref (excluding views — they're inline aggregates passed by
        // value), an array, or an interface — all the reference-
        // semantics types. Primitives and value-typed structs fall
        // through harmlessly.
        bool returnIsReferenceTyped = false;
        if (returnType) {
            if (auto rc = std::dynamic_pointer_cast<CajetaClass>(returnType)) {
                // Views and @ValueType classes are inline aggregates returned BY
                // VALUE (Copy semantics) — not references — so a static operator
                // like `Vec2 operator+(Vec2, Vec2)` returns an owned value, not a
                // borrow, and is exempt from the multi-param-borrow rule. See
                // plans/value-type-overloading-plan.md (value types are Copy).
                if (!std::dynamic_pointer_cast<CajetaView>(returnType)
                        && !returnType->isValueType()) {
                    returnIsReferenceTyped = true;
                }
            } else if (returnType->getLlvmType()
                    && returnType->getLlvmType()->isPointerTy()) {
                // Legacy `pointer`-typed return (the bare-pointer
                // bootstrap shape).
                returnIsReferenceTyped = true;
            }
        }
        // sret value-returning methods aren't borrow-returning — the result
        // is constructed directly into the caller's slot by copy (M3
        // NRVO), so there's no parameter lifetime to inherit. Exempt them
        // from the multi-parameter borrow-return check (otherwise e.g.
        // `Tasks.withTimeout(Duration, Task<R>) -> Optional<R>` is
        // wrongly rejected even though every return is `return stack
        // Optional<R>(...)`).
        if (staticMethod && !returnsOwnership && !returnsStackValue()
                && returnIsReferenceTyped
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
            // (per docs/specification/lang/Views.md): a struct IS a typed view over a
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
            // class-like type — and CajetaArray and plain CajetaClass
            // are CajetaClass subclasses under the unified-class model.
            bool isArr = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(pt) != nullptr;
            bool isPrim = pt && (pt->getTypeFlags() & PRIMITIVE_FLAG);
            // @ValueType params pass BY VALUE (the aggregate), like a POD
            // primitive — they are Copy, so the callee gets its own copy and
            // there is no aliasing to preserve. The call site loads the inline
            // slot and the signature carries getLlvmType(). See
            // plans/value-type-overloading-plan.md.
            bool passByPointer = isClassLike && (isArr || !isPrim)
                && !pt->isValueType();
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
        // S9.5.5 — interface returns get a by-value treatment: a 24-byte
        // fat-pointer body returned by value via the small-struct ABI
        // (typically sret for 24 bytes on x86-64 SysV), with the caller
        // repackaging into a fresh body alloca. The body might point at
        // a class instance that outlives the call, but the body
        // STRUCTURE itself (the three-word tuple) is callee-local stack
        // for any synthesized interface value, so by-value return is
        // the only safe shape.
        // Way 2 value-return ABI (sret + NRVO): a method that returns a
        // `stack`-constructed value hands it back by copy. The LLVM function
        // returns `void` and takes a hidden leading `ptr` (param 0, before
        // `this`) carrying the `sret(structTy)` attribute; the callee
        // constructs its result directly into that caller-owned slot. See
        // docs/specification/lang/ValueReturns.md.
        bool sretReturn = returnsStackValue();
        llvm::Type* sretStructTy = nullptr;
        llvm::Type* llvmRet;
        {
            CajetaTypePtr rt = returnType;
            bool isArrR = rt
                && dynamic_pointer_cast<CajetaArray>(rt) != nullptr;
            auto rtClass = dynamic_pointer_cast<CajetaClass>(rt);
            bool isClassLikeR = rtClass != nullptr;
            bool isPrimR = rt && (rt->getTypeFlags() & PRIMITIVE_FLAG);
            bool isInterfaceR = rtClass && rtClass->isInterface();
            // @ValueType returns by value (the aggregate), like a POD primitive —
            // not by pointer. See plans/value-type-overloading-plan.md.
            bool isValueTypeR = rt && rt->isValueType();
            bool returnByPointer = isClassLikeR && (isArrR || !isPrimR)
                && !isInterfaceR && !isValueTypeR;
            if (sretReturn) {
                sretStructTy = rt ? rt->getLlvmType() : nullptr;
                llvmRet = llvm::Type::getVoidTy(*module->getLlvmContext());
                llvmTypes.insert(llvmTypes.begin(),
                    llvm::PointerType::get(*module->getLlvmContext(), 0));
            } else if (returnByPointer) {
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

        // Two-layer naming: instantiations of a same-canonical template
        // get distinct LLVM symbols via getLlvmSymbolName() (= canonical
        // + method-arg suffix). Ordinary methods get plain canonical.
        // See docs/specification/lang/MethodLevelTemplate.md § two-layer naming.
        string canonical = getLlvmSymbolName();
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

        // S4 — force-inline @ValueType operators. The AlwaysInlinerPass runs
        // even at O0 (Optimizer.cpp), so a dispatched value-type operator folds
        // to the same flat, register-resident IR a compiler intrinsic would
        // emit — on host AND device — instead of a real call passing the
        // aggregate by value. Gate: the declaring class is @ValueType and the
        // method is an operator overload (`operator+`, `operator==`, …).
        // (Size guard for an oversized operator body is a future refinement —
        // S4; today's value-type operators are small by construction.)
        if (parent && parent->isValueType()
                && name.rfind("operator", 0) == 0) {
            llvmFunction->addFnAttr(llvm::Attribute::AlwaysInline);
        }

        // @Inline / @NoInline — user-facing inlining control, the two halves of
        // the container hot/cold idiom. `@Inline` forces a small hot method to
        // fold into its caller (AlwaysInlinerPass runs even at O0, and crucially
        // forces the inline ACROSS ThinLTO module boundaries — e.g. stdlib
        // `ArrayList.add` into a user append loop, matching Rust `Vec::push` /
        // C++ `vector::push_back`). `@NoInline` keeps a cold slow path
        // (grow/rehash) out-of-line so it never bloats that hot method past the
        // inline threshold; also `cold` so block placement favors the hot path.
        // Mutually exclusive — @Inline wins if both are (mistakenly) present.
        if (findAnnotation("Inline") != nullptr) {
            llvmFunction->addFnAttr(llvm::Attribute::AlwaysInline);
        } else if (findAnnotation("NoInline") != nullptr) {
            llvmFunction->addFnAttr(llvm::Attribute::NoInline);
            llvmFunction->addFnAttr(llvm::Attribute::Cold);
        }

        // Tag the hidden sret pointer (arg 0) so the backend + optimizer treat
        // it as the return slot. Idempotent across reprototype calls.
        if (sretReturn && sretStructTy && llvmFunction->arg_size() > 0) {
            llvmFunction->getArg(0)->addAttr(
                llvm::Attribute::getWithStructRetType(
                    *module->getLlvmContext(), sretStructTy));
        }

        archive[canonical] = shared_from_this();

        module->getLlvmModule()->getOrInsertFunction(canonical, llvmFunctionType);
    }

    // ---- Frame-arena escape analysis (frame-arena-plan U2) ----------------
    namespace {
        bool arenaIsStringType(const CajetaTypePtr& t) {
            if (!t || !t->getQName()) return false;
            return t->getQName()->getTypeName() == "String"
                && t->getQName()->getPackageName() == "cajeta.lang";
        }
        // Leftmost (receiver-side) identifier of a subtree — the name a `#move`
        // transfers (mirrors Expression.cpp::firstIdentifierIn; `#c.next()` parses
        // as `#(c.next())`, so the transferred name is the call's receiver leaf).
        std::string arenaLeftmostId(const AbstractSyntaxNodePtr& node) {
            if (!node) return "";
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node))
                return id->getTextValue();
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                if (!mc->getChildren().empty())
                    return arenaLeftmostId(mc->getChildren()[0]);
                return "";
            }
            for (auto& c : node->getChildren()) {
                auto r = arenaLeftmostId(c);
                if (!r.empty()) return r;
            }
            return "";
        }
        // The concat BinaryOpExpression directly initializing a declarator, if any.
        std::shared_ptr<BinaryOpExpression> arenaConcatInit(const VariableDeclaratorPtr& d) {
            if (!d || !d->getInitializer()) return nullptr;
            auto& kids = d->getInitializer()->getChildren();
            if (kids.empty()) return nullptr;
            auto bo = std::dynamic_pointer_cast<BinaryOpExpression>(kids[0]);
            if (bo && bo->getBinaryOp() == BINARY_OP_ADD) return bo;
            return nullptr;
        }
        // The `heap T[N]` NewExpression directly initializing a declarator, IF it is
        // a single-dimension array of primitive elements (frame-arena-plan U3). The
        // primitive-element gate keeps element drops a no-op, so the arena reset
        // fully reclaims the array; multi-dim and class/String/array-element arrays
        // (whose drops free per-element owned storage) are excluded — they keep the
        // heap path.
        std::shared_ptr<NewExpression> arenaArrayInit(const CajetaTypePtr& declType,
                                                      const VariableDeclaratorPtr& d) {
            // Use the declared local type (always populated post-resolve) rather than
            // the NewExpression's resolvedType, which isn't reliably set this early.
            auto arr = std::dynamic_pointer_cast<CajetaArray>(declType);
            if (!arr) return nullptr;
            if (!d || !d->getInitializer()) return nullptr;
            auto& kids = d->getInitializer()->getChildren();
            if (kids.empty()) return nullptr;
            auto ne = std::dynamic_pointer_cast<NewExpression>(kids[0]);
            if (!ne || ne->getStackAlloc() || ne->getSharedAlloc()) return nullptr;
            // Must be an array creator (`heap T[N]`), single-dimension.
            auto acr = std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest());
            if (!acr || acr->getTotalBracketPairs() != 1) return nullptr;
            auto elem = arr->getElementType();
            // Element must be a plain primitive: not a class/String, array, view, or
            // function type (any of which can carry per-element owned storage).
            if (!elem
                    || std::dynamic_pointer_cast<CajetaClass>(elem)
                    || std::dynamic_pointer_cast<CajetaArray>(elem)
                    || std::dynamic_pointer_cast<CajetaView>(elem)
                    || std::dynamic_pointer_cast<CajetaFunctionType>(elem)) {
                return nullptr;
            }
            return ne;
        }
        // Direct identifier RHS of an expression (alias source) — `x = name` /
        // `T x = name`. A name nested inside a call/operator is a borrow, not a
        // store, so only a bare top-level identifier counts as an escape here.
        std::string arenaDirectIdentifier(const AbstractSyntaxNodePtr& node) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node))
                return id->getTextValue();
            return "";
        }
        // Comprehensive escape walk (holder set mirrors Expression.cpp's
        // collectFreeIdentifiers so `#name` / returns / stores anywhere are seen).
        // Collects names that ESCAPE the frame and concat-decl candidates.
        void arenaWalk(const AbstractSyntaxNodePtr& node,
                       std::set<std::string>& escaping,
                       std::vector<std::pair<std::string,
                           std::shared_ptr<BinaryOpExpression>>>& candidates,
                       std::vector<std::pair<std::string,
                           std::shared_ptr<NewExpression>>>& arrayCandidates) {
            if (!node) return;
            // #name → the transferred name escapes (ownership leaves the frame).
            if (auto mv = std::dynamic_pointer_cast<MoveExpression>(node)) {
                if (!mv->getChildren().empty()) {
                    std::string n = arenaLeftmostId(mv->getChildren()[0]);
                    if (!n.empty()) escaping.insert(n);
                    for (auto& c : mv->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
                }
                return;
            }
            // return name → escapes to caller (return of a concat expr does not:
            // that builds a fresh value, the operands are borrowed).
            if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
                std::string n = arenaDirectIdentifier(ret->getExpression());
                if (!n.empty()) escaping.insert(n);
                arenaWalk(ret->getExpression(), escaping, candidates, arrayCandidates);
                return;
            }
            // x = name / this.f = name / arr[i] = name → name is stored.
            if (auto bo = std::dynamic_pointer_cast<BinaryOpExpression>(node)) {
                if (bo->getBinaryOp() == BINARY_OP_ASSIGN
                        && bo->getChildren().size() >= 2) {
                    std::string n = arenaDirectIdentifier(bo->getChildren()[1]);
                    if (!n.empty()) escaping.insert(n);
                }
                for (auto& c : bo->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
                return;
            }
            // Declarations: collect String-concat candidates; alias init escapes.
            if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                bool isStr = arenaIsStringType(lvd->getType());
                for (auto& d : lvd->getVariableDeclarators()) {
                    if (!d) continue;
                    if (d->getInitializer()) {
                        auto& kids = d->getInitializer()->getChildren();
                        if (!kids.empty()) {
                            std::string alias = arenaDirectIdentifier(kids[0]);
                            if (!alias.empty()) escaping.insert(alias);  // T x = name
                        }
                        arenaWalk(d->getInitializer(), escaping, candidates, arrayCandidates);
                    }
                    if (isStr) {
                        if (auto cbo = arenaConcatInit(d))
                            candidates.emplace_back(d->getIdentifier(), cbo);
                    }
                    // U3: non-escaping single-dim primitive-array locals.
                    if (auto ane = arenaArrayInit(lvd->getType(), d))
                        arrayCandidates.emplace_back(d->getIdentifier(), ane);
                }
                return;
            }
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                for (auto& c : mc->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
                for (auto& p : mc->getParameters()) {
                    // A `#arg` call argument is recorded as callerTransferred on the
                    // parameter (the callee takes ownership) — the named value
                    // escapes the frame. This is the bench's `m.put(#k, i)` shape.
                    if (p.callerTransferred) {
                        std::string n = arenaLeftmostId(p.expression);
                        if (!n.empty()) escaping.insert(n);
                    }
                    arenaWalk(p.expression, escaping, candidates, arrayCandidates);
                }
                return;
            }
            if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
                arenaWalk(es->getExpression(), escaping, candidates, arrayCandidates);
                return;
            }
            if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
                arenaWalk(ifs->getCondition(), escaping, candidates, arrayCandidates);
                arenaWalk(ifs->getThenBranch(), escaping, candidates, arrayCandidates);
                arenaWalk(ifs->getElseBranch(), escaping, candidates, arrayCandidates);
                return;
            }
            if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
                arenaWalk(ls->getBlock(), escaping, candidates, arrayCandidates); return;
            }
            if (auto ss = std::dynamic_pointer_cast<ScopeStatement>(node)) {
                arenaWalk(ss->getBlock(), escaping, candidates, arrayCandidates); return;
            }
            if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
                arenaWalk(ws->getCondition(), escaping, candidates, arrayCandidates);
                arenaWalk(ws->getBody(), escaping, candidates, arrayCandidates); return;
            }
            if (auto dos = std::dynamic_pointer_cast<DoStatement>(node)) {
                arenaWalk(dos->getBody(), escaping, candidates, arrayCandidates);
                arenaWalk(dos->getCondition(), escaping, candidates, arrayCandidates); return;
            }
            if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
                arenaWalk(fs->getInit(), escaping, candidates, arrayCandidates);
                arenaWalk(fs->getCondition(), escaping, candidates, arrayCandidates);
                for (auto& u : fs->getUpdate()) arenaWalk(u, escaping, candidates, arrayCandidates);
                arenaWalk(fs->getBody(), escaping, candidates, arrayCandidates); return;
            }
            if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
                arenaWalk(efs->getIterableExpr(), escaping, candidates, arrayCandidates);
                arenaWalk(efs->getBody(), escaping, candidates, arrayCandidates); return;
            }
            for (auto& c : node->getChildren()) arenaWalk(c, escaping, candidates, arrayCandidates);
        }
    }

    void Method::computeArenaEligibility() {
        arenaEligibleNames.clear();
        methodUsesArena = false;
        if (!block) return;
        std::set<std::string> escaping;
        std::vector<std::pair<std::string, std::shared_ptr<BinaryOpExpression>>> candidates;
        std::vector<std::pair<std::string, std::shared_ptr<NewExpression>>> arrayCandidates;
        arenaWalk(block, escaping, candidates, arrayCandidates);
        for (auto& cand : candidates) {
            // A concat local is arena-eligible iff its name never escapes. A name
            // declared more than once is eligible only if NO declaration escapes
            // (conservative on shadowing).
            if (escaping.find(cand.first) != escaping.end()) continue;
            cand.second->setArenaEligible(true);
            arenaEligibleNames.insert(cand.first);
            methodUsesArena = true;
        }
        // U3: non-escaping single-dim primitive-array locals take the same gate.
        for (auto& cand : arrayCandidates) {
            if (escaping.find(cand.first) != escaping.end()) continue;
            cand.second->setArenaEligible(true);
            arenaEligibleNames.insert(cand.first);
            methodUsesArena = true;
        }
    }

    void Method::generateCode() {
        // Emit-target swap (test-reuse): point `module` at the emit module for
        // the whole body. generateCode passes `module` DOWN to every statement/
        // expression (generateCode(CajetaModulePtr)), so this single swap
        // redirects ALL emission — Function/global creation, intrinsics, malloc,
        // source-file constants, drop functions — into the user emit module,
        // leaving the cached stdlib pristine. Because the same emit-module pointer
        // flows to the whole subtree, the per-module codegen cursor
        // (currentMethod/scopeStack/structureStack/builder) stays self-consistent:
        // statements set AND read state on the emit module. The one hazard is
        // NESTING — an instantiated method emitted on-reference inside another
        // method's body that emits into the SAME user module would clobber the
        // outer method's currentMethod/builder; the save/restore below handles it
        // (an earlier swap attempt WITHOUT that save/restore tripped a spurious
        // heap-return ownership error on async ParallelDriver workers, because the
        // worker and its caller shared the user module's cursor). No-op in
        // production: getEmitModule() == module.
        CajetaModulePtr savedModule = module;
        if (emitModule && emitModule != module) module = emitModule;
        struct RestoreModule {
            Method* self; CajetaModulePtr saved;
            ~RestoreModule() { self->module = saved; }
        } restoreModule{this, savedModule};

        // Incremental compilation (Phase 2/3): mark this method's module as
        // the active codegen frame for the duration of body lowering, so the
        // template-instantiation choke point can attribute any cross-module
        // instantiation it triggers to the right module. RAII restores the
        // previous frame on every exit path (including the early returns
        // below) and nests correctly when codegen of one method drives
        // codegen of another.
        struct CodegenFrame {
            CajetaModulePtr prev;
            explicit CodegenFrame(const CajetaModulePtr& m)
                : prev(CajetaModule::getCurrentCodegenModule()) {
                CajetaModule::setCurrentCodegenModule(m);
            }
            ~CodegenFrame() { CajetaModule::setCurrentCodegenModule(prev); }
        } codegenFrame(module);

        // [heap-optional-return] lint fires before the abstract /
        // method-template early-returns: the body is still walkable on
        // template declarations and the warning is independent of LLVM
        // emission. Dedupes internally so a single template doesn't
        // re-warn per instantiation.
        lintHeapOptionalReturn();
        // Abstract methods carry no body — dispatch goes to a concrete
        // implementation via the vtable.
        if (abstractFlag) return;
        // Method-level template declarations don't get an LLVM function.
        // Per-call sites monomorphize via instantiateMethodTemplate, which
        // produces a concrete Method (methodTypeArguments non-empty) that
        // generateCode emits normally. See docs/specification/
        // MethodLevelTemplate.md.
        if (isMethodTemplate()) return;
        if (llvmBasicBlock != nullptr) {
            return;
        }

        // XPU @Kernel parameter-type validation (CajetaXPU step 3).
        // No-op when the method isn't a @Kernel. Throws cajeta::Exception
        // (errorId XPU-K01) on a non-admissible parameter type so the
        // diagnostic surfaces before we waste codegen on an invalid
        // kernel. The llvmBasicBlock guard above means this runs once
        // per method, not once per generateCode invocation.
        cajeta::xpu::validateKernelParams(shared_from_this());

        // CajetaXPU: a @Kernel — or a @Device helper — taking Buffer<T>
        // arguments operates on device memory and cannot execute on the host:
        // buffer indexing (buf[i]) is device-only (CajetaXPU.md §3.6). Its real
        // lowering is the device function (NvptxRegistration / the @Device
        // lowering in KernelLowering); the host function is never called in the
        // launch model. Emit a trivial host stub so host Phase-2 codegen doesn't
        // attempt to lower device-only constructs and crash on the resulting
        // null operand. Methods that take only host arrays / primitives keep
        // their real body (kernels for the CPU-emulation path; scalar @Device
        // helpers because they're harmless host functions).
        if (cajeta::xpu::isKernel(*this) || cajeta::xpu::isDevice(*this)) {
            bool hasDeviceBuffer = false;
            for (auto& p : parameterList) {
                if (p && p->getType()
                        && p->getType()->toCanonical().rfind(
                               "cajeta.xpu.KernelBuffer", 0) == 0) {
                    hasDeviceBuffer = true;
                    break;
                }
            }
            if (hasDeviceBuffer) {
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(
                    *module->getLlvmContext(), "entry", llvmFunction);
                llvm::IRBuilder<> b(bb);
                if (llvmFunction->getReturnType()->isVoidTy()) {
                    b.CreateRetVoid();
                } else {
                    b.CreateRet(llvm::Constant::getNullValue(
                        llvmFunction->getReturnType()));
                }
                return;
            }
        }

        // A bounded-wildcard instantiation (`Holder<? extends Floating>`) is an
        // abstract handle: you operate on it by capturing back to a concrete
        // instantiation (reified-capture-spec.md §5), never by calling its
        // methods directly — external mutator calls are PECS-rejected at the call
        // site (MethodCallExpression), and there is no single concrete primitive
        // element layout to lower a real body against (and the class's own-T
        // internal write would trip the PECS guard). Emit a trap stub so the
        // vtable/method symbol still resolves; it is never validly reached. Does
        // NOT apply to the unbounded `?` (e.g. Class<?>), whose bodies reflection
        // force-builds and dispatches — that path keeps its real body below.
        if (parent && parent->isBoundedWildcardInstantiation() && llvmFunction) {
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(
                *module->getLlvmContext(), "entry", llvmFunction);
            llvm::IRBuilder<> b(bb);
            if (llvmFunction->getReturnType()->isVoidTy()) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(llvm::Constant::getNullValue(
                    llvmFunction->getReturnType()));
            }
            llvmBasicBlock = bb;   // mark emitted (idempotency guard above)
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
            // Two forms:
            //   @Native("__cajeta_x")                 — runtime symbol (no lib)
            //   @Native(symbol="X", lib="zstd")       — external-library binding
            // The external form additionally records a native requirement
            // (lib-id, symbol) for the resolver; codegen is identical (the
            // symbol is an extern resolved at link/JIT time).
            std::string symbol = nativeAnn->getString("symbol");
            if (symbol.empty()) symbol = nativeAnn->getString("value");
            std::string lib = nativeAnn->getString("lib");
            if (symbol.empty()) {
                cerr << "@Native on " << buildCanonical(parent, name, parameterList, true)
                     << " requires a symbol-name argument" << std::endl;
                return;
            }
            emitNativeForwardingBody(symbol);
            if (!lib.empty()) recordNativeRequirement(lib, symbol);
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
                    std::string originalName = getLlvmSymbolName() + "__original";
                    llvmOriginalFunction = llvm::Function::Create(
                        llvmFunctionType, llvm::Function::ExternalLinkage,
                        originalName, getEmitModule()->getLlvmModule());
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
        // Save/restore the cursor scalars (builder/currentMethod) on `module`:
        // when this method's body is emitted on-reference while NESTED inside
        // another method's body (template instantiation) that emits into the SAME
        // module, both share that module's cursor — so the outer method's insert
        // point and current-method must be restored when we return, or its
        // remaining statements would emit into THIS function (and read THIS as the
        // current method — the async heap-return false positive). scopeStack/
        // structureStack self-balance via the push/pop below. Captured BEFORE the
        // sets, so they hold the outer values.
        llvm::IRBuilder<>* prevBuilder = module->getBuilder();
        MethodPtr prevCurrentMethod = module->getCurrentMethod();
        struct RestoreCursor {
            CajetaModulePtr m; llvm::IRBuilder<>* b; MethodPtr cm;
            ~RestoreCursor() { m->setBuilder(b); m->setCurrentMethod(cm); }
        } restoreCursor{module, prevBuilder, prevCurrentMethod};
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());
        // Mark the module new IR should land in for the duration of this body
        // (test-reuse: bodyFn lives in the user emit module even though `module`
        // is the resolution module). RAII restore so nested method codegen
        // (a call that drives another method's generateCode) nests correctly.
        // emitTargetLlvmModule() reads this; getRuntimeFunction / string
        // constants / spawn trampolines / lambda fns target it. == module's own
        // llvm module in production.
        llvm::Module* prevEmitLlvmModule = CajetaModule::getCurrentEmitLlvmModule();
        CajetaModule::setCurrentEmitLlvmModule(bodyFn->getParent());
        struct RestoreEmitLlvm {
            llvm::Module* prev;
            ~RestoreEmitLlvm() { CajetaModule::setCurrentEmitLlvmModule(prev); }
        } restoreEmitLlvm{prevEmitLlvmModule};

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

        // Value-returning (sret) methods reserve arg 0 for the hidden result
        // pointer, so the real parameters (including `this`) start at arg 1.
        int i = returnsStackValue() ? 1 : 0;
        for (auto& parameter: parameterList) {
            FieldPtr parameterField = make_shared<ParameterField>(module, parameter, bodyFn, i++);
            module->getScopeStack().peek()->putField(parameterField);
        }

        // cajeta-ir Unit 4 (closure specialization): a specialized instance had
        // its function-typed parameter(s) dropped from the signature above and
        // bound to a statically-known function. Inject a BoundClosureField for
        // each so a call to that name lowers to a DIRECT call (no closure record,
        // no indirect dispatch). No-op on every ordinary method.
        for (auto& bc : boundClosures) {
            auto boundField = make_shared<BoundClosureField>(
                module, bc.name, bc.fnType, bc.fn, bc.record);
            module->getScopeStack().peek()->putField(boundField);
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
        // Phase 0.6: emit the implicit scope frame ONLY when the body actually
        // spawns/scopes. Spawn-free methods (incl. every value-type operator and
        // SIMD hot loop) leave scopeWatermark null and emit no scope runtime
        // calls; the exit_to paths (here + emitScopeExitToWatermark) are guarded
        // on a null watermark. This generalizes --lazy-scope into the automatic,
        // always-correct "lazily push at spawn sites" form (and subsumes it: a
        // method that DOES spawn still pushes the frame).
        bool needsScopeFrame = bodyNeedsScopeFrame();
        if (needsScopeFrame) {
            scopeWatermark = builder->CreateAlloca(ptrTy, nullptr, "scope_watermark");
            if (llvm::Function* saveFn = module->getRuntimeFunction(
                    "__cajeta_scope_save_top")) {
                llvm::Value* mark = builder->CreateCall(saveFn, {});
                builder->CreateStore(mark, scopeWatermark);
            }
            // --lazy-scope additionally skips the heap-allocating scope_enter even
            // for spawning methods (the frame is then lazily pushed at the spawn
            // site); off by default.
            if (!module->getFlags().lazyScope) {
                if (llvm::Function* enterFn = module->getRuntimeFunction(
                        "__cajeta_scope_enter")) {
                    builder->CreateCall(enterFn, {});
                }
            }
        }

        // Debugger CP5: push a debug frame for this method (no-op unless
        // --debug-info). Paired with __cajeta_dbg_frame_leave on every return
        // path (emitScopeExitToWatermark + the synthetic fall-through below).
        dbg::emitDbgFrameEnter(module, getLlvmSymbolName());

        // Register the parameters as locals in the debug frame. Materializing
        // their slots here (getOrCreateAllocation) is the same store-arg-to-
        // alloca the body would do on first use; doing it at entry just makes
        // every parameter inspectable from the first statement on.
        if (module->getFlags().debugInfo) {
            for (auto& parameter : parameterList) {
                FieldPtr pf = module->getScopeStack().peek()
                                    ->getField(parameter->getName());
                if (!pf || !pf->getType()) continue;
                // CP7-1b memory facets. alloc: primitives live inline in the
                // slot (Stack), class/array/view params hold a pointer (Heap),
                // matching ParameterField::getOrCreateAllocation. ownership: a
                // `#`-transferred param takes ownership (Owner); any other
                // non-primitive param is a borrow the caller still owns; a
                // primitive is a plain value (Unknown role). `shared` is
                // deferred (XPU placement, not a parameter form).
                CajetaTypePtr pt = pf->getType();
                bool isArr  = dynamic_pointer_cast<CajetaArray>(pt) != nullptr;
                bool isPrim = (pt->getTypeFlags() & PRIMITIVE_FLAG) && !isArr;
                dbg::FieldFacetInputs facetIn;
                facetIn.isStackField = isPrim;
                facetIn.isHeapField  = !isPrim;
                facetIn.ownsDrop     = parameter->isTransferred()
                                       || pf->getDropEntry() != nullptr;
                facetIn.isReference  = !isPrim && !parameter->isTransferred();
                dbg::emitDbgLocal(module, pf->getName(),
                                  pt->toCanonical(),
                                  pf->getOrCreateAllocation(),
                                  dbg::classifyField(facetIn),
                                  pf->getDropEntry());
            }
        }

        // @NonNull parameter checks. Fire BEFORE the try frame so a
        // null-arg violation is a precondition failure that escapes
        // any in-method @AfterThrowing handling (matches the spec
        // intent — the method body doesn't get to "handle" a contract
        // violation on its own arguments).
        emitNonNullParamChecks(module);

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
        //
        // Multi-inheritance: invoke the no-arg ctor on EVERY parent in
        // declared order. Each parent's ctor writes only into fields
        // it owns (slots laid out by appendInherited / resolved by
        // getFieldLlvmIndex). Skipping a parent leaves its inherited
        // state uninitialized — required for `Optional<T> extends
        // Stream<T>, AbstractHashable<T>` (the AbstractHashable side
        // wouldn't initialize otherwise).
        // MultiClassing Phase 3 v4 vbase init. Before any inherited-field
        // access (which goes through vbase) and before super-ctor calls
        // (parent's ctor will set ITS own vbases), populate self's vbase
        // pointers to point at the inline ancestor sub-objects within
        // self. In a single-inheritance / no-diamond case these stay as
        // the canonical pointers forever. In a diamond descendant, the
        // descendant ctor (further down, after super-ctor calls)
        // overwrites non-first parents' vbase slots to canonical.
        if (constructor && parent && bodyFn->arg_size() > 0
                && !parent->getVbaseAncestors().empty()) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& vctx = *module->getLlvmContext();
            llvm::Type* vi8Ty = llvm::Type::getInt8Ty(vctx);
            llvm::Type* vi64Ty = llvm::Type::getInt64Ty(vctx);
            llvm::Type* parentLlvmType = parent->getLlvmType();
            for (auto& anc : parent->getVbaseAncestors()) {
                if (!anc) continue;
                int slotIdx = parent->getVbaseSlotIndex(anc.get());
                if (slotIdx < 0) continue;
                llvm::Value* slotPtr = builder->CreateStructGEP(
                    parentLlvmType, receiver, (unsigned) slotIdx,
                    "vbase_init_slot");
                uint64_t off = parent->getSubObjectByteOffset(anc.get());
                llvm::Value* ancPtr = (off == 0)
                    ? receiver
                    : builder->CreateInBoundsGEP(vi8Ty, receiver,
                        llvm::ConstantInt::get(vi64Ty, off),
                        "vbase_init_target");
                builder->CreateStore(ancPtr, slotPtr);
            }
        }

        // MultiClassing Phase 3 v4 vbase init — INHERITED sub-objects.
        // The block above initializes only SELF's own appended vbase
        // slots — those are the slots an inherited-field access loads when
        // the receiver's STATIC type is this most-derived class. But an
        // inherited-field read through an UPCAST base static type loads the
        // vbase pointer from the BASE sub-object's own vbase slot instead
        // (e.g. `e.message` where `e` is statically `NetException` but the
        // object is a `ConnectionRefusedException` subtype uses
        // NetException's vbase-to-Throwable slot, NOT the subtype's). Those
        // inherited sub-object vbase slots are normally initialized by each
        // ancestor's own constructor — but only if it actually RUNS. A
        // subtype whose parent has no no-arg constructor (so the auto
        // super-ctor invoke below is skipped) and that does not chain an
        // explicit `super(...)` would otherwise leave them null, and an
        // upcast inherited-field read then dereferences a null vbase
        // pointer and crashes. So initialize every first-parent-chain
        // ancestor's own vbase slots here too, pointing at self's canonical
        // sub-object positions. First-parent-chain ancestors all sit at
        // byte offset 0 (they share self's primary layout prefix), so each
        // ancestor's struct is a valid prefix of self and a StructGEP off
        // the receiver lands on the right slot. Non-first (diamond) parents
        // are still handled by the descendant fixup further below.
        if (constructor && parent && bodyFn->arg_size() > 0) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& vctx = *module->getLlvmContext();
            llvm::Type* vi8Ty = llvm::Type::getInt8Ty(vctx);
            llvm::Type* vi64Ty = llvm::Type::getInt64Ty(vctx);
            CajetaClass* chain = parent->getSuperClasses().empty()
                ? nullptr : parent->getSuperClasses().front().get();
            while (chain) {
                if (!chain->getVbaseAncestors().empty()) {
                    llvm::Type* chainLlvm = chain->getLlvmType();
                    for (auto& anc : chain->getVbaseAncestors()) {
                        if (!anc) continue;
                        int slotIdx = chain->getVbaseSlotIndex(anc.get());
                        if (slotIdx < 0) continue;
                        llvm::Value* slotPtr = builder->CreateStructGEP(
                            chainLlvm, receiver, (unsigned) slotIdx,
                            "vbase_init_inh_slot");
                        uint64_t off = parent->getSubObjectByteOffset(anc.get());
                        llvm::Value* ancPtr = (off == 0)
                            ? receiver
                            : builder->CreateInBoundsGEP(vi8Ty, receiver,
                                llvm::ConstantInt::get(vi64Ty, off),
                                "vbase_init_inh_target");
                        builder->CreateStore(ancPtr, slotPtr);
                    }
                }
                chain = chain->getSuperClasses().empty()
                    ? nullptr : chain->getSuperClasses().front().get();
            }
        }

        if (constructor && parent && bodyFn->arg_size() > 0) {
            // MultiClassing R-2: pre-walk the ctor body's AST to detect
            // any explicit `super(args)` call. The warning condition
            // requires the user to have explicitly picked one parent's
            // ctor — that's the signal they were thinking about ctor
            // selection and might've missed a sibling. Walk recursively
            // so super calls nested in if/for/etc. blocks are detected.
            bool userHasExplicitSuperCtor = false;
            if (block) {
                // ExpressionStatement and ReturnStatement store their
                // wrapped expression in a member field (not in children)
                // so the default child walk misses it — special-case
                // both to forward into the expression. Other wrapper
                // shapes (if/for/while/block) keep their inner
                // statements in `children` so the recursive walk
                // reaches them naturally.
                std::function<bool(AbstractSyntaxNodePtr)> findSuperCtor =
                    [&](AbstractSyntaxNodePtr node) -> bool {
                        if (!node) return false;
                        if (auto mce = std::dynamic_pointer_cast<
                                cajeta::MethodCallExpression>(node)) {
                            if (mce->isSuperCtorCall()) return true;
                        }
                        if (auto es = std::dynamic_pointer_cast<
                                cajeta::ExpressionStatement>(node)) {
                            if (findSuperCtor(es->getExpression())) return true;
                        }
                        for (auto& child : node->getChildren()) {
                            if (findSuperCtor(child)) return true;
                        }
                        return false;
                    };
                userHasExplicitSuperCtor = findSuperCtor(block);
            }
            llvm::Value* receiver = bodyFn->getArg(0);
            int parentIdx = 0;
            for (auto& sup : parent->getSuperClasses()) {
                bool isNonFirstParent = (parentIdx > 0);
                parentIdx++;
                if (!sup) continue;
                std::vector<ParameterEntry> noArgs;
                std::string supCtorName = sup->getQName()->getTypeName();
                // MultiClassing R-2: if user wrote explicit super(args)
                // AND this is a sibling (non-first) parent AND the
                // sibling has BOTH a no-arg ctor and an args ctor,
                // warn that the no-arg was picked implicitly — the
                // user may have wanted to call the sibling's args
                // ctor too. Narrow on purpose (only fires when all
                // three conditions hold) so the build log doesn't
                // flood on intentional skips.
                if (userHasExplicitSuperCtor && isNonFirstParent) {
                    bool supHasNoArg = false;
                    bool supHasArgs = false;
                    // Walk `methods` (map by canonical) rather than
                    // `methodList` — ctors are kept in the map, not
                    // the list, so iterating methodList would skip
                    // them entirely and the warning would never fire.
                    for (auto& [name, m] : sup->getMethods()) {
                        if (!m || !m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC)
                                != m->getModifiers().end()) continue;
                        // parameterList includes implicit `this` for
                        // non-static ctors — minus 1 for user-visible
                        // arg count.
                        int userArgs = (int) m->getParameterList().size() - 1;
                        if (userArgs <= 0) supHasNoArg = true;
                        else supHasArgs = true;
                    }
                    if (supHasNoArg && supHasArgs) {
                        std::cerr << "warning: [implicit-ctor-skip] in "
                            << parent->getQName()->toCanonical()
                            << "(): explicit super(...) targets only the "
                            << "first parent's ctor; sibling parent '"
                            << sup->getQName()->toCanonical()
                            << "' also has an args constructor — its "
                            << "no-arg constructor was picked implicitly. "
                            << "Consider super<"
                            << sup->getQName()->getTypeName()
                            << ">(...) once that grammar lands, or "
                            << "restructure to pick explicitly via "
                            << "composition."
                            << std::endl;
                    }
                }
                if (sup->resolveMethod(supCtorName, noArgs,
                        /*isConstructor=*/true, /*floatingParams=*/false)) {
                    // Per-parent sub-object adjustment (Gap 8). The receiver
                    // is the subclass instance pointer; the parent ctor was
                    // compiled against the parent's standalone struct layout,
                    // so we must pass a pointer to where the parent's
                    // sub-object actually lives inside the subclass instance.
                    // First parent's offset is 0 (shares the primary vtable
                    // slot); non-first parents have their sub-object further
                    // down.
                    llvm::Value* supThis = receiver;
                    uint64_t off = parent->getSubObjectByteOffset(sup.get());
                    if (off != 0) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(
                            *module->getLlvmContext());
                        supThis = builder->CreateInBoundsGEP(i8Ty, receiver,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                off),
                            "super_ctor_subobj");
                    }
                    sup->invokeMethod(supCtorName, noArgs,
                        /*isConstructor=*/true,
                        supThis,
                        /*callerModule=*/module);
                }
            }
        }

        // MultiClassing Phase 3 v4 vbase descendant fixup. After all
        // parent ctors have run (each having set ITS own vbase pointers
        // to point at inline copies within its sub-object), walk each
        // non-first parent and overwrite their vbase slots to point at
        // self's CANONICAL (first-encountered) sub-object positions.
        // This is what makes a diamond ancestor "shared" — every path's
        // vbase load reaches the same storage.
        //
        // For the first parent we don't touch anything: its vbase
        // initializations already point at canonical positions because
        // self's canonical for any first-parent-chain ancestor IS the
        // position the first parent already set.
        if (constructor && parent && bodyFn->arg_size() > 0
                && parent->getSuperClasses().size() > 1) {
            llvm::Value* receiver = bodyFn->getArg(0);
            auto& fctx = *module->getLlvmContext();
            llvm::Type* fi8Ty = llvm::Type::getInt8Ty(fctx);
            llvm::Type* fi64Ty = llvm::Type::getInt64Ty(fctx);
            int fpIdx = 0;
            for (auto& sup : parent->getSuperClasses()) {
                bool isNonFirst = (fpIdx > 0);
                fpIdx++;
                if (!isNonFirst) continue;
                if (!sup) continue;
                if (sup->getVbaseAncestors().empty()) continue;
                // Compute non-first parent's start in self.
                uint64_t parentOff = parent->getSubObjectByteOffset(sup.get());
                llvm::Value* supThis = (parentOff == 0)
                    ? receiver
                    : builder->CreateInBoundsGEP(fi8Ty, receiver,
                        llvm::ConstantInt::get(fi64Ty, parentOff),
                        "vbase_fixup_supbase");
                llvm::Type* parentLlvm = sup->getLlvmType();
                for (auto& anc : sup->getVbaseAncestors()) {
                    if (!anc) continue;
                    int slotIdx = sup->getVbaseSlotIndex(anc.get());
                    if (slotIdx < 0) continue;
                    // self's canonical position for anc (first-encountered
                    // sub-object slot via self's subObjectSlotMap).
                    uint64_t canonOff = parent->getSubObjectByteOffset(anc.get());
                    llvm::Value* canonPtr = (canonOff == 0)
                        ? receiver
                        : builder->CreateInBoundsGEP(fi8Ty, receiver,
                            llvm::ConstantInt::get(fi64Ty, canonOff),
                            "vbase_fixup_canon");
                    llvm::Value* slotPtr = builder->CreateStructGEP(
                        parentLlvm, supThis, (unsigned) slotIdx,
                        "vbase_fixup_slot");
                    builder->CreateStore(canonPtr, slotPtr);
                }
            }
        }

        // Instance-field initializers in a USER-written ctor. Java
        // semantics: super()/this() resolves → field initializers
        // execute → ctor body runs. SynthesizedConstructorMethod
        // handles its own initializer pass (it doesn't go through
        // Method::generateCode); this branch is the user-ctor case.
        //
        // Skip the field-init pass when the user explicitly delegates
        // via `this(args)` — the delegated-to ctor will run the
        // initializers, so doing it here would double-init (and worse,
        // pre-clobber whatever the delegated ctor set). Detect via the
        // same recursive AST walk used for super-ctor probing above.
        if (constructor && parent && bodyFn->arg_size() > 0 && block) {
            bool delegatesViaThis = false;
            std::function<bool(AbstractSyntaxNodePtr)> findThisCtor =
                [&](AbstractSyntaxNodePtr node) -> bool {
                    if (!node) return false;
                    if (auto mce = std::dynamic_pointer_cast<
                            cajeta::MethodCallExpression>(node)) {
                        if (mce->getMethodCallName() == "this") return true;
                    }
                    if (auto es = std::dynamic_pointer_cast<
                            cajeta::ExpressionStatement>(node)) {
                        if (findThisCtor(es->getExpression())) return true;
                    }
                    for (auto& child : node->getChildren()) {
                        if (findThisCtor(child)) return true;
                    }
                    return false;
                };
            delegatesViaThis = findThisCtor(block);

            if (!delegatesViaThis) {
                llvm::Value* thisPtr = bodyFn->getArg(0);
                auto& ictx = *module->getLlvmContext();
                for (auto& prop : parent->getPropertyList()) {
                    if (!prop || prop->isStatic()) continue;
                    auto init = prop->getInitializer();
                    if (!init) continue;
                    int idx = parent->getFieldLlvmIndex(prop);
                    if (idx < 0) continue;
                    llvm::Value* initVal = init->generateCode(module);
                    if (!initVal) continue;
                    llvm::Value* fp = builder->CreateStructGEP(
                        parent->getLlvmType(), thisPtr, (unsigned) idx,
                        std::string("user_ctor.init.") + prop->getName());
                    // Width / FP coercion mirrors the synthesized-ctor
                    // path (SynthesizedConstructorMethod.cpp:117-130).
                    CajetaTypePtr ft = prop->getType();
                    llvm::Type* slotTy = ft ? ft->getLlvmType() : nullptr;
                    if (slotTy && initVal->getType() != slotTy) {
                        llvm::Type* srcTy = initVal->getType();
                        if (slotTy->isIntegerTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateIntCast(initVal, slotTy, /*isSigned=*/true);
                        } else if (slotTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPCast(initVal, slotTy);
                        } else if (slotTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                            initVal = builder->CreateSIToFP(initVal, slotTy);
                        } else if (slotTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                            initVal = builder->CreateFPToSI(initVal, slotTy);
                        }
                    }
                    builder->CreateStore(initVal, fp);
                }
                (void) ictx;
            }
        }

        // Type-resolver pre-pass: populates Expression::resolvedType so codegen can
        // distinguish e.g. fp8 from i8 when they share an LLVM type.
        if (block) {
            block->resolveTypes(module);
            computeArenaEligibility();   // flag non-escaping concat locals (U2)
            block->generateCode(module);
        }

        // Emit a terminator only if the body didn't (i.e. no explicit return). For void
        // methods this is the conventional "fall through to ret"; for non-void methods
        // a missing return is undefined in Cajeta semantics, but we emit a zero-value
        // ret so the IR remains well-formed.
        if (!builder->GetInsertBlock()->hasTerminator()) {
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
            // Phase 0.6: null when the method elided its scope frame (spawn-free
            // body) — nothing to pop.
            if (scopeWatermark) {
                if (llvm::Function* exitToFn = module->getRuntimeFunction(
                        "__cajeta_scope_exit_to")) {
                    llvm::Value* mark = builder->CreateLoad(ptrTy, scopeWatermark);
                    builder->CreateCall(exitToFn, {mark});
                }
            }
            // Debugger CP5: pop this method's debug frame on the fall-through
            // return path (mirrors the explicit-return path in
            // emitScopeExitToWatermark). No-op unless --debug-info.
            dbg::emitDbgFrameLeave(module);
            // Fire scope-end drops before the synthetic return so the chain is
            // unwound the same way an explicit `return` would do it.
            emitOwnerDrops(module);
            // Use the LLVM function's actual return type, not the
            // CajetaType's LLVM type. They diverge for class returns:
            // CajetaClass::getLlvmType() yields the struct layout, but
            // the function signature (built by buildLlvmFunctionType /
            // similar) uses `ptr` for class-pass-by-pointer ABI. Before
            // 2026-05-19, the synthetic-return path used the CajetaType
            // and emitted a struct-typed PoisonValue for class returns,
            // producing `ret <struct> poison` against a `ptr ()`
            // function signature — JIT-time verify rejection with
            // "Function return type does not match operand type of
            // return inst!". Source: `while (true) { ... return X; }`
            // patterns where the visitor doesn't prove the loop never
            // exits, so this fallback fires for genuinely unreachable
            // code. Class return broke; primitive return worked because
            // the int/float/ptr arms above produced correctly-typed
            // zero/null values.
            llvm::Function* hostFn = builder->GetInsertBlock()->getParent();
            llvm::Type* retLlvmTy = hostFn ? hostFn->getReturnType() : nullptr;
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

        // S4 size guard. A @ValueType operator is marked `alwaysinline` at
        // prototype time (see generatePrototype) so it folds to flat,
        // intrinsic-like IR at O0. But force-inlining a LARGE operator body at
        // every call site bloats code without the register-residency payoff.
        // Now that the body is emitted we can measure it: if it exceeds the
        // threshold, drop the attribute so the operator keeps a real call.
        // Plain (no-@Around) path only — there the body lives in llvmFunction.
        // See value-type-overloading-plan.md Decision #5 (threshold).
        if (!hasAroundWrapper && llvmFunction
                && llvmFunction->hasFnAttribute(llvm::Attribute::AlwaysInline)) {
            unsigned instrs = 0;
            for (auto& bb : *llvmFunction) instrs += (unsigned) bb.size();
            // Chosen so typical value-type operators (a handful of field loads +
            // scalar/vector ops + return — well under 100 IR instructions, even
            // for a 4x4 matrix add) still inline, while a genuinely large body
            // keeps a call.
            static const unsigned kValueOpInlineMaxInstrs = 100;
            if (instrs > kValueOpInlineMaxInstrs) {
                llvmFunction->removeFnAttr(llvm::Attribute::AlwaysInline);
            }
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

        std::string baseName = getLlvmSymbolName();

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
        // Launch-borrow drop gate (XPU-K02, the implicit-drop half). A
        // `kernel.launch(...)` borrows each Buffer until `Stream.sync()`; with
        // Buffer<T>'s RAII destructor, letting a still-borrowed buffer leave
        // scope before a sync would free device memory an in-flight kernel
        // still references. The explicit-`free()` half is caught at the call
        // site (MethodCallExpression); this catches the drop at scope exit.
        // Only owned locals actually dropped here (a live drop entry, not
        // moved-out) trip it — borrowed params and `#`-transferred buffers are
        // skipped. Sync clears the borrow set, so a synced buffer is fine.
        if (ScopePtr sc = module->getScopeStack().peek()) {
            for (const string& name : sc->pendingLaunchBorrows()) {
                if (!sc->containsField(name) || sc->isMoved(name)) continue;
                FieldPtr f = sc->getField(name);
                if (!f || !f->getDropEntry()) continue;
                CajetaTypePtr t = f->getType();
                if (!t) continue;
                // Every borrowed device resource — Buffer, Texture2D, and
                // AccelerationStructure — must outlive the launch. (Previously
                // Buffer-only, so Texture2D/AS borrows were recorded but never
                // enforced; AS in particular could free its device BVH mid-kernel.)
                const string c = t->toCanonical();
                bool isDeviceResource =
                    c.rfind("cajeta.xpu.KernelBuffer", 0) == 0 ||
                    c.rfind("cajeta.gpu.Texture2D", 0) == 0 ||
                    c.rfind("cajeta.gpu.Texture3D", 0) == 0 ||
                    c.rfind("cajeta.gpu.Texture1D", 0) == 0 ||
                    c.rfind("cajeta.gpu.Texture2DArray", 0) == 0 ||
                    c.rfind("cajeta.gpu.TextureCube", 0) == 0 ||
                    c == "cajeta.xpu.AccelerationStructure";
                if (!isDeviceResource) continue;
                throw Exception(
                    "device resource '" + name + "' leaves scope while a launch "
                    "still references it — sync the stream (Stream.sync()) before "
                    "it is dropped", "XPU-K02");
            }
        }
        module->getScopeStack().pop();
    }

    FieldPtr Method::getVariable(string name) {
        ScopePtr scope = module->getScopeStack().peek();
        return scope->getField(name);
    }


    // Two-layer naming helpers (docs/specification/lang/MethodLevelTemplate.md
    // § Status / Known limitations). For instantiations of a method-
    // template, append the concrete method-level type args so the map
    // key and LLVM symbol disambiguate two instantiations that would
    // otherwise share a single `toCanonical` — the case where the T-
    // vars don't appear in value params (e.g. `static <T> int32 sizeOf()`
    // or `static <T> Collector<T, ArrayList<T>> toList()`).
    //
    // Suffix shape: `<canonical,canonical,...>` over methodTypeArguments.
    // Empty for the template itself and for ordinary non-templated
    // methods, so non-instantiation keys/symbols are unchanged.
    static string buildMethodTypeArgSuffix(
            const vector<CajetaTypePtr>& methodTypeArguments) {
        if (methodTypeArguments.empty()) {
            return "";
        }
        string s = "<";
        for (size_t i = 0; i < methodTypeArguments.size(); ++i) {
            if (i > 0) s += ",";
            s += methodTypeArguments[i]->getQName()->toCanonical();
        }
        s += ">";
        return s;
    }

    const string Method::getMapKey(bool labeled) const {
        // const_cast: toCanonical and parameterList walk are read-only.
        // const-correctness across the Method API would be a separate
        // refactor.
        string base = const_cast<Method*>(this)->toCanonical(labeled);
        if (methodTypeParameters.empty()) {
            // Ordinary (non-templated) method — plain canonical (+ any
            // closure-specialization tag, normally empty).
            return base + specializationTag;
        }
        if (!methodTypeArguments.empty()) {
            // Concrete instantiation — append the resolved type-args
            // so distinct instantiations of the same template have
            // distinct keys. See § two-layer naming below. The
            // specialization tag (cajeta-ir Unit 4) further distinguishes a
            // closure-specialized instance from its base instantiation and
            // from a sibling specialized over a different lambda.
            return base + buildMethodTypeArgSuffix(methodTypeArguments)
                 + specializationTag;
        }
        // Method-template declaration (template params declared, no
        // concrete args bound yet). Suffix with the T-var NAMES so
        // the declaration's key is distinct from a same-value-param
        // non-templated overload. Without this, addMethod's
        // duplicate-static check rejected the second registration
        // when both `static T parse(int8[], int64)` and `<T> static T
        // parse(int8[], int64)` were declared in the same class —
        // forcing the workaround of giving the templated variant a
        // different name (e.g. `parseT`). The names-not-types choice
        // keeps the suffix stable across re-parses of the same
        // declaration; the resolver doesn't compare these strings to
        // user-supplied type args, so the choice is purely cosmetic
        // for the key-distinctness purpose.
        string s = base + "<";
        for (size_t i = 0; i < methodTypeParameters.size(); ++i) {
            if (i > 0) s += ",";
            s += methodTypeParameters[i].name;
        }
        s += ">";
        return s;
    }

    const string Method::getLlvmSymbolName() const {
        return getMapKey(true);
    }

    // cajeta-ir Unit 4: drop a (closure-specialized-away) parameter from both the
    // ordered list and the by-name map so the specialized instance's signature
    // and canonical name omit it.
    void Method::dropParameter(const string& paramName) {
        for (auto it = parameterList.begin(); it != parameterList.end(); ++it) {
            if (*it && (*it)->getName() == paramName) {
                parameterList.erase(it);
                break;
            }
        }
        parameters.erase(paramName);
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

    string Method::buildTemplateOriginCanonical(
            CajetaClassPtr instClass,
            const string& name,
            vector<FormalParameterPtr> parameters,
            bool labeled) {
        auto origin = instClass ? instClass->getTemplateOrigin() : nullptr;
        if (!origin) {
            return buildCanonical(instClass, name, parameters, labeled);
        }
        const auto& typeParams = instClass->getTypeParameters();
        const auto& typeArgs = instClass->getTypeArguments();
        // Pre-compute the unsubstituted name for each declared type arg by
        // matching pointer identity. Identity match is the right comparison —
        // the substitution that happened at instantiation time stored the
        // typeArgument pointer as the field type, so a parameter whose type
        // appears in typeArgs is exactly one that came from a template
        // parameter slot. Equal canonicals across pointers (e.g. two
        // independent instantiations resolving to the same primitive int32)
        // also map back; we accept the false-positive risk because the
        // canonical-name path lands on the same alias hash anyway.
        auto unsubstitute = [&](CajetaTypePtr t) -> string {
            if (!t) return "";
            if (typeParams.size() == typeArgs.size()) {
                for (size_t i = 0; i < typeArgs.size(); ++i) {
                    if (typeArgs[i].get() == t.get()) {
                        return typeParams[i].name;
                    }
                }
            }
            return t->toCanonical();
        };

        string canonical;
        canonical.append(origin->toCanonical());
        canonical.append("::");
        canonical.append(name);
        canonical.append("(");

        if (labeled) {
            sort(parameters.begin(), parameters.end(),
                [](FormalParameterPtr a, FormalParameterPtr b) {
                    return a->getName() > b->getName();
                });
        }

        if (!parameters.empty()) {
            bool first = true;
            for (auto& parameter : parameters) {
                if (first) first = false; else canonical.append(",");
                if (labeled) {
                    canonical.append(parameter->getName()).append(":");
                }
                canonical.append(unsubstitute(parameter->getType()));
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