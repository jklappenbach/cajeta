//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

using namespace std;

namespace cajeta {

    class DotExpression : public Expression {
        string identifier;
    public:
        DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // cajeta