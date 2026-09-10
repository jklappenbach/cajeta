// Debugger CP5 host-side value layer: walks the dbg frame chain (StopEvent::frameTop) through
// the runtime's stateless extern "C" accessors, rendering each captured local for DAP.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cajeta/dbg/MemoryFacets.h"

namespace cajeta::dbg {

    struct DbgVar {
        std::string name;
        std::string type;   // cajeta canonical type name
        void* addr = nullptr;  // the local's slot
        // CP7-1b/1c memory facets read back at a stop: alloc and ownership arrive as bytes
        // from __cajeta_dbg_local, lifetime is derived here from the drop entry's flag.
        AllocClass    alloc    = AllocClass::Unknown;
        OwnershipRole ownership = OwnershipRole::Unknown;
        LifetimeState lifetime = LifetimeState::Unknown;
    };

    struct DbgFrameInfo {
        std::string func;       // cajeta-mangled enclosing function name
        int32_t locId = -1;     // current loc in this frame (-1 if none yet)
        std::vector<DbgVar> locals;
    };

    // Walk the frame chain from `top` (the StopEvent value) outward, innermost first.
    std::vector<DbgFrameInfo> walkFrames(void* top);

    // True if `type` is a cajeta primitive - the single source of truth for "scalar leaf".
    bool isPrimitiveTypeName(const std::string& type);

    // Render the value at `addr` as cajeta type `type`. Primitives are read by width;
    // other types render `<type@0xADDR>` from the heap pointer the slot holds.
    std::string formatValue(const std::string& type, void* addr);

    // Write `text` into the primitive at `addr`; false (with *err) for other types.
    bool writeValue(const std::string& type, void* addr,
                    const std::string& text, std::string* err);

    // Evaluate a constrained breakpoint condition, `<localName> <op> <literal>` with op one
    // of == != < <= > >=, against a frame's locals. Returns whether to STOP; on any error it
    // fills *err and returns true, so a malformed condition stops rather than hiding.
    bool evaluateCondition(const std::string& expr,
                           const std::vector<DbgVar>& locals,
                           std::string* err);

} // namespace cajeta::dbg
