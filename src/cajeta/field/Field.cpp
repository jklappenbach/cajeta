//
// Created by James Klappenbach on 2/20/22.
//

#include "Field.h"
#include "StackField.h"
#include "../type/CajetaType.h"
#include "../asn/VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../type/Scope.h"

namespace cajeta {
    Field::Field(CajetaModulePtr module, string name, llvm::AllocaInst* alloc) {
        this->module = module;
        this->name = name;
        this->type = CajetaType::of(alloc);
    }

    list<FieldPtr> Field::fromContext(CajetaParser::FieldDeclarationContext* ctx, CajetaModulePtr module) {
        list<FieldPtr> fields;
        CajetaTypePtr type = CajetaType::fromContext(ctx->typeType(), module);

        for (auto& ctxVariableDeclarator: ctx->variableDeclarators()->variableDeclarator()) {
            string name = ctxVariableDeclarator->variableDeclaratorId()->identifier()->getText();
            // TODO: Figure out how to tell if we have a stack or heap variable
            fields.push_back(make_shared<StackField>(module, name, type));
        }
        return fields;
    }
}
