//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/ast/LocalTypeDeclaration.h"

namespace cajeta {
    LocalTypeDeclaration* fromContext(CajetaParser::LocalTypeDeclarationContext* ctx) {
        CajetaType* type = CajetaType::fromContext(ctx);
        LocalTypeDeclaration* result = new LocalTypeDeclaration(type);

    }
}