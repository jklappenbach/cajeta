//
// debugger-variable-inspection: the type-introspection bridge.
//
// Given a canonical cajeta type name (e.g. "cajeta.lang.String", "int32",
// "tour.Point[]"), a live address, and the JIT's DataLayout, decode the value:
// classify it leaf vs aggregate, render a collapsed summary, and (Units 2-3)
// enumerate typed children. This is the ONLY place that bridges a debug-time
// type-name string to the compiler's live type world (CajetaType) + LLVM
// DataLayout; DebugVars and DapServer consume it and never reach into
// CajetaType themselves (spec §2.1.2).
//
// Layout comes from the live LLJIT DataLayout, exactly as deriveEntryArgsABI
// derives the String ABI — nothing about a type's layout is hardcoded here.
//
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace llvm { class DataLayout; }

namespace cajeta::dbg {

    // How a child's bytes sit relative to its address (spec §2.1.3). A pointer
    // slot holds a pointer to a heap instance (reference classes, String); an
    // inline slot holds the value's bytes directly (primitives, value structs).
    enum class Storage { Inline, Pointer };

    enum class ValueKind { Unknown, Leaf, Aggregate };

    // A decoded value: its classification plus a rendered collapsed summary.
    // Children are enumerated lazily (children(), Units 2-3), not carried here.
    struct InspectedValue {
        ValueKind kind = ValueKind::Unknown;
        std::string summary;   // e.g. "5", "\"hi\"", "{3 elements}", "<null>"
    };

    // One child row of an aggregate (Units 2-3).
    struct InspectedChild {
        std::string name;   // declared field name, "[i]", or a map key
        std::string type;   // canonical type name
        void* addr = nullptr;
        Storage storage = Storage::Inline;
    };

    class ValueInspector {
    public:
        explicit ValueInspector(const llvm::DataLayout& dl);

        // Classify + summarize the value of canonical `type` at `addr`.
        // `addr` points at the value's slot: for a reference type (String,
        // class) the slot holds a pointer to the instance and inspect
        // dereferences it; for a primitive the slot holds the bytes. Null and
        // unresolved types render safely, never fault (spec §1.5).
        InspectedValue inspect(const std::string& type, void* addr);

        // True if `type` resolves in the live type world (FQ or short name).
        bool canResolve(const std::string& type) const;

    private:
        const llvm::DataLayout& dl_;

        // Decode a cajeta.lang.String instance reachable from `slot` (which
        // holds the String pointer) into its text, quoted+escaped. Returns
        // "<null>" for a null pointer. Units 1.
        std::string decodeString(void* slot);
    };

    // Render `text` as a quoted, escaped debugger value: "a\nb" -> "\"a\\nb\"".
    // Pure; no type world needed.
    std::string escapeAndQuote(const std::string& text);

} // namespace cajeta::dbg
