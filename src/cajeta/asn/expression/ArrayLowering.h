//
// array-literals §7 — one store loop shared by the `[...]` literal expression
// (ArrayLiteralExpression) and the `{...}` declarator initializer
// (ArrayInitializer), so the two forms cannot drift.
//
// Given a resolved element type and the element nodes, allocate a
// `{ i64 size, [0 x T] data }` header of length N (the droppable-bits variant
// when the element type carries per-slot ownership), evaluate each element in
// order, coerce it to the slot width, and store it. Returns the array header
// pointer, or null if the runtime allocator can't be resolved.
//

#pragma once

#include <memory>
#include <vector>

namespace llvm { class Value; }

namespace cajeta {
    class CajetaModule;
    class CajetaType;
    class AbstractSyntaxNode;
    typedef std::shared_ptr<CajetaModule> CajetaModulePtr;
    typedef std::shared_ptr<CajetaType> CajetaTypePtr;
    typedef std::shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;

    // useArena routes the header through the frame arena
    // (__cajeta_new_array_header_arena) instead of the heap allocator —
    // array-literals §4, for a `stack [...]` literal proven non-escaping. The
    // arena allocator takes the identical (headerBytes, elemBytes, count)
    // signature; only the callee differs. Arena arrays are primitive-element
    // only, so they never combine with the droppable-bits allocator.
    llvm::Value* emitArrayFromElements(
        CajetaModulePtr module,
        CajetaTypePtr elementType,
        const std::vector<AbstractSyntaxNodePtr>& elements,
        bool useArena = false);
}
