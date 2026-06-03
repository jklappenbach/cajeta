//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"
#include "CreatorRest.h"
#include "../../type/CajetaType.h"
#include "../../compile/CajetaModule.h"

namespace cajeta {

    class NewExpression : public Expression {
        string package;
        string typeName;
        // Resolved template arguments for `new Box<int32>(...)`. Empty for
        // non-templated `new Foo(...)` and for diamond-form `new Box<>(...)`
        // (TPL-7 fills these in by inference at codegen time).
        vector<CajetaTypePtr> typeArguments;
        bool isDiamond = false;
        CreatorRestPtr creatorRest;
        // Captured at construction-time (parse walk) when `typeName` matches
        // a template parameter active on the module's substitution stack.
        // Required because resolveTypes/generateCode run later (after the
        // walk) when the stack is gone — without this we'd resolve `new T[N]`
        // inside an instantiated template's body to null and segfault. See
        // CajetaModule::pushTypeSubstitution / lookupTypeParameter.
        CajetaTypePtr boundElementType;
        // P2a: `stack ClassName(args)` routes through NewExpression with
        // this flag set so ClassCreatorRest emits an entry-block alloca
        // instead of a malloc. `heap ClassName(args)` and bare `new
        // ClassName(args)` keep the default (heap).
        bool stackAlloc = false;
        // `shared ClassName(args)` / `shared T[N]` — GPU workgroup-shared
        // placement (NV addrspace 3). Device-only: the NVPTX kernel lowerer
        // recognizes this on a kernel-local's initializer and emits a per-block
        // addrspace(3) global; the host generateCode path rejects it.
        bool sharedAlloc = false;
        // NRVO sret slot, set by ReturnStatement when this `stack X(...)` is
        // the returned expression of a value-returning method. Forwarded to
        // the CreatorRest at generateCode so the instance is built directly
        // into the caller's return slot. See cajeta-docs/stdlib/ValueReturns.md.
        llvm::Value* nrvoTarget = nullptr;
    public:
        void setStackAlloc(bool v) { stackAlloc = v; }
        bool getStackAlloc() const { return stackAlloc; }
        void setSharedAlloc(bool v) { sharedAlloc = v; }
        bool getSharedAlloc() const { return sharedAlloc; }
        void setNrvoTarget(llvm::Value* t) { nrvoTarget = t; }
        llvm::Value* getNrvoTarget() const { return nrvoTarget; }

        // The creator-rest (ClassCreatorRest or ArrayCreatorRest), exposed so
        // the device lowerer can read a `shared T[N]` array creation's size
        // operand without going through host codegen.
        const CreatorRestPtr& getCreatorRest() const { return creatorRest; }
        const string& getTypeName() const { return typeName; }

        NewExpression(antlr4::Token* token) : Expression(token) { }

        NewExpression(CajetaParser::CreatorContext* creatorContext, antlr4::Token* token) : Expression(token) {
            if (creatorContext->createdName() != nullptr) {
                if (creatorContext->createdName()->primitiveType()) {
                    typeName = creatorContext->createdName()->primitiveType()->getText();
                } else if (!creatorContext->createdName()->identifier().empty()) {
                    int count = creatorContext->createdName()->identifier().size();
                    int n = 0;
                    for (auto& identifierPart: creatorContext->createdName()->identifier()) {
                        if (n++ == count - 1) {
                            typeName = identifierPart->getText();
                        } else {
                            package.append(identifierPart->getText());
                        }
                    }
                    // Template arguments: createdName allows typeArgumentsOrDiamond
                    // after each identifier. v1 looks at the LAST one (applying
                    // to the leaf type); multiple levels of template args in a
                    // qualified name (e.g. `Outer<A>.Inner<B>`) are deferred.
                    auto tads = creatorContext->createdName()->typeArgumentsOrDiamond();
                    if (!tads.empty()) {
                        auto* lastTad = tads.back();
                        if (auto* targs = lastTad->typeArguments()) {
                            for (auto* targ : targs->typeArgument()) {
                                if (!targ->typeType()) {
                                    throw "wildcard type arguments not supported in v1";
                                }
                                // module=nullptr; fromContext falls back to
                                // CajetaModule::getActiveModule() so any
                                // outer-template substitution stack is honored
                                // (e.g. `new Box<T>()` inside a template body
                                // where T was bound by the instantiation).
                                CajetaTypePtr argType = CajetaType::fromContext(targ->typeType(), nullptr);
                                if (!argType) {
                                    throw "unresolved template argument in `new`";
                                }
                                typeArguments.push_back(argType);
                            }
                        } else {
                            // Diamond form: typeArgumentsOrDiamond has '<' '>'
                            // tokens but no inner typeArguments rule match.
                            // TPL-7 handles inference from constructor args.
                            isDiamond = true;
                        }
                    }
                }
            }
            creatorRest = CreatorRest::fromContext(creatorContext, token);
            // Capture template-parameter binding while the substitution
            // stack is still live (we're in the parse walk now). See
            // boundElementType comment above.
            if (!typeName.empty()) {
                if (auto am = CajetaModule::getActiveModule()) {
                    boundElementType = am->lookupTypeParameter(typeName);
                }
            }
        }

        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        bool getIsDiamond() const { return isDiamond; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code