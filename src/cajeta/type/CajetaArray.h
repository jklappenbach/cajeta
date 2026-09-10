// Java-style nested array: a `T[]` value is a `ptr` to `{ i64 size, [0 x T] data }`, one
// heap allocation per array. `T[][]` nests a CajetaArray of CajetaArray, not a flat block.
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
        // -1 => heap reference `T[]`. >= 0 => fixed-size inline array `T[N]`, N elements
        // stored INLINE in the enclosing object; the canonical encodes N, so they differ.
        int32_t fixedLength = -1;
    public:
        static constexpr unsigned SIZE_FIELD_INDEX = 0;
        static constexpr unsigned DATA_FIELD_INDEX = 1;

        CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType,
                    int32_t fixedLength = -1);

        CajetaTypeFlags getTypeFlags() override { return ARRAY_TYPE_ID; }

        int32_t getFixedLength() const { return fixedLength; }
        bool isInlineArray() const { return fixedLength >= 0; }

        // LLVM type of a `T[N]` field's inline storage, `[N x T]`; only when isInlineArray().
        llvm::Type* getInlineLlvmType(llvm::LLVMContext* ctx) const;

        // The element type for one level of indexing; `T[][]` returns the CajetaArray `T[]`.
        CajetaTypePtr getElementType() { return elementType; }
        // Address of the element-ownership tail bitmap: hdr + headerBytes + count*elemBytes,
        // count loaded from the header at runtime. Shared by slot-store, move-out, teardown.
        static llvm::Value* emitElementBitsBase(llvm::IRBuilder<>& builder,
            llvm::Value* hdrPtr, uint64_t headerBytes, uint64_t elemBytes);

        // The LLVM type inside the header's `[0 x T]`; `ptr` when the element is a reference.
        llvm::Type* getElementLlvmType(llvm::LLVMContext* ctx) const;

        // The SLOT STRIDE in bytes, read from the BUILT `{ i64, [0 x T] }` type that every
        // element GEP follows: a self-referential element degraded to `ptr` when the array
        // type was built, so every tail-bitmap walk must use THIS or indexes collapse.
        uint64_t elementStrideBytes(const llvm::DataLayout& dl,
                                    llvm::LLVMContext* ctx);

        // Build (intern) the `{ i64 size, [0 x T] data }` struct in `ctx` (per-thread rebuild).
        llvm::Type* buildLlvmType(llvm::LLVMContext* ctx) const;

        // When frozen with an empty per-thread table, rebuild in this thread's context.
        llvm::Type* getLlvmType() override;
    };
    typedef shared_ptr<CajetaArray> CajetaArrayPtr;
}
