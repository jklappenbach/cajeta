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
        llvm::BasicBlock* llvmBasicBlock;
        // R5-A' implicit function-body scope: at function entry codegen
        // alloca's a ptr slot here and stores `__cajeta_scope_save_top()`
        // into it. Each return path loads it back and calls
        // `__cajeta_scope_exit_to(watermark)` so every scope frame
        // pushed inside the function — implicit function-body OR any
        // explicit `scope { }` the return is inside of — gets waited
        // and popped before the ret instruction.
        llvm::AllocaInst* scopeWatermark = nullptr;
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

        llvm::Function* getLlvmFunction() { return llvmFunction; }

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


