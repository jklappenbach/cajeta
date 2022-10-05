//
// Created by James Klappenbach on 2/20/22.
//

#include "cajeta/Field.h"
#include "cajeta/Type.h"

namespace cajeta {
    list<Field*> Field::fromContext(CajetaParser::FieldDeclarationContext* ctx) {
        list<Field*> fields;
        Type* type = Type::fromContext(ctx->typeType());

        for (auto & ctxVariableDeclarator : ctx->variableDeclarators()->variableDeclarator()) {
            string name = ctxVariableDeclarator->variableDeclaratorId()->identifier()->getText();
// TODO: Finish me!            Field* field = new Field(name, )
        }
        return fields;
    }
}
