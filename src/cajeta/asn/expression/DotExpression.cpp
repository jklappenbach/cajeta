//
// Created by James Klappenbach on 4/14/23.
//

#include "DotExpression.h"
#include "../../compile/CajetaModule.h"

namespace cajeta {
    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {
        identifier = ctx->identifier()->getText();
    }

    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        llvm::Value* value = children[0]->generateCode(module);
//        CajetaStructurePtr structure = static_pointer_cast<CajetaStructure>(module->getFieldStack().back()->getType());
//        module->getFieldStack().pop_back();
//        StructurePropertyPtr property = structure->getProperties()[identifier];
        // TODO: We shouldn't be using structureField, we should be creating a PropertyField from the property
        // TODO: We should automatically create an entry in our scope for a structure hierarchy.  These can be lazily loaded, but should be available for reference
        // TODO: and deallocation here.
        module->getAsnStack().pop_back();

        return nullptr;
//        module->getFieldStack().push_back(structureField);
//        if (structureField == nullptr) {
//            throw "identifier did not map to field";
//        }
//        return module->getBuilder()->CreateStructGEP(structure->getLlvmType(),
//                                                     value,
//                                                     structureField->getOrder(),
//                                                     identifier);
    }

} // cajeta