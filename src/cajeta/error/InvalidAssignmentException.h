//
// Created by James Klappenbach on 3/30/23.
//

#pragma once

#include "Exception.h"
#include "../type/CajetaType.h"

using namespace std;

namespace cajeta {
    class InvalidAssignmentException : public Exception {
    private:
        string lhsName;
        string rhsName;
        CajetaTypeFlags lhsFlags;
        CajetaTypeFlags rhsFlags;
    public:
        InvalidAssignmentException(string lhsName, CajetaTypeFlags lhsFlags, string rhsName, CajetaTypeFlags rhsFlags) {
            this->lhsFlags = lhsFlags;
            this->rhsFlags = rhsFlags;
        }
    };
}

