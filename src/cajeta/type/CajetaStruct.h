//
// CajetaStruct — retained as an empty CajetaClass subclass (P7.6).
//
// Under the unified-class model (Q5: `struct` keyword is a transitional
// alias for `class`), visitStructDeclaration produces a CajetaClass, so
// no instance is ever a CajetaStruct anymore. The type is kept as a
// header-only stub so that lingering `dynamic_pointer_cast<CajetaStruct>`
// expressions in the compiler compile and return null (dead branches
// that will be cleaned up in a future sweep). The .cpp file is gone.
// Once every dispatch site is rewritten to drop the CajetaStruct branch,
// this header retires too.
//

#pragma once

#include "CajetaClass.h"

namespace cajeta {

    class CajetaStruct;
    typedef shared_ptr<CajetaStruct> CajetaStructPtr;

    class CajetaStruct : public CajetaClass {
    public:
        CajetaStruct(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaStruct(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }
    };

} // namespace cajeta
