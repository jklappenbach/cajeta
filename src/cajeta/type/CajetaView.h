//
// CajetaView — zero-copy memory overlay onto a byte buffer (Views.md).
//
// Direct child of CajetaClass. Owns the wire-format-view codegen
// (packed/aligned layout, bswap, length-prefix sweep, bounds check,
// view-constructor synthesis) and a no-vtable field-index override —
// views are typed overlays, not value aggregates, and skip the vtable
// header that every plain CajetaClass instance carries at offset 0.
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

#include "CajetaClass.h"
#include "StructureProperty.h"

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

    class CajetaView : public CajetaClass {
    private:
        ViewEndianness endianness = ViewEndianness::Host;
        ViewAlignment alignment = ViewAlignment::Packed;
        // Tracks whether endianness was explicitly annotated. A view with no
        // annotation defaults to host order, but nested views inherit their
        // outer's order when unannotated (Views.md § Endianness inheritance);
        // the default value of `endianness` alone can't distinguish
        // "user wrote @HostEndian" from "user wrote nothing".
        bool endiannessExplicit = false;
        // Count of variable-size fields. Populated during generatePrototype;
        // used by getMinimumSize and the construction-time validation sweep.
        int variableSizeFieldCount = 0;
    public:
        CajetaView(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaView(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }

        // True iff `property` is a variable-size field — String today,
        // T[]-as-inline-field once that lands. Migrated from
        // CajetaAggregate (P7.6); used by view layout (length-prefix
        // substitution) and view field accessors (DotExpression.cpp).
        static bool isVariableSize(const StructurePropertyPtr& property);

        // True iff `property` is an ELEMENT-ARRAY field (view v1.1,
        // specs/view-element-arrays-spec.md): `V[]` where V is a view
        // type, or `String[]`. Wire layout: u32 count + count elements
        // back-to-back, each self-delimiting. Subset of isVariableSize;
        // the ctor sweep walks these with a per-element runtime loop
        // (primitive `T[]` advances by count*sizeof(T) instead).
        static bool isElementArray(const StructurePropertyPtr& property);

        // Access-side offset arithmetic (post-validation — the ctor sweep
        // already proved every prefix in-bounds, so these emit NO checks).
        // Both walk relative to `basePtr` (the view's data pointer) and
        // return the new offset as an i64 llvm::Value*.
        //
        // emitAccessAdvance: advance `offset` over one property of a view —
        // fixed (+static size), String (+4+len), primitive T[]
        // (+4+count*sizeof(T)), or element array (+4 + a runtime loop over
        // the elements; constant stride when the element view is
        // fixed-size).
        static llvm::Value* emitAccessAdvance(CajetaModulePtr module,
            const StructurePropertyPtr& property,
            llvm::Value* basePtr, llvm::Value* offset);

        // emitElementAdvance: advance `offset` over ONE element of a `V[]`
        // field — walks V's properties in declaration order. V is
        // guaranteed element-array-free (the generatePrototype composition
        // guard), so this emits straight-line code.
        static llvm::Value* emitElementAdvance(CajetaModulePtr module,
            const shared_ptr<CajetaView>& elemView,
            llvm::Value* basePtr, llvm::Value* offset);

        // No vtable header — view properties stay at 0-based LLVM
        // indices. CajetaClass's default reserves slot 0 for the vtable
        // pointer. Override carried over from the retired
        // CajetaAggregate so views keep the same wire-format layout
        // (Views.md § Layout).
        int getFieldLlvmIndex(const StructurePropertyPtr& prop) const override {
            return prop->getOrder();
        }

        ViewEndianness getEndianness() const { return endianness; }
        void setEndianness(ViewEndianness e) { endianness = e; endiannessExplicit = true; }
        bool hasExplicitEndianness() const { return endiannessExplicit; }

        ViewAlignment getAlignment() const { return alignment; }
        void setAlignment(ViewAlignment a) { alignment = a; }

        // Count of variable-size fields (`String`, `T[]`) declared on this
        // view. Used by getMinimumSize and by the view-ctor's length-prefix
        // validation sweep. Populated during generatePrototype.
        int getVariableSizeFieldCount() const { return variableSizeFieldCount; }

        // Minimum buffer size required to construct an instance of this
        // view: fixed-prefix bytes + 4 bytes per variable-size field (each
        // needs at least its i32 length-prefix to be readable; minimum data
        // length is zero). The view-ctor's bounds check uses this; if the
        // caller's buffer is smaller, construction throws.
        uint64_t getMinimumSize() const;

        // Builds the LLVM struct body (packed or natural alignment), registers
        // the view in the module's canonical type map, and generates method
        // prototypes. The view constructor itself is synthesized on demand by
        // MethodCallExpression when it sees `MyView(byte[])`.
        void generatePrototype() override;

        // U6.4.2 — frozen per-thread rebuild: when frozen and this thread's
        // binding is empty, re-create the view's fixed-prefix struct + setBody in
        // the thread's context (no canonicalMap re-registration). Inert while not
        // frozen (delegates to the base, returning the inline-bound struct).
        llvm::Type* getLlvmType() override;

        // Byte size of the fixed prefix (sum of fixed-size fields, with
        // packed or natural alignment per annotation). Variable-size fields
        // contribute their i32 length-prefix; their data bytes are not
        // counted (they live past the LLVM struct's footprint in the buffer).
        uint64_t getFixedSize() const;
    };

} // namespace cajeta
