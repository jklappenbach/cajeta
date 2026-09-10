//
// Created by James Klappenbach on 4/14/23.
//

#pragma once

#include "Expression.h"

using namespace std;

namespace cajeta {

    class CajetaClass;
    typedef std::shared_ptr<CajetaClass> CajetaClassPtr;

    class StructureProperty;
    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

    class DotExpression : public Expression {
        string identifier;
        // Position of the IDENTIFIER token, not of the node, which is at the lhs.
        int idLine = 0;
        int idColumn = 0;
        // When set, generateCode on an element-array view field returns the raw i8*
        // to its u32 count prefix. One-shot: cleared on use, set only by f[i]/count().
        bool elementArrayPrefixMode = false;
        // Stashed by a prefix-mode generateCode; null/-1 unless a descriptor view.
        llvm::Value* earrDataBase = nullptr;
        llvm::Value* earrTable = nullptr;
        int earrSlot = -1;
        shared_ptr<class CajetaView> earrViewType;
    public:
        llvm::Value* getEarrDataBase() const { return earrDataBase; }
        llvm::Value* getEarrTable() const { return earrTable; }
        int getEarrSlot() const { return earrSlot; }
        const shared_ptr<class CajetaView>& getEarrViewType() const {
            return earrViewType;
        }
        DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        const string& getIdentifier() const { return identifier; }

        void setElementArrayPrefixMode(bool m) { elementArrayPrefixMode = m; }

        // Non-null iff this dot names an element-array field on a view receiver.
        StructurePropertyPtr resolveViewElementArrayProperty(
            CajetaModulePtr module);

        void resolveTypes(CajetaModulePtr module) override;

        // Emit an xref edge targeting the DECLARING class, not the receiver's.
        void recordFieldXref(const CajetaClassPtr& owner);

        llvm::Value* generateCode(CajetaModulePtr module) override;

        // The dotted path for `person.address.city`, "" if it is not all identifiers.
        static string buildPath(const ExpressionPtr& expr);

        // Apply `llvm.bswap.iN` to `v` when the receiver's struct declares a non-host
        // endianness: after a field load, before a field store, so wire order stays.
        static llvm::Value* maybeBswap(CajetaModulePtr module, llvm::Value* v,
                                        const ExpressionPtr& receiver);
    };

} // code