//
// Created by James Klappenbach on 4/19/23.
//

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
        // into the caller's return slot. See docs/specification/lang/ValueReturns.md.
        llvm::Value* nrvoTarget = nullptr;
        // Frame-arena (U3): set by Method::computeArenaEligibility when this
        // `heap T[N]` initializes a non-escaping single-dimension primitive-element
        // array local. Propagated to ArrayCreatorRest at generateCode so the header
        // is bump-allocated from the frame arena (no malloc, no live-set, no drop).
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

        // The creator-rest (ClassCreatorRest or ArrayCreatorRest), exposed so
        // the device lowerer can read a `shared T[N]` array creation's size
        // operand without going through host codegen.
        const CreatorRestPtr& getCreatorRest() const { return creatorRest; }
        const string& getTypeName() const { return typeName; }

        // 7.2.4 — the creator-rest (and with it the ctor args / dims) is a
        // private slot, not a child.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            if (creatorRest) fn(creatorRest);
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        NewExpression(antlr4::Token* token) : Expression(token) { }

        NewExpression(CajetaParser::CreatorContext* creatorContext, antlr4::Token* token) : Expression(token) {
            // The leaf type-name token of `heap pkg.Point(...)` — the `Point`.
            // Captured so the created type can be recorded as an xref reference
            // at parse time (ide-symbol-index): lint stops before the codegen
            // pass that otherwise resolves an allocation's type, so without this
            // Ctrl-click on `heap Point(...)` would find no edge.
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
                    // Template arguments: createdName allows typeArgumentsOrDiamond
                    // after each identifier. v1 looks at the LAST one (applying
                    // to the leaf type); multiple levels of template args in a
                    // qualified name (e.g. `Outer<A>.Inner<B>`) are deferred.
                    auto tads = creatorContext->createdName()->typeArgumentsOrDiamond();
                    if (!tads.empty()) {
                        auto* lastTad = tads.back();
                        if (auto* targs = lastTad->typeArguments()) {
                            for (auto* targ : targs->typeArgument()) {
                                // A use-site `#` on a type argument carries no
                                // meaning and is rejected below with
                                // TYPE_TRANSFER_RETIRED. There is one monomorph
                                // per type; ownership is decided per call.
                                if (targ->integerLiteral() != nullptr) {
                                    // Non-type (integer) template argument —
                                    // the `N` in `new Vector<float32, 4>(...)`.
                                    typeArguments.push_back(CajetaConstantType::of(
                                        CajetaConstantType::parseLiteral(
                                            targ->integerLiteral())));
                                    continue;
                                }
                                // Wildcard arg — `?`, `? extends T`, `? super T`
                                // — in a `heap T<?>(...)` / `heap T<?>[n]`
                                // creator. Mirrors CajetaType::fromContext's
                                // wildcard branch (REFL-1.7 needs `heap
                                // Class<?>[n]` for the registry queries). The
                                // grammar puts the BOUND, if any, in typeType().
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
                                // title-tracking §8.1 (plan 7.1.1) — creator
                                // type-argument `#` is retired with the
                                // declared-type form.
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
            // Record the created type as an xref reference at its token (parse
            // time, so lint captures it). No-op unless --emit-xref is on.
            recordCreatedTypeXref(createdTypeToken);
        }

        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        bool getIsDiamond() const { return isDiamond; }

        // Synthetic construction (collection-literals §2): populate the fields a
        // parsed `heap Name<args>(...)` would carry, so a target-typed
        // collection literal can be rewritten into a real creator without a
        // parse context. resolveTypes/generateCode then re-resolve `Name` and
        // re-instantiate against `typeArguments` exactly as for the spelled form.
        void setTypeName(string name) { typeName = std::move(name); }
        void setPackage(string pkg) { package = std::move(pkg); }
        void setTypeArguments(vector<CajetaTypePtr> args) { typeArguments = std::move(args); }
        void setCreatorRest(CreatorRestPtr rest) { creatorRest = std::move(rest); }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;

    private:
        // ide-symbol-index: record the created type (`heap Point(...)`) as a
        // type-reference edge at `tok`, resolving the name scoped like
        // resolveTypes. Best-effort and gated on xref::captureEnabled(), so a
        // normal compile never runs it; a resolution miss or throw records no
        // edge rather than affecting the compile.
        void recordCreatedTypeXref(antlr4::Token* tok);
    };

} // code