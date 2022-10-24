//
// Created by James Klappenbach on 10/6/22.
//

#include "cajeta/ast/Identifier.h"

namespace cajeta {
    Identifier toIdentifier(string str) {
        if (str == "module") {
            return MODULE;
        } else if (str == "require") {
            return REQUIRE;
        } else if (str == "exports") {
            return EXPORTS;
        } else if (str == "opens") {
            return OPENS;
        } else if (str == "to") {
            return TO;
        } else if (str == "uses") {
            return USES;
        } else if (str == "provides") {
            return PROVIDES;
        } else if (str == "with") {
            return WITH;
        } else if (str == "transitive") {
            return TRANSITIVE;
        } else if (str == "yield") {
            return YIELD;
        } else if (str == "sealed") {
            return SEALED;
        } else if (str == "permits") {
            return PERMITS;
        } else if (str == "record") {
            return RECORD;
        } else if (str == "var") {
            return VAR;
        } else {
            return UNKNOWN;
        }
    }
}
