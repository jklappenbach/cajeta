// One store loop shared by the `[...]` literal and the `{...}` declarator
// initializer so the two cannot drift: allocate a `{ i64 size, [0 x T] data }`
// header of length N, then evaluate, coerce and store each element in order.

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

    // Returns the array header pointer, or null when the allocator cannot resolve.
    // `useArena` routes the header through the frame arena instead of the heap for
    // a `stack [...]` proven non-escaping; arena arrays are primitive-element only.
    llvm::Value* emitArrayFromElements(
        CajetaModulePtr module,
        CajetaTypePtr elementType,
        const std::vector<AbstractSyntaxNodePtr>& elements,
        bool useArena = false,
        std::vector<std::pair<int, std::string>>* borrowedLocals = nullptr);
}
