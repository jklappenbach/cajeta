//
// Created by James Klappenbach on 11/6/22.
//

#pragma once

#include "antlr4-runtime.h"
#include <string>
#include <cajeta/exception/Exception.h>
#include <cajeta/asn/AbstractSyntaxNode.h>

using namespace std;

namespace cajeta {
    enum LoggingLevel {
        INFO = 0, WARNING = 1, ERROR = 2, FATAL = 3
    };

    class CajetaLogger {
    public:
        static void log(LoggingLevel level,
            antlr4::ParserRuleContext* ctx,
            string sourcePath,
            string errorId,
            string message);

        static void log(LoggingLevel level,
            CajetaModule* cajetaModule,
            AbstractSyntaxNode* node,
            Exception& e);

    };

} // cajeta