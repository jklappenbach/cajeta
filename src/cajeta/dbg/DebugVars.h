//
// Debugger CP5 host-side value layer. At a breakpoint stop the runtime hands
// the host the dbg frame-chain head (StopEvent::frameTop). These helpers walk
// that chain — via the runtime's stateless extern "C" accessors, so the host's
// native runtime copy reads a chain the JIT'd bitcode copy built — and turn
// each captured local into a DAP-presentable {name, type, value}.
//
// Value slice: primitives (int/uint widths, char, boolean, float32/64) render
// as their literal value and can be written back via writeValue; every other
// type renders opaquely as `<Type@addr>` and rejects writes.
//
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
        // CP7-1b/1c memory facets, read back from the frame chain at a stop.
        // alloc + ownership are carried as bytes by __cajeta_dbg_local;
        // lifetime is derived here (deriveLifetime) from the drop entry's live
        // `active` flag. Default Unknown until walkFrames fills them.
        AllocClass    alloc    = AllocClass::Unknown;
        OwnershipRole ownership = OwnershipRole::Unknown;
        LifetimeState lifetime = LifetimeState::Unknown;
    };

    struct DbgFrameInfo {
        std::string func;       // cajeta-mangled enclosing function name
        int32_t locId = -1;     // current loc in this frame (-1 if none yet)
        std::vector<DbgVar> locals;
    };

    // Walk the frame chain from `top` (innermost) outward. Returns frames in
    // innermost-first order. `top` is the StopEvent::frameTop value; null -> {}.
    std::vector<DbgFrameInfo> walkFrames(void* top);

    // Render the value at `addr` interpreted as cajeta type `type`. Primitives
    // are read by width; other types render `<type@0xADDR>` (reading the heap
    // pointer the slot holds).
    std::string formatValue(const std::string& type, void* addr);

    // Write `text` into the primitive at `addr`. Returns false (and fills *err
    // if non-null) for non-primitive types or unparseable text.
    bool writeValue(const std::string& type, void* addr,
                    const std::string& text, std::string* err);

    // Evaluate a constrained breakpoint condition against a frame's locals
    // (debugger CP6f). The supported grammar is deliberately tiny — there is no
    // expression engine in the tree — and covers the common case:
    //
    //     <localName> <op> <literal>      op ∈ { ==, !=, <, <=, >, >= }
    //
    // The named local is read by its primitive type; integer/char/boolean are
    // compared in the integer domain (boolean also accepts true/false literals),
    // float32/64 in the double domain. Returns whether the breakpoint should
    // STOP: true when the condition holds. On any error (no operator, unknown
    // local, non-primitive, unparseable literal) it fills *err and returns true
    // — a malformed condition stops so the user notices, matching common IDE
    // behavior. On success *err is cleared.
    bool evaluateCondition(const std::string& expr,
                           const std::vector<DbgVar>& locals,
                           std::string* err);

} // namespace cajeta::dbg
