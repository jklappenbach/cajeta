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
        CajetaTypePtr targetType;
        // When set, ClassCreatorRest constructs directly into this caller-provided slot.
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
        // When true, generateCode emits an entry-block alloca instead of malloc + memset.
        bool stackAlloc = false;
    public:
        void setStackAlloc(bool v) { stackAlloc = v; }
        // Synthetic construction from a pre-assembled argument list, no parse context.
        ClassCreatorRest(vector<MethodCallParameter> params, antlr4::Token* token)
            : CreatorRest(token), parameters(std::move(params)) { }
        ClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            if (ctx->arguments()->parameterList()) {
                for (auto& ctxParameterEntry: ctx->arguments()->parameterList()->parameterEntry()) {
                    MethodCallParameter entry;
                    entry.expression = Expression::fromContext(ctxParameterEntry->expression());
                    // Label only when a parameterLabel exists: getText() on the entry
                    // captures the expression too, so a positional arg would look labeled.
                    if (ctxParameterEntry->parameterLabel()) {
                        entry.label = ctxParameterEntry->parameterLabel()->getText();
                    }
                    if (ctxParameterEntry->REFERENCE()) {
                        entry.callerTransferred = true;
                    }
                    parameters.push_back(entry);
                }
            }
        }

        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            for (auto& p : parameters) {
                if (p.expression) fn(p.expression);
            }
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        // The constructor twin of MethodCallExpression::resolveTypes: walks the ctor args
        // (which live in `parameters`, not `children`) and records the constructor edge.
        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class ArrayCreatorRest : public CreatorRest {
    private:
        // Total `[]` pairs; children.size() counts only the levels with explicit sizes.
        int totalBracketPairs;
    public:
        ArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            totalBracketPairs = static_cast<int>(ctx->LBRACK().size());
            for (auto& expressionContext: ctx->expression()) {
                children.push_back(Expression::fromContext(expressionContext));
            }
        }

        // When set, the outer array header is bump-allocated from the frame arena, which
        // the escape pre-pass allows only for a non-escaping primitive-element local.
        bool arenaEligible = false;
    public:
        void setArenaEligible(bool v) { arenaEligible = v; }

        int getTotalBracketPairs() const { return totalBracketPairs; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code