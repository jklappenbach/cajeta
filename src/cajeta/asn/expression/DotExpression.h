//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

using namespace std;

namespace cajeta {

    class CajetaClass;
    typedef std::shared_ptr<CajetaClass> CajetaClassPtr;

    class DotExpression : public Expression {
        string identifier;
        // Position of the IDENTIFIER token, not of the expression. The node's own
        // sourceLine/Column point at the lhs (`person` in `person.address`), but a
        // field reference must anchor on the name the developer Ctrl-clicks — the
        // IDE resolves a reference by the offset under the caret. 0 when the rhs is
        // not an identifier form. (ide-symbol-index 2.1.6)
        int idLine = 0;
        int idColumn = 0;
    public:
        DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        const string& getIdentifier() const { return identifier; }

        void resolveTypes(CajetaModulePtr module) override;

        // Emit an xref reference edge for this field access, targeting the class
        // that DECLARES the field (which for an inherited field is not the
        // receiver's class). See DotExpression.cpp.
        void recordFieldXref(const CajetaClassPtr& owner);

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