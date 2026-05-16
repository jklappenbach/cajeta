//
// CajetaView — zero-copy memory overlay onto a byte buffer (Views.md).
//
// Sibling of CajetaStruct under CajetaAggregate. Owns the legacy wire-format-
// view codegen (packed/aligned layout, bswap, length-prefix sweep, bounds
// check, view-constructor synthesis) that previously lived on CajetaStruct.
//
// v1 capabilities:
//   - Packed layout by default; @Align(natural) opts into padding.
//   - @BigEndian / @LittleEndian / @HostEndian annotations.
//   - View constructor synthesis (`MyView(byte[])`) with bounds check.
//   - Single trailing String field with inline length-prefix encoding.
//
// S3-S5 expand this: required endianness, owning variant (`MyView(#bytes)`),
// multiple variable-size fields, fields after a variable-size field,
// nested views, methods on views, length-prefix validation.
//

#pragma once

#include "CajetaAggregate.h"

namespace cajeta {

    class CajetaView;
    typedef shared_ptr<CajetaView> CajetaViewPtr;

    // Endianness annotation on a view declaration. Resolved from the
    // `@BigEndian` / `@LittleEndian` annotations during type registration;
    // codegen consults it when emitting field accessors. Nested views
    // inherit from their outer unless they carry their own annotation —
    // see Views.md § Endianness inheritance.
    enum struct ViewEndianness {
        Host,           // default — no annotation, use host order
        Big,            // @BigEndian
        Little          // @LittleEndian
    };

    // Alignment annotation. Default is packed (no padding); @Align(natural)
    // opts into natural alignment per field. See Views.md § Alignment.
    enum struct ViewAlignment {
        Packed,         // default
        Natural         // @Align(natural)
    };

    class CajetaView : public CajetaAggregate {
    private:
        ViewEndianness endianness = ViewEndianness::Host;
        ViewAlignment alignment = ViewAlignment::Packed;
    public:
        CajetaView(CajetaModulePtr module) : CajetaAggregate(module) { }
        CajetaView(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaAggregate(module, qName) { }

        ViewEndianness getEndianness() const { return endianness; }
        void setEndianness(ViewEndianness e) { endianness = e; }

        ViewAlignment getAlignment() const { return alignment; }
        void setAlignment(ViewAlignment a) { alignment = a; }

        // Builds the LLVM struct body (packed or natural alignment), registers
        // the view in the module's canonical type map, and generates method
        // prototypes. The view constructor itself is synthesized on demand by
        // MethodCallExpression when it sees `MyView(byte[])`.
        void generatePrototype() override;

        // Byte size of the fixed prefix (sum of fixed-size fields, with
        // packed or natural alignment per annotation). Variable-size fields
        // contribute their i32 length-prefix; their data bytes are not
        // counted (they live past the LLVM struct's footprint in the buffer).
        uint64_t getFixedSize() const;
    };

} // namespace cajeta
