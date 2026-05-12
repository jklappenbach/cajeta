//
// Java-style nested array. A `T[]` value is a `ptr` to a header struct laid out as
// `{ i64 size, [0 x T] data }` — one heap allocation per array. `T[][]` wraps another
// CajetaArray whose element type is itself a CajetaArray; multi-dim arrays are nested
// rather than flattened, matching Java semantics.
//

#pragma once

#include "CajetaClass.h"
#include "cajeta/field/Field.h"
#include "cajeta/method/Method.h"
#include "Modifiable.h"
#include <stdio.h>
#include <vector>

namespace cajeta {
    class CajetaArray : public CajetaClass {
    private:
        CajetaTypePtr elementType;
    public:
        // Index of the size field in the header struct's GEP layout.
        static constexpr unsigned SIZE_FIELD_INDEX = 0;
        // Index of the inline data array `[0 x T]` in the header struct.
        static constexpr unsigned DATA_FIELD_INDEX = 1;

        CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType);

        CajetaTypeFlags getTypeFlags() override { return ARRAY_TYPE_ID; }

        // The element type for one level of indexing. For `T[][]` this returns the
        // CajetaArray for `T[]`; the next level of unwrapping happens via that.
        CajetaTypePtr getElementType() { return elementType; }

        // The LLVM type used inside the header's `[0 x T]`. Equal to elementType's
        // llvm type for value-type elements, or `ptr` when elementType is itself an
        // array or other reference type.
        llvm::Type* getElementLlvmType(llvm::LLVMContext* ctx) const;
    };
    typedef shared_ptr<CajetaArray> CajetaArrayPtr;
}
