//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"

namespace cajeta {

    class CreatorRest : public AbstractSyntaxNode {
    protected:
        // Target type set by the parent NewExpression before generateCode runs.
        // For ClassCreatorRest this is the struct type; for ArrayCreatorRest the
        // element type.
        CajetaTypePtr targetType;
        // NRVO target: when set, ClassCreatorRest constructs the instance
        // directly into this caller-provided slot (the sret return pointer)
        // instead of allocating its own — zero-copy value returns. See
        // docs/specification/lang/ValueReturns.md.
        llvm::Value* nrvoTarget = nullptr;
    public:
        CreatorRest(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        void setTargetType(CajetaTypePtr t) { targetType = t; }
        void setNrvoTarget(llvm::Value* t) { nrvoTarget = t; }

        static shared_ptr<CreatorRest> fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token);
    };
    typedef shared_ptr<CreatorRest> CreatorRestPtr;

    class ClassCreatorRest : public CreatorRest {
        vector<MethodCallParameter> parameters;
        // P2a: when true, generateCode emits an entry-block alloca + vtable
        // init + ctor call. When false, the legacy malloc + memset + vtable
        // init + ctor call path. NewExpression propagates this from its
        // own stackAlloc flag before invoking generateCode.
        bool stackAlloc = false;
    public:
        void setStackAlloc(bool v) { stackAlloc = v; }
        // Synthetic construction (collection-literals §2): build a ctor-call
        // rest from a pre-assembled argument list, no parse context. Used to
        // lower a target-typed collection literal `ArrayList<int32> xs =
        // [1,2,3]` into `heap ArrayList<int32>([1,2,3])`.
        ClassCreatorRest(vector<MethodCallParameter> params, antlr4::Token* token)
            : CreatorRest(token), parameters(std::move(params)) { }
        ClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            if (ctx->arguments()->parameterList()) {
                for (auto& ctxParameterEntry: ctx->arguments()->parameterList()->parameterEntry()) {
                    MethodCallParameter entry;
                    entry.expression = Expression::fromContext(ctxParameterEntry->expression());
                    // Only set label if a parameterLabel actually exists. Reading
                    // ctxParameterEntry->getText() captures the entire entry text
                    // (including the expression), which would make every positional
                    // arg look labeled and route through the labeled-ctor resolver
                    // — silently failing the lookup.
                    if (ctxParameterEntry->parameterLabel()) {
                        entry.label = ctxParameterEntry->parameterLabel()->getText();
                    }
                    // Caller-side `#x` transfer (Phase 1 of #68).
                    if (ctxParameterEntry->REFERENCE()) {
                        entry.callerTransferred = true;
                    }
                    parameters.push_back(entry);
                }
            }
        }

        // Read-only access to the constructor-call argument list, exposed so
        // TPL-7 diamond inference can inspect arg types without re-evaluating
        // expressions.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        // 7.2.4 — ctor args are private, same split as MethodCallExpression.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            for (auto& p : parameters) {
                if (p.expression) fn(p.expression);
            }
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        // xref-lint-emission-gap 4.2.3 — the constructor twin of
        // MethodCallExpression::resolveTypes. Walks the ctor args (which are
        // in `parameters`, not `children`) and records the constructor edge,
        // so Ctrl-click on `heap Derived(7)` lands on Derived's constructor
        // under lint and not only in a build.
        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class ArrayCreatorRest : public CreatorRest {
    private:
        // Total `[]` pairs in the creator (e.g. `new T[2][3][]` has 3). Equals the
        // nesting depth of the resulting array type. children.size() is the number
        // of levels with explicit sizes — the rest are left null after allocation.
        int totalBracketPairs;
    public:
        ArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            totalBracketPairs = static_cast<int>(ctx->LBRACK().size());
            for (auto& expressionContext: ctx->expression()) {
                children.push_back(Expression::fromContext(expressionContext));
            }
        }

        // Frame-arena (U3): when set, the outer array header is bump-allocated
        // from the frame arena (no malloc, no live-set). NewExpression propagates
        // this from its own arenaEligible flag, which the escape pre-pass sets only
        // for a non-escaping single-dimension primitive-element array local.
        bool arenaEligible = false;
    public:
        void setArenaEligible(bool v) { arenaEligible = v; }

        int getTotalBracketPairs() const { return totalBracketPairs; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code