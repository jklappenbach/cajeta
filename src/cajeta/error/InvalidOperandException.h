//
// Created by James Klappenbach on 3/30/23.
//

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

