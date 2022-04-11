//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <CajetaParser.h>

namespace cajeta {
    struct Expression {
        static Expression* create(CajetaParser::ExpressionContext* ctxExpression);
    };
}
