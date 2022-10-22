//
// Created by James Klappenbach on 10/6/22.
//

#pragma once

#include <string>

using namespace std;

namespace cajeta {
    enum Identifier { UNKNOWN = -1, MODULE, REQUIRE, EXPORTS, OPENS, TO, USES, PROVIDES, WITH, TRANSITIVE, YIELD, SEALED, PERMITS, RECORD, VAR };

    Identifier toIdentifier(string str);
}