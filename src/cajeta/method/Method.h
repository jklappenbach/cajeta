//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "../type/Modifiable.h"
#include "llvm/IR/BasicBlock.h"
#include "../type/QualifiedName.h"
#include "../type/Annotatable.h"
#include "../type/CajetaType.h"
#include "../type/FormalParameter.h"
#include "../asn/Block.h"
#include "../type/Scope.h"
#include "../type/CajetaType.h"
#include "../field/Field.h"
#include "queue"
#include "map"

using namespace std;

namespace cajeta {
    class CajetaModule;

    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Expression;

    class CajetaClass;

    typedef shared_ptr<CajetaClass> CajetaClassPtr;

    class Mathod;

    typedef shared_ptr<Method> MethodPtr;

    // Advice kinds — which `@Before` / `@After` / etc. annotation
    // sits on the advice method. A3's pointcut-matching pass tags
    // each AdviceMatch with one of these so A4+'s codegen can pick
    // the right wrapper shape per advice form.
    enum class AdviceKind {
        Before,
        After,
        Around,
        AfterReturning,
        AfterThrowing,
    };

    // Pointcut shape — how the advice method's pointcut argument
    // resolved. Marker-annotation pointcut: the advice fires on
    // every method annotated with the named annotation type.
    // Type-based pointcut: the advice fires on every method on the
    // named class or any subclass. v1's two recognized shapes.
    enum class PointcutShape {
        MarkerAnnotation,
        Type,
    };

    // One (aspect, advice method, advice kind, pointcut shape)
    // tuple cached on a user Method that the pointcut-matching
    // pass deemed a match. A4+ walks the list at codegen time to
    // generate the appropriate wrapper.
    struct AdviceMatch {
        CajetaClassPtr aspectClass;     // declaring @Aspect class
        MethodPtr adviceMethod;         // method annotated @Before/etc.
        AdviceKind kind;
        PointcutShape shape;
    };

    class MethodCallParameter;

    struct ParameterEntry {
        CajetaTypePtr type;
        llvm::Value* value;
        string label;
        ParameterEntry(CajetaTypePtr type, string label, llvm::Value* value) { this->type = type; this->label = label; this->value = value; }
        ParameterEntry(const ParameterEntry& src) { type = src.type; label = src.label; value = src.value; }
    };

    class Method : public Modifiable, public Annotatable, public std::enable_shared_from_this<Method> {
    protected:
        static map<string, MethodPtr> archive;
        string name;
        CajetaClassPtr parent;
        CajetaTypePtr returnType;
        // True iff the return type is prefixed with `#`, meaning the method
        // transfers ownership of the returned value to its caller. See
        // `MemoryModel.md` § Function signatures.
        bool returnsOwnership = false;
        BlockPtr block;
        bool constructor;
        // Abstract method — has no body, no LLVM function declaration.
        // Interface methods are abstract; the implementing class's matching
        // method is what actually executes at dispatch time. The abstract
        // method's only role is to carry the signature (for canonical
        // computation + vtable-hash lookup).
        bool abstractFlag = false;
        // Varargs (`T... args`) method. The last parameter's type is `T[]`;
        // call sites with more than (parameterList.size() - 1) args pack
        // the trailing args into a fresh array before passing. Tracked as a
        // flag because the parameter type alone (`T[]`) can't distinguish
        // a varargs slot from a regular array-typed slot.
        bool varargsFlag = false;
        map<string, FormalParameterPtr> parameters;
        vector<FormalParameterPtr> parameterList;
        // RecoverableException subtypes the method documents itself as
        // throwing (Java-style `throws T1, T2` clause). Advisory only —
        // no compile-time enforcement; the lint pass uses this list to
        // emit uncaught-throws warnings at call sites. UnrecoverableException
        // subtypes are not declared here (per ErrorModel.md). The list
        // stores QualifiedNamePtrs because the named types may not have
        // been resolved at parse time; resolution happens lazily when the
        // lint pass walks them.
        vector<QualifiedNamePtr> throwsList;
        int virtualTableIndex;

        // Stack of drop frames. Each Block::generateCode pushes a frame
        // at entry, registers any owned locals declared inside into the
        // top frame, and fires + pops the frame's entries at the
        // block's closing `}` (unless the block was already terminated
        // by a return/throw — those fire ALL frames LIFO themselves
        // before the terminator). This is what makes RAII guards
        // release at inner-block exit instead of at method exit. See
        // MemoryModel.md § Runtime: drop chain with watermark.
        vector<vector<llvm::Value*>> dropFrameStack;

        CajetaModulePtr module;
        llvm::IRBuilder<>* builder;
        llvm::FunctionType* llvmFunctionType;
        llvm::Function* llvmFunction;
        // A5 method extraction: when at least one @Around advice
        // matches this method, the user-written body emits into THIS
        // separately-named llvm Function (canonical + `__original`
        // suffix), and llvmFunction becomes the wrapper whose body
        // calls the advice with this function pointer as the
        // `@Original` proceed argument. Null on methods that don't
        // need wrapping — the body emits directly into llvmFunction
        // and external callers' getLlvmFunction() resolves to the
        // same direct entry point.
        llvm::Function* llvmOriginalFunction = nullptr;
        llvm::BasicBlock* llvmBasicBlock;
        // R5-A' implicit function-body scope: at function entry codegen
        // alloca's a ptr slot here and stores `__cajeta_scope_save_top()`
        // into it. Each return path loads it back and calls
        // `__cajeta_scope_exit_to(watermark)` so every scope frame
        // pushed inside the function — implicit function-body OR any
        // explicit `scope { }` the return is inside of — gets waited
        // and popped before the ret instruction.
        llvm::AllocaInst* scopeWatermark = nullptr;
        // Advice matches: each entry says "this aspect's advice
        // method applies to me." Populated by the pointcut-matching
        // pass (AspectModel.md § A3) that runs once per Compiler
        // after parse, before codegen. Read by A4+'s codegen wrapper.
        vector<AdviceMatch> matchingAdvice;
    public:
        Method(CajetaModulePtr module,
            string& name,
            CajetaTypePtr returnType,
            vector<FormalParameterPtr> parameters,
            BlockPtr block,
            CajetaClassPtr parent);

        Method(CajetaModulePtr module,
            string name,
            CajetaTypePtr returnType,
            CajetaClassPtr parent);

        llvm::FunctionType* getLlvmFunctionType() {
            if (!llvmFunctionType) {
                generatePrototype();
            }
            return llvmFunctionType;
        }

        llvm::AllocaInst* getScopeWatermark() const { return scopeWatermark; }

        const vector<QualifiedNamePtr>& getThrowsList() const { return throwsList; }
        void setThrowsList(vector<QualifiedNamePtr> list) { throwsList = std::move(list); }

        // Advice-match cache (AspectModel.md § A3). The pointcut-
        // matching pass appends; A4+ codegen iterates. Empty for
        // every method by default — only the matching pass populates.
        void addAdviceMatch(AdviceMatch m) {
            matchingAdvice.push_back(std::move(m));
        }
        const vector<AdviceMatch>& getMatchingAdvice() const {
            return matchingAdvice;
        }

        // A4 codegen wrappers. emitBeforeAdvice fires every matching
        // @Before advice at the current insert point (called from
        // Method::generateCode right after scope_enter, before the
        // body emits). emitAfterAdvice fires every matching @After
        // advice (called from Method::generateCode at fall-through
        // exit and from ReturnStatement::generateCode at every
        // explicit return — both BEFORE the existing
        // emitScopeExitToWatermark / emitOwnerDrops calls, so the
        // advice runs in the method-body lifetime).
        //
        // v1 advice constraints (relaxed in A5+): static method,
        // no parameters, void return. Methods that don't match are
        // silently skipped — A12's diagnostics pass surfaces them.
        void emitBeforeAdvice(CajetaModulePtr module);
        void emitAfterAdvice(CajetaModulePtr module);
        // A6: @AfterReturning fires only on the normal-return path
        // (same hook points as @After). @AfterThrowing fires only
        // from inside the catch arm of the try/catch wrapping (see
        // hasAfterThrowingAdvice). v1 advice keeps the no-arg shape
        // — the return-value / Throwable parameters from the spec
        // are deferred to a follow-up.
        void emitAfterReturningAdvice(CajetaModulePtr module);
        void emitAfterThrowingAdvice(CajetaModulePtr module);
        bool hasAfterThrowingAdvice() const;

        // A6 try-frame setup. Push an exception frame + setjmp at
        // the current insert point; return the catch basic block
        // (still empty — the caller emits the catch arm separately
        // after the body finishes). The frame's storage is alloca'd
        // in the function entry block to keep setjmp/alloca
        // interaction sane. Reused by Method::generateCode (for
        // non-@Around methods) and emitAroundWrapper (for the
        // wrapper path). The body emits into the "try" block this
        // function leaves the builder pointing at.
        struct TryFrameInfo {
            llvm::BasicBlock* tryBB;
            llvm::BasicBlock* catchBB;
            llvm::Value* framePtr;
        };
        TryFrameInfo emitAfterThrowingTryEntry(
            CajetaModulePtr module, llvm::IRBuilder<>& wb,
            llvm::Function* parentFn);
        // Pops the try frame at a normal-exit point. Called from
        // every normal-return site (Method::generateCode fall-
        // through, ReturnStatement explicit returns) when the
        // enclosing method has @AfterThrowing matched. No-op
        // otherwise — call sites guard with hasAfterThrowingAdvice.
        void emitAfterThrowingTryPop(CajetaModulePtr module);
        // Catch arm body: get the thrown value, pop the frame,
        // fire @AfterThrowing + @After advice, re-raise. The
        // builder is left at an unreachable terminator. Caller
        // should set the insert point to the catchBB before
        // calling this.
        void emitAfterThrowingCatchArm(
            CajetaModulePtr module, llvm::IRBuilder<>& wb);

        // A5: emit the @Around wrapper. Called from generateCode
        // after the original body has been emitted into
        // llvmOriginalFunction. The wrapper fires any @Before
        // advice, calls the @Around advice (passing
        // llvmOriginalFunction as the proceed argument), fires any
        // @After advice, and returns the advice's result. Only one
        // @Around match is honored in v1; @Order-driven chaining
        // of multiple Arounds joins A7.
        void emitAroundWrapper();

        llvm::Function* getLlvmFunction() { return llvmFunction; }
        // Extracted-body function for @Around-wrapped methods. Null
        // unless A5 method extraction kicked in. External callers
        // shouldn't usually need this — the public entry is
        // getLlvmFunction (the wrapper); the original is wired
        // internally by Method::generateCode.
        llvm::Function* getLlvmOriginalFunction() { return llvmOriginalFunction; }

        bool isConstructor() { return constructor; }

        bool isAbstract() const { return abstractFlag; }
        void setAbstract(bool v) { abstractFlag = v; }

        bool isVarargs() const { return varargsFlag; }
        void setVarargs(bool v) { varargsFlag = v; }

        vector<FormalParameterPtr> getParameterList() { return parameterList; }

        map<string, FormalParameterPtr> getParameters() { return parameters; }

        CajetaTypePtr getReturnType() { return returnType; }

        CajetaClassPtr getParent() const { return parent; }

        bool isReturnsOwnership() const { return returnsOwnership; }
        void setReturnsOwnership(bool v) { returnsOwnership = v; }

        // Push a fresh (empty) drop frame onto the stack. Block::generateCode
        // calls this at its entry; the frame collects every owned local
        // declared inside the block until the matching pop.
        void pushDropFrame() { dropFrameStack.emplace_back(); }

        // Pop the top frame without emitting any IR. Use after the
        // matching emitTopFrameDrops (or after a terminator made the
        // drops dead code).
        void popDropFrame() {
            if (!dropFrameStack.empty()) dropFrameStack.pop_back();
        }

        // Register a DropEntry alloca (as emitted by LocalVariableDeclaration for
        // an owned local) into the current top frame.
        void registerDropEntry(llvm::Value* entry) {
            if (dropFrameStack.empty()) {
                // Defensive: a caller that registers before any block has
                // pushed gets a synthetic outer frame so the entry isn't
                // silently dropped. Shouldn't happen in normal flow.
                dropFrameStack.emplace_back();
            }
            dropFrameStack.back().push_back(entry);
        }

        // Emit drop-chain pops + drops for the top frame's entries in
        // reverse order. Called by Block::generateCode at its normal
        // closing `}` to release the locals declared inside that block.
        // The frame is NOT popped — call popDropFrame() afterwards.
        void emitTopFrameDrops(CajetaModulePtr module);

        // Emit drop-chain pops + drops for EVERY frame still on the
        // stack, walking from innermost to outermost (LIFO across
        // frames, LIFO within each). Called by ReturnStatement before
        // the ret so the chain unwinds across all enclosing blocks at
        // once. Frames stay on the stack — each enclosing block will
        // observe the terminator and skip its own fire on the way out.
        void emitOwnerDrops(CajetaModulePtr module);

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        int getVirtualTableIndex() {
            return virtualTableIndex;
        }

        void setVirtualTableIndex(int virtualTableIndex) {
            this->virtualTableIndex = virtualTableIndex;
        }

        bool operator<(const Method& src) const {
            return virtualTableIndex < src.virtualTableIndex;
        }

        void destroyScope();

        FieldPtr getVariable(string name);

        CajetaModulePtr getModule() { return module; }

        const string toCanonical(bool labeled = false) {
            return buildCanonical(parent, name, parameterList, labeled);
        }

        const string toGeneric(bool labeled = false) {
            return buildGeneric(parent, name, parameterList, labeled);
        }

        void generatePrototype();

        void setBlock(BlockPtr block);

        void createScope();

        void createLocalVariable(CajetaModulePtr module, FieldPtr field);

        void setLocalVariable(CajetaModulePtr module, string name, llvm::Value* value);

        virtual void generateCode();

        static map<string, MethodPtr>& getArchive();

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled);

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled);

        static string buildGeneric(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled);

        static string buildGeneric(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled);

        static MethodPtr create(CajetaModulePtr module,
            string& name,
            CajetaTypePtr returnType,
            vector<FormalParameterPtr> parameters,
            BlockPtr block,
            CajetaClassPtr parent);

        static MethodPtr create(CajetaModulePtr module,
            string name,
            CajetaTypePtr returnType,
            CajetaClassPtr parent);
    };
}


