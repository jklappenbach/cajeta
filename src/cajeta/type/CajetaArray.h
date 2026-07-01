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
        // -1 => heap reference `T[]` (a pointer to a `{i64 size, [0 x T]}`
        // header). >= 0 => fixed-size inline array `T[N]`: N contiguous
        // elements stored INLINE in the enclosing object (no pointer, no
        // heap), used for SSO-style inline buffers. The canonical name
        // encodes N (`int8[64]` vs `int8[]`) so the two are distinct types.
        int32_t fixedLength = -1;
    public:
        // Index of the size field in the header struct's GEP layout.
        static constexpr unsigned SIZE_FIELD_INDEX = 0;
        // Index of the inline data array `[0 x T]` in the header struct.
        static constexpr unsigned DATA_FIELD_INDEX = 1;

        CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType,
                    int32_t fixedLength = -1);

        CajetaTypeFlags getTypeFlags() override { return ARRAY_TYPE_ID; }

        // Fixed inline length (>= 0) or -1 for a heap reference. See the field
        // comment above.
        int32_t getFixedLength() const { return fixedLength; }
        bool isInlineArray() const { return fixedLength >= 0; }

        // The LLVM type of the inline storage for a `T[N]` field: `[N x T]`.
        // Only valid when isInlineArray(); used by the class field-layout and
        // the inline element-access GEP.
        llvm::Type* getInlineLlvmType(llvm::LLVMContext* ctx) const;

        // The element type for one level of indexing. For `T[][]` this returns the
        // CajetaArray for `T[]`; the next level of unwrapping happens via that.
        CajetaTypePtr getElementType() { return elementType; }

        // The LLVM type used inside the header's `[0 x T]`. Equal to elementType's
        // llvm type for value-type elements, or `ptr` when elementType is itself an
        // array or other reference type.
        llvm::Type* getElementLlvmType(llvm::LLVMContext* ctx) const;

        // U6.4.1 — build (intern) the array's `{ i64 size, [0 x T] data }` struct
        // in `ctx` from the (immutable) element type + fixed length. Context-
        // parameterized so the frozen-stdlib path can rebuild it in a thread's own
        // context (U6.4.2); the ctor calls it with the home module's context.
        llvm::Type* buildLlvmType(llvm::LLVMContext* ctx) const;

        // U6.4.2 — when frozen and this thread's binding table is empty, rebuild
        // the struct in the thread's current LLVM context. Inert while not frozen
        // (delegates to the base, which returns the inline-bound type).
        llvm::Type* getLlvmType() override;
    };
    typedef shared_ptr<CajetaArray> CajetaArrayPtr;
}
