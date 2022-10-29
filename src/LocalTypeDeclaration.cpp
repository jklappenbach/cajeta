//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/ast/LocalTypeDeclaration.h"

namespace cajeta {
    LocalTypeDeclaration* LocalTypeDeclaration::fromContext(CajetaParser::LocalTypeDeclarationContext* ctx) {
//        CajetaType* type = CajetaType::fromContext(ctx->);
        return new LocalTypeDeclaration(nullptr);
    }
}