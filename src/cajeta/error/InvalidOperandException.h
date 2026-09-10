// Exception for an operator applied to incompatible operand types.

#pragma once

#include "Exception.h"
#include "../type/CajetaType.h"

namespace cajeta {
    class InvalidOperandException : public Exception {
    private:
        CajetaTypeFlags lhsFlags;
        CajetaTypeFlags rhsFlags;
    public:
        InvalidOperandException(CajetaTypeFlags lhsFlags, CajetaTypeFlags rhsFlags) {
            this->lhsFlags = lhsFlags;
            this->rhsFlags = rhsFlags;
        }
    };
}

