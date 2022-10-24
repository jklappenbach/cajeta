//
// Created by James Klappenbach on 2/20/22.
//

#include "cajeta/type/Field.h"
#include "cajeta/type/CajetaType.h"

namespace cajeta {
    list<Field*> Field::fromContext(CajetaParser::FieldDeclarationContext* ctx) {
        list<Field*> fields;
        CajetaType* type = CajetaType::fromContext(ctx->typeType());

        for (auto & ctxVariableDeclarator : ctx->variableDeclarators()->variableDeclarator()) {
            string name = ctxVariableDeclarator->variableDeclaratorId()->identifier()->getText();
// TODO: Finish me!            Field* field = new Field(name, )
        }
        return fields;
    }
}
