//
// Created by James Klappenbach on 2/20/22.
//

#include "cajeta/type/Field.h"
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

    llvm::Value* Field::getOrCreateStackAllocation(CajetaModule* module) {
        if (!allocation) {
            allocation = module->getBuilder()->CreateAlloca(type->getLlvmType(), nullptr, name);
            if (initializer) {
                module->setInitializerType(type);
                module->pushCurrentValue(allocation);
                initializer->generateCode(module);
            }
        }
        return allocation;
    }

    /**
     * I think this is how variable allocations should be handled, either way: the initializer will
     * either create a stack allocation, or one off the heap created by malloc
     * @param module
     * @return
     */
    llvm::Value* Field::getOrCreateAllocation(CajetaModule* module) {
        if (!allocation) {
            allocation = module->getBuilder()->CreateAlloca(type->getLlvmType(), nullptr, name);
            module->getBuilder()->CreateStore(initializer->generateCode(module), allocation);
        }
        return allocation;
    }

    void Field::onDelete(CajetaModule* module, Scope* scope) {
        if (type->getStructType() != STRUCT_TYPE_PRIMITIVE) {
            if (!reference) {
                llvm::LoadInst* loadInst = module->getBuilder()->CreateLoad(type->getLlvmType(), allocation, "");
                MemoryManager::getInstance(module)->createFreeInstruction(loadInst,
                    module->getBuilder()->GetInsertBlock());
            }
        }
    }
}
