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
    public:
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