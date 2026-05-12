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
     * POD struct type. Sibling to CajetaClass. v1 implementation is a stub —
     * it carries the struct's annotations (endianness, alignment) and inherits
     * the field machinery from CajetaClass, but does not yet emit a view
     * constructor or compute byte offsets.
     *
     * Real layout + constructor + field-accessor codegen lands in Session 4
     * (see `ImplementationStatus.md`).
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

        // Override: packed layout (no padding by default), no default
        // constructor, no vtable/RTTI. Register the struct itself in the
        // canonical type map so name lookups return this instance (not a
        // generic placeholder).
        void generatePrototype() override;

        // Byte size of the struct's fixed prefix (sum of fixed-size fields,
        // with packed or natural alignment depending on this struct's
        // annotation). Variable-size fields aren't counted here — their
        // contribution is computed at view-construction time.
        uint64_t getFixedSize() const;
    };

} // namespace cajeta
