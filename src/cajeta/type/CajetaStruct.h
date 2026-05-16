//
// `struct` types — stack-allocated value aggregates (Structs.md).
//
// Rollout note (StructsViewsStatus.md / S2): CajetaStruct is the in-progress
// new stack-struct construct. Its real semantics (alloca-on-declaration,
// class-ref fields, inline composition into classes, interface dispatch via
// tagged fat pointer) land in Sessions 6-11. Until then, the struct keyword
// parses but generatePrototype() throws CAJETA_ERROR_STRUCT_UNIMPLEMENTED.
//
// The existing wire-format-view machinery (packed/aligned layout, bswap,
// length-prefix sweep, bounds check, view-constructor synthesis) moves to
// the CajetaView sibling type. CajetaView inherits from CajetaStruct so all
// existing dynamic_pointer_cast<CajetaStruct>(t) checks in codegen continue
// to match view types — that's the correct shape because views ARE struct-
// shaped at the LLVM layer.
//

#pragma once

#include "CajetaClass.h"

namespace cajeta {

    class CajetaStruct;
    typedef shared_ptr<CajetaStruct> CajetaStructPtr;
    class CajetaView;
    typedef shared_ptr<CajetaView> CajetaViewPtr;

    // Endianness annotation on a struct declaration. Resolved from the
    // `@BigEndian` / `@LittleEndian` annotations during type registration;
    // codegen consults it when emitting field accessors. Nested structs
    // inherit from their outer unless they carry their own annotation — see
    // Structs.md § Endianness inheritance.
    enum struct StructEndianness {
        Host,           // default — no annotation, use host order
        Big,            // @BigEndian
        Little          // @LittleEndian
    };

    // Alignment annotation. Default is packed (no padding); @Align(natural)
    // opts into natural alignment per field. See Structs.md § Alignment.
    enum struct StructAlignment {
        Packed,         // default
        Natural         // @Align(natural)
    };

    /**
     * Stack-allocated value aggregate. Real semantics (S6-S11) not yet wired;
     * generatePrototype() throws CAJETA_ERROR_STRUCT_UNIMPLEMENTED.
     *
     * Subclassed by CajetaView, which calls generatePrototypeImpl() to get
     * the packed-layout / endianness-aware / variable-size-field codegen
     * inherited from the prior wire-format-view implementation. The shared
     * code lives in this class (protected) so the view subclass can reuse it
     * without duplicating; the struct path will replace it in S6 with stack-
     * alloca semantics.
     */
    class CajetaStruct : public CajetaClass {
    private:
        StructEndianness endianness = StructEndianness::Host;
        StructAlignment alignment = StructAlignment::Packed;
    protected:
        // The legacy view-style layout + LLVM struct body generation. CajetaView
        // calls this directly; CajetaStruct does not (S6 will replace it with
        // stack-struct semantics).
        void generatePrototypeImpl();
    public:
        CajetaStruct(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaStruct(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }

        StructEndianness getEndianness() const { return endianness; }
        void setEndianness(StructEndianness e) { endianness = e; }

        StructAlignment getAlignment() const { return alignment; }
        void setAlignment(StructAlignment a) { alignment = a; }

        // True iff `property` is a variable-size field (String today; T[]
        // support to follow). Variable-size fields are laid out as an inline
        // i32 length prefix followed by `length` bytes.
        static bool isVariableSize(const StructurePropertyPtr& property);

        // Stack-struct codegen is not yet implemented. Throws
        // CAJETA_ERROR_STRUCT_UNIMPLEMENTED until S6.
        void generatePrototype() override;

        // Byte size of the fixed prefix (sum of fixed-size fields, packed or
        // natural alignment depending on annotation). Variable-size fields
        // contribute their i32 length-prefix; their data bytes are not counted
        // (they live past the LLVM struct's footprint in the buffer). Returns
        // 0 if the LLVM type hasn't been built yet (CajetaStruct never builds
        // one in v1; CajetaView does).
        uint64_t getFixedSize() const;

        // No vtable header — user properties stay at 0-based LLVM indices.
        int getFieldLlvmIndex(const StructurePropertyPtr& prop) const override {
            return prop->getOrder();
        }
    };

    /**
     * Zero-copy memory overlay onto a byte buffer (Views.md). Inherits the
     * shared layout / codegen machinery from CajetaStruct via
     * generatePrototypeImpl(). v1 capabilities (carried over from the prior
     * struct-as-view work):
     *
     *   - Packed layout by default; @Align(natural) opts into padding.
     *   - @BigEndian / @LittleEndian / @HostEndian annotations.
     *   - View constructor synthesis (`MyView(byte[])`) with bounds check.
     *   - Single trailing String field with inline length-prefix encoding.
     *
     * S3-S5 expand this: required endianness annotation, owning variant
     * (`MyView(#bytes)`), multiple variable-size fields, fields after a
     * variable-size field, nested views, methods, length-prefix validation.
     */
    class CajetaView : public CajetaStruct {
    public:
        CajetaView(CajetaModulePtr module) : CajetaStruct(module) { }
        CajetaView(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaStruct(module, qName) { }

        // Wires up the legacy view codegen via the shared impl. Overrides
        // CajetaStruct's stub.
        void generatePrototype() override { generatePrototypeImpl(); }
    };

} // namespace cajeta
