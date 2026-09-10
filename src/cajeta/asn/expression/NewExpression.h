// Created by James Klappenbach on 4/19/23.

#pragma once

#include "Expression.h"
#include "CreatorRest.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaConstantType.h"
#include "../../compile/CajetaModule.h"

namespace cajeta {

    class NewExpression : public Expression {
        string package;
        string typeName;
        // Empty for non-templated and for diamond forms, which TPL-7 infers.
        vector<CajetaTypePtr> typeArguments;
        bool isDiamond = false;
        CreatorRestPtr creatorRest;
        // Bound in the parse walk; the substitution stack is gone by resolveTypes.
        CajetaTypePtr boundElementType;
        // `stack X(args)`: an entry-block alloca instead of a malloc.
        bool stackAlloc = false;
        // `shared X(args)`: device-only workgroup-shared placement (addrspace 3).
        bool sharedAlloc = false;
        // NRVO sret slot: the instance is built into the caller's return slot.
        llvm::Value* nrvoTarget = nullptr;
        // Set by Method::computeArenaEligibility: bump-allocate from the arena.
        bool arenaEligible = false;
    public:
        void setArenaEligible(bool v) { arenaEligible = v; }
        bool isArenaEligible() const { return arenaEligible; }
        void setStackAlloc(bool v) { stackAlloc = v; }
        bool getStackAlloc() const { return stackAlloc; }
        void setSharedAlloc(bool v) { sharedAlloc = v; }
        bool getSharedAlloc() const { return sharedAlloc; }
        void setNrvoTarget(llvm::Value* t) { nrvoTarget = t; }
        llvm::Value* getNrvoTarget() const { return nrvoTarget; }

        /// The ClassCreatorRest or ArrayCreatorRest, exposed so the device
        /// lowerer can read a `shared T[N]` size operand without host codegen.
        const CreatorRestPtr& getCreatorRest() const { return creatorRest; }
        const string& getTypeName() const { return typeName; }

        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            if (creatorRest) fn(creatorRest);
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        NewExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::New; }

        /// Builds the creator from its parse context: leaf type name, package,
        /// template arguments (or diamond), creator-rest. Runs in the parse walk.
        NewExpression(CajetaParser::CreatorContext* creatorContext, antlr4::Token* token) : Expression(token) { exprKind = ExprKind::New;
            antlr4::Token* createdTypeToken = nullptr;
            if (creatorContext->createdName() != nullptr) {
                if (creatorContext->createdName()->primitiveType()) {
                    typeName = creatorContext->createdName()->primitiveType()->getText();
                } else if (!creatorContext->createdName()->identifier().empty()) {
                    int count = creatorContext->createdName()->identifier().size();
                    int n = 0;
                    for (auto& identifierPart: creatorContext->createdName()->identifier()) {
                        if (n++ == count - 1) {
                            typeName = identifierPart->getText();
                            createdTypeToken = identifierPart->getStart();
                        } else {
                            package.append(identifierPart->getText());
                        }
                    }
                    auto tads = creatorContext->createdName()->typeArgumentsOrDiamond();
                    if (!tads.empty()) {
                        auto* lastTad = tads.back();
                        if (auto* targs = lastTad->typeArguments()) {
                            for (auto* targ : targs->typeArgument()) {
                                if (targ->integerLiteral() != nullptr) {
                                    typeArguments.push_back(CajetaConstantType::of(
                                        CajetaConstantType::parseLiteral(
                                            targ->integerLiteral())));
                                    continue;
                                }
                                // The grammar puts a wildcard's BOUND, if it
                                // has one, in typeType().
                                if (targ->QUESTION() != nullptr) {
                                    if (!CajetaType::wildcardsEnabled()) {
                                        throw "wildcard type arguments not supported in v1";
                                    }
                                    CajetaTypePtr bound = nullptr;
                                    if (targ->typeType() != nullptr) {
                                        bound = CajetaType::fromContext(targ->typeType(), nullptr);
                                        if (!bound) {
                                            throw "unresolved wildcard bound type";
                                        }
                                    }
                                    CajetaTypePtr wild;
                                    if (targ->EXTENDS() != nullptr) {
                                        wild = CajetaType::wildcardSentinelExtends(bound);
                                    } else if (targ->SUPER() != nullptr) {
                                        wild = CajetaType::wildcardSentinelSuper(bound);
                                    } else {
                                        wild = CajetaType::wildcardSentinel();
                                    }
                                    if (!wild) {
                                        throw "wildcard sentinel construction failed";
                                    }
                                    typeArguments.push_back(wild);
                                    continue;
                                }
                                if (!targ->typeType()) {
                                    throw "wildcard type arguments not supported in v1";
                                }
                                // module=nullptr makes fromContext fall back to
                                // getActiveModule(), honoring outer substitutions.
                                CajetaTypePtr argType = CajetaType::fromContext(targ->typeType(), nullptr);
                                if (!argType) {
                                    throw "unresolved template argument in `new`";
                                }
                                typeArguments.push_back(argType);
                                if (targ->REFERENCE() != nullptr) {
                                    throw Exception(
                                        "`#` on a type argument is retired: "
                                        "ownership is per-call under "
                                        "title-tracking (specs/title-tracking-"
                                        "spec.md §8.1) — spell it at the store "
                                        "site and drop the `#` from `"
                                        + argType->toCanonical() + "`",
                                        "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
                                }
                            }
                        } else {
                            // Diamond: '<' '>' with no inner typeArguments match.
                            isDiamond = true;
                        }
                    }
                }
            }
            creatorRest = CreatorRest::fromContext(creatorContext, token);
            // The substitution stack is still live here, in the parse walk.
            if (!typeName.empty()) {
                if (auto am = CajetaModule::getActiveModule()) {
                    boundElementType = am->lookupTypeParameter(typeName);
                }
            }
            recordCreatedTypeXref(createdTypeToken);
        }

        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        bool getIsDiamond() const { return isDiamond; }

        /// Synthetic construction: these populate what a parsed creator would
        /// carry, so a collection literal becomes a creator with no parse context.
        void setTypeName(string name) { typeName = std::move(name); }
        void setPackage(string pkg) { package = std::move(pkg); }
        void setTypeArguments(vector<CajetaTypePtr> args) { typeArguments = std::move(args); }
        void setCreatorRest(CreatorRestPtr rest) { creatorRest = std::move(rest); }

        /// Resolves the created type, its template arguments, and the rest.
        void resolveTypes(CajetaModulePtr module) override;
        /// Emits the allocation and constructor call; returns the instance.
        llvm::Value* generateCode(CajetaModulePtr module) override;

    private:
        /// Records the created type as a type-reference edge at `tok`. Gated on
        /// xref::captureEnabled(); a miss or a throw records no edge and no error.
        void recordCreatedTypeXref(antlr4::Token* tok);

        /// Records the constructor call edge under lint, where the creator's
        /// own targetType is not yet set. Unique-arity match only.
        void recordConstructorCallXref(const CajetaTypePtr& type,
                                       CajetaModulePtr module);
    };

} // code