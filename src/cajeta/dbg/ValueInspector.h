//
// debugger-variable-inspection: the type-introspection bridge.
//
// Given a canonical cajeta type name (e.g. "cajeta.lang.String", "int32",
// "tour.Point[]") and a live address, decode the value: classify it leaf vs
// aggregate, render a collapsed summary, and enumerate typed children.
// DebugVars and DapServer consume this bridge and never reach into type
// information themselves (spec §2.1.2).
//
// Every layout fact comes from the DebugTypeTable (debug-type-sidecar §4.1):
// field byte offsets, element strides, inline-vs-pointer storage and the String
// ABI, all resolved from the live DataLayout when the table was built. The
// bridge itself never touches CajetaType, so it decodes identically on a cold
// launch (table built from the type world) and a warm cache-hit launch (table
// loaded from the slot sidecar, no type world at all). Nothing here is
// hardcoded: an empty table decodes to `<unknown>`, never to a guessed width.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// `Storage` (how a child's bytes sit relative to its address) lives with the
// type table — it is a layout fact the table carries, and the bridge reads it
// from there (debug-type-sidecar §2.1.2).
#include "cajeta/dbg/DebugTypeTable.h"

namespace llvm { class DataLayout; }

namespace cajeta::dbg {

    enum class ValueKind { Unknown, Leaf, Aggregate };

    // A decoded value: its classification plus a rendered collapsed summary.
    // Children are enumerated lazily (children(), Units 2-3), not carried here.
    struct InspectedValue {
        ValueKind kind = ValueKind::Unknown;
        std::string summary;   // e.g. "5", "\"hi\"", "{3 elements}", "<null>"
    };

    // One child row of an aggregate (Units 2-3). `addr` is the child's SLOT,
    // uniform with a frame local: for a Pointer slot the slot holds a pointer
    // to the instance (String*, class*), for an Inline slot the slot holds the
    // value's bytes. inspect(child.type, child.addr) decodes it either way.
    struct InspectedChild {
        std::string name;   // declared field name, "[i]", or a map key
        std::string type;   // canonical type name
        void* addr = nullptr;
        Storage storage = Storage::Inline;
        // A static field row (runtime-type-inspection §4): `addr` is the
        // session-resolved GLOBAL, not an instance offset. Rendered inline
        // after instance fields; the DAP layer maps this to the "static"
        // presentation hint.
        bool isStatic = false;
    };

    // One page of an aggregate's children (spec §2.2.4). A large array is never
    // enumerated eagerly: children() returns the window [start, start+pageSize)
    // plus how many elements remain past it and where the next page continues.
    struct ChildPage {
        std::vector<InspectedChild> children;
        size_t remaining = 0;   // elements after this window not yet returned
        size_t nextStart = 0;   // start index for the follow-up page
    };

    class ValueInspector {
    public:
        // Decode against the process-global table — the one the cold build
        // populated from the type world, or the one a cache hit loaded from the
        // sidecar. `dl` names the session whose target the table was built for;
        // the layout facts themselves come from the table, so every existing
        // call site (DapServer, tests) is unchanged.
        explicit ValueInspector(const llvm::DataLayout& dl,
                                const ResolvedTypeSymbols* symbols = nullptr);

        // Decode against a caller-supplied table. Used by tests to decode
        // through a table whose type names DO NOT exist in the live type world
        // — the warm path, simulated while the world is still up.
        // `symbols` (runtime-type-inspection Unit 3) is the session's resolved
        // symbol maps; with it, a reference row's slot-0 word narrows the row
        // to its RUNTIME type. Null = declared-type decode only.
        ValueInspector(const llvm::DataLayout& dl, const DebugTypeTable& table,
                       const ResolvedTypeSymbols* symbols = nullptr);

        // Default elements per page when a caller does not supply one (§3.1.4
        // threads the launch param; the bridge only needs a sane fallback).
        static constexpr size_t kDefaultPageSize = 100;

        // Classify + summarize the value of canonical `type` at `addr`.
        // `addr` points at the value's slot: for a reference type (String,
        // class) the slot holds a pointer to the instance and inspect
        // dereferences it; for a primitive the slot holds the bytes. Null and
        // unresolved types render safely, never fault (spec §1.5).
        InspectedValue inspect(const std::string& type, void* addr);

        // Enumerate one page of an aggregate's children. `addr` is the
        // aggregate's slot (for an array, the slot holds the header pointer).
        // Unit 2 handles arrays; object/collection children are Units 3/7 and
        // return an empty page here. Null/garbage yields an empty page, never a
        // fault (spec §2.2.5).
        ChildPage children(const std::string& type, void* addr,
                           size_t start = 0, size_t pageSize = kDefaultPageSize);

        // True if `type` is decodable: a primitive, or carried by the table.
        bool canResolve(const std::string& type) const;

        // The row's RUNTIME type: the declared name, narrowed through the
        // instance's slot-0 vtable word when the session's symbol map knows it
        // (runtime-type-inspection §2.1.3). Falls back to `declared` for value
        // types, leaves, unmatched words, null, or when no symbols were given
        // — never a fault, never a guess (§2.1.4).
        std::string runtimeType(const std::string& declared, void* addr);

    private:
        const llvm::DataLayout& dl_;
        const DebugTypeTable& table_;
        const ResolvedTypeSymbols* symbols_ = nullptr;

        // The one narrowing seam (§2.1.5): resolve a reference row to its
        // runtime record and REBASED instance base. Falls back to the declared
        // record and the plain deref when narrowing does not apply.
        struct ResolvedObject {
            const TypeRecord* rec = nullptr;  // effective record (may be null)
            void* inst = nullptr;             // instance base, rebased
            std::string type;                 // effective canonical name
        };
        ResolvedObject resolveObject(const std::string& declared, void* addr);

        // Narrow one child row's reported type (Pointer-storage rows only).
        void narrowRow(InspectedChild& child);

        // Decode a cajeta.lang.String instance reachable from `slot` (which
        // holds the String pointer) into its text, quoted+escaped. Returns
        // "<null>" for a null pointer, "<string?>" with no String ABI.
        std::string decodeString(void* slot);

        // Storage geometry of one array element, read from the array's table
        // record: a reference-class element keeps its own struct slot with the
        // instance pointer at the base, a nested array is a pointer slot, and
        // primitives and value structs are inline. ok=false when the table
        // carries no record — the decoder then shows no children rather than
        // walking a guessed stride.
        struct ArrayInfo {
            bool ok = false;
            std::string elemType;   // canonical element type name
            uint64_t stride = 0;    // per-element slot stride
            Storage storage = Storage::Inline;
        };
        ArrayInfo resolveArrayElement(const std::string& arrayType);

        // Locate the live backing of an array slot: dereference to the header,
        // read the length at offset 0, point `data` past the header. Returns
        // ok=false (and leaves *data/*length untouched) for a null pointer.
        bool openArray(void* addr, char** data, int64_t* length);

        // Collapsed array summary (§4.1.3): ≤5 elements inline (each rendered
        // by its own leaf value), else "{N elements}"; length-capped.
        std::string arraySummary(const ArrayInfo& info, char* data,
                                 int64_t length);

        // Object field decode (Unit 3): enumerate `type`'s non-static fields
        // (inherited then own) at their DataLayout byte offsets, addressed off
        // the instance. `addr` is the object's slot — dereferenced for a
        // reference class, used as-is for a value type. Null-safe.
        std::vector<InspectedChild> objectChildren(const std::string& type,
                                                   void* addr);

        // Brief field peek for an object value (§4.1.4): the first few scalar
        // fields as "{x=3, y=4}", length-capped; "{…}" when it has none.
        std::string objectSummary(const std::string& type, void* addr);

        // Unit 7: the logical view for a registered collection (ArrayList shows
        // its `sizeCount` elements from the `data` backing, not the capacity;
        // HashMap walks live `ctrl` slots to one entry per key). Returns nullopt
        // to fall back to the object-field view — for an unregistered type, or
        // when a backing field the view needs (by declared name) is missing, so
        // a stdlib layout shift fails safe rather than misreading (§8.3).
        std::optional<ChildPage> collectionChildren(const std::string& type,
                                                    void* addr, size_t start,
                                                    size_t pageSize);
        std::optional<ChildPage> arrayListChildren(const std::string& type,
                                                   void* addr, size_t start,
                                                   size_t pageSize);
        std::optional<ChildPage> hashMapChildren(const std::string& type,
                                                 void* addr, size_t start,
                                                 size_t pageSize);
    };

    // Render `text` as a quoted, escaped debugger value: "a\nb" -> "\"a\\nb\"".
    // Pure; no type world needed.
    std::string escapeAndQuote(const std::string& text);

} // namespace cajeta::dbg
