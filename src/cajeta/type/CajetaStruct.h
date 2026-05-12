//
// `struct` types — POD aggregates with declared (not compiler-chosen) layout.
// See WireFormats.md for full layout / endianness / packing / view-construction
// rules. v1 of this type is a stub: it identifies that a declaration is a
// struct vs a class, but the actual layout computation, view-constructor
// synthesis, and field-accessor codegen land in Session 4 of the rollout.
//
// Created during the Memory Model + Wire Formats implementation (Session 2 /
// Step 2.3).
//

#pragma once

#include "CajetaClass.h"

namespace cajeta {

    class CajetaStruct;
    typedef shared_ptr<CajetaStruct> CajetaStructPtr;

    // Endianness annotation on a struct declaration. Resolved from the
    // `@BigEndian` / `@LittleEndian` annotations during type registration;
    // codegen consults it when emitting field accessors. Nested structs
    // inherit from their outer unless they carry their own annotation — see
    // WireFormats.md § Endianness inheritance.
    enum struct StructEndianness {
        Host,           // default — no annotation, use host order
        Big,            // @BigEndian
        Little          // @LittleEndian
    };

    // Alignment annotation. Default is packed (no padding); @Align(natural)
    // opts into natural alignment per field. See WireFormats.md § Alignment.
    enum struct StructAlignment {
        Packed,         // default
        Natural         // @Align(natural)
    };

    /**
     * POD struct type. Sibling to CajetaClass. Real layout + view constructor
     * + field-accessor codegen are wired in Sessions 4 and 5 of the rollout.
     *
     * Variable-size field support (Session 5.5b): String-typed fields lay out
     * as an inline `i32 length` followed by `length` bytes. The LLVM struct
     * type substitutes the length prefix for the field's slot; the data bytes
     * live past the LLVM struct's footprint in the buffer. Restriction in
     * v1: at most one variable-size field, and it must be the last field.
     * Reading such a field allocates a null-terminated copy so the result is
     * compatible with the existing String stdlib.
     */
    class CajetaStruct : public CajetaClass {
    private:
        StructEndianness endianness = StructEndianness::Host;
        StructAlignment alignment = StructAlignment::Packed;
    public:
        CajetaStruct(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaStruct(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }

        StructEndianness getEndianness() const { return endianness; }
        void setEndianness(StructEndianness e) { endianness = e; }

        StructAlignment getAlignment() const { return alignment; }
        void setAlignment(StructAlignment a) { alignment = a; }

        // True iff `property` is a variable-size struct field (String today;
        // T[] support to follow). Variable-size fields are laid out as an
        // inline i32 length prefix followed by `length` bytes.
        static bool isVariableSize(const StructurePropertyPtr& property);

        // Override: packed layout (no padding by default), no default
        // constructor, no vtable/RTTI. Register the struct itself in the
        // canonical type map so name lookups return this instance (not a
        // generic placeholder).
        void generatePrototype() override;

        // Byte size of the struct's fixed prefix (sum of fixed-size fields,
        // with packed or natural alignment depending on this struct's
        // annotation). Variable-size fields contribute their i32 length-prefix
        // here; their data bytes are not counted (they live past the LLVM
        // struct's footprint in the buffer).
        uint64_t getFixedSize() const;
    };

} // namespace cajeta
