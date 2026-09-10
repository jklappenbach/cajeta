// Exception for an assignment whose left and right operand types are incompatible.

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

