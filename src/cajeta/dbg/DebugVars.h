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

namespace cajeta::dbg {

    struct DbgVar {
        std::string name;
        std::string type;   // cajeta canonical type name
        void* addr = nullptr;  // the local's slot
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

} // namespace cajeta::dbg
