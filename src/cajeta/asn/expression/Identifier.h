//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {
    class IdentifierExpression : public Expression {
    private:
        string identifier;
    public:
        IdentifierExpression(CajetaParser::IdentifierContext* ctx, bool primary) : Expression(ctx->getStart()) {
            this->primary = primary;
            identifier = ctx->getText();
        }

        IdentifierExpression(bool primary, CajetaParser::IdentifierContext* ctx) : Expression(primary,
            ctx->getStart()) {
            identifier = ctx->getText();
        }

        string& getTextValue() { return identifier; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
} // cajeta