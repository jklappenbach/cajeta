// CajetaView — a zero-copy overlay onto a byte buffer (Views.md), owning the
// wire-format codegen: packed/aligned layout, bswap, length-prefix sweep,
// bounds check, ctor synthesis, and a no-vtable field-index override.

#pragma once

#include "CajetaClass.h"
#include "StructureProperty.h"

namespace cajeta {

    class CajetaView;
    typedef shared_ptr<CajetaView> CajetaViewPtr;

    // A view's byte order; a nested view inherits its outer's unannotated.
    enum struct ViewEndianness {
        Host,           // default — no annotation, use host order
        Big,            // @BigEndian
        Little          // @LittleEndian
    };

    // Packed (no padding) unless @Align(natural) asks for per-field alignment.
    enum struct ViewAlignment {
        Packed,         // default
        Natural         // @Align(natural)
    };

    class CajetaView : public CajetaClass {
    private:
        ViewEndianness endianness = ViewEndianness::Host;
        ViewAlignment alignment = ViewAlignment::Packed;
        // `endianness` alone cannot tell "@HostEndian" from "no annotation".
        bool endiannessExplicit = false;
        // Separate, so a second conflicting inheritance is detectable.
        bool endiannessInherited = false;
        // Populated during generatePrototype.
        int variableSizeFieldCount = 0;
        // Any element-array field makes this a DESCRIPTOR view: its value is a
        // {i8* data, i64* table} pair, not the raw data pointer.
        bool hasElementArrayField_ = false;
    public:
        CajetaView(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaView(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }

        // True iff `property` is a variable-size field (String, inline `T[]`).
        static bool isVariableSize(const StructurePropertyPtr& property);

        // True iff `property` is an ELEMENT-ARRAY field (`V[]` or `String[]`):
        // on the wire, a u32 count then that many self-delimiting elements.
        static bool isElementArray(const StructurePropertyPtr& property);

        // Advance `offset` (i64, relative to `basePtr`) over one property,
        // emitting NO bounds checks: the ctor sweep proved every prefix in
        // bounds. `e` is the CONTAINING view's order, which prefix loads swap by.
        static llvm::Value* emitAccessAdvance(CajetaModulePtr module,
            const StructurePropertyPtr& property,
            llvm::Value* basePtr, llvm::Value* offset,
            ViewEndianness e = ViewEndianness::Host);

        // Advance `offset` over ONE element of a `V[]`, which the composition
        // guard keeps element-array-free, so this is straight-line code.
        static llvm::Value* emitElementAdvance(CajetaModulePtr module,
            const shared_ptr<CajetaView>& elemView,
            llvm::Value* basePtr, llvm::Value* offset);

        // --- Descriptor views (view v1.1 offset table) -------------------
        // The table gives O(1) offsets: a fixed slot per post-first-var
        // property, two for a var-size-element array, then per-element regions.
        bool getHasElementArrayField() const { return hasElementArrayField_; }

        // True iff the array's ELEMENTS are var-size, which earns a region.
        static bool elementArrayHasVarSizeElements(
            const StructurePropertyPtr& property);

        // `property`'s fixed slot in the offset table, or -1 when it precedes
        // the first var-size field and its offset is a compile-time constant.
        // Slots run in declaration order, 2 for a var-size-element array.
        int tableSlotOf(const StructurePropertyPtr& property) const;

        // Total fixed slots (the per-element regions start here).
        int tableFixedSlotCount() const;

        // No vtable header, so view properties keep 0-based LLVM indices.
        int getFieldLlvmIndex(const StructurePropertyPtr& prop) const override {
            return prop->getOrder();
        }

        ViewEndianness getEndianness() const { return endianness; }
        void setEndianness(ViewEndianness e) { endianness = e; endiannessExplicit = true; }
        bool hasExplicitEndianness() const { return endiannessExplicit; }

        // An unannotated `V[]` element view inherits the OUTER view's order;
        // explicit wins, and two different inherited orders are an error.
        bool hasInheritedEndianness() const { return endiannessInherited; }
        void inheritEndianness(ViewEndianness e) {
            endianness = e;
            endiannessInherited = true;
        }

        // True iff endianness `e` differs from this build's host order.
        static bool needsBswap(CajetaModulePtr module, ViewEndianness e);

        // bswap `v` when `e` differs from host order; every wire prefix and
        // count load routes through this.
        static llvm::Value* emitSwapIfNeeded(CajetaModulePtr module,
            ViewEndianness e, llvm::Value* v);

        ViewAlignment getAlignment() const { return alignment; }
        void setAlignment(ViewAlignment a) { alignment = a; }

        int getVariableSizeFieldCount() const { return variableSizeFieldCount; }

        // Smallest buffer this view can overlay: the fixed prefix plus each
        // variable-size field's i32 length prefix. A shorter buffer throws.
        uint64_t getMinimumSize() const;

        // Build the struct body, register the view canonically and generate
        // prototypes; the ctor is synthesized on demand at its first call site.
        void generatePrototype() override;

        // Frozen per-thread rebuild: with an empty thread binding, re-create the
        // fixed-prefix struct in this thread's context, registering nothing.
        llvm::Type* getLlvmType() override;

        // Byte size of the fixed prefix. A variable-size field contributes only
        // its i32 length prefix; its data lives past the struct's footprint.
        uint64_t getFixedSize() const;
    };

} // namespace cajeta
