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

        // Apply an `llvm.bswap.iN` intrinsic to `v` if the receiver's struct
        // type carries a non-host endianness annotation. Used by both field
        // load (after read) and field store (before write) so the value seen
        // by the host is in host order while the in-buffer bytes match the
        // declared wire order. Floats and i8 pass through unchanged today.
        static llvm::Value* maybeBswap(CajetaModulePtr module, llvm::Value* v,
                                        const ExpressionPtr& receiver);
    };

} // code