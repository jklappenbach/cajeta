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

        const string& getIdentifier() const { return identifier; }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;

        // Build the dotted-path string for a chain like `person.address.city`.
        // Returns "" if the chain bottoms out at something that isn't a named
        // identifier (e.g. a method call result). Used by the borrow checker
        // to track moved paths and reject reads through them.
        static string buildPath(const ExpressionPtr& expr);
    };

} // code