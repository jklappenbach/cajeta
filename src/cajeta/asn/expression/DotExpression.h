//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

using namespace std;

namespace cajeta {

    class StructureProperty;
    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

    class DotExpression : public Expression {
        string identifier;
        // view v1.1 element arrays (specs/view-element-arrays-spec.md):
        // when set, generateCode on an element-array view field returns the
        // raw i8* to the field's u32 count prefix instead of throwing the
        // bare-read error. One-shot — cleared on use. Set only by the
        // callers that consume the prefix directly: ArrayIndexExpression
        // (f[i]) and MethodCallExpression (f.count()).
        bool elementArrayPrefixMode = false;
    public:
        DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        const string& getIdentifier() const { return identifier; }

        void setElementArrayPrefixMode(bool m) { elementArrayPrefixMode = m; }

        // Non-null iff this dot names a view element-array field (`V[]` /
        // `String[]`) on a view-typed receiver — the gate the f[i] and
        // f.count() special paths key on. Resolves the receiver's type if
        // needed.
        StructurePropertyPtr resolveViewElementArrayProperty(
            CajetaModulePtr module);

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