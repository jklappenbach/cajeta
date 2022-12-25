//
// Created by James Klappenbach on 2/20/22.
//

#include "Field.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/asn/VariableDeclarator.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/Scope.h"

namespace cajeta {
    list<Field*> Field::fromContext(CajetaParser::FieldDeclarationContext* ctx, CajetaModule* module) {
        list<Field*> fields;
        CajetaType* type = CajetaType::fromContext(ctx->typeType(), module);

        for (auto& ctxVariableDeclarator: ctx->variableDeclarators()->variableDeclarator()) {
            string name = ctxVariableDeclarator->variableDeclaratorId()->identifier()->getText();
            fields.push_back(new Field(name, type));
        }
        return fields;
    }

    /**
     * I think this is how variable allocations should be handled, either way: the initializer will
     * either create a stack origin, or one off the heap created by malloc
     * @param module
     * @return
     */
    llvm::Value* Field::getOrCreateAllocation(CajetaModule* module) {
        if (!origin) {
            if (type->getStructType() == STRUCT_TYPE_PRIMITIVE) {
                origin = module->getBuilder()->CreateAlloca(type->getLlvmType(), nullptr, getHierarchicalName());
            } else {
                origin = module->getBuilder()->CreateAlloca(type->getLlvmType()->getPointerTo(), nullptr, getHierarchicalName());
            }
            module->getValueStack().push_back(origin);
            initializer->generateCode(module);
            module->getValueStack().pop_back();
        }
        return origin;
    }

    llvm::Value* Field::createLoad(CajetaModule* module) {
        if (!origin) {
            load = getOrCreateAllocation(module);
        }
        if (type->getStructType() == STRUCT_TYPE_PRIMITIVE) {
            return load = module->getBuilder()->CreateLoad(type->getLlvmType(), origin);
        } else {
            return load = module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), origin);
        }
    }

    void Field::onDelete(CajetaModule* module, Scope* scope) {
        if (type->getStructType() != STRUCT_TYPE_PRIMITIVE) {
            MemoryManager::getInstance(module)->createFreeInstruction(createLoad(module),
                module->getBuilder()->GetInsertBlock());
        }
    }
}
