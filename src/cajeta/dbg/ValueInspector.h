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
#include <cstdint>
#include <optional>
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

    // One child row of an aggregate (Units 2-3). `addr` is the child's SLOT,
    // uniform with a frame local: for a Pointer slot the slot holds a pointer
    // to the instance (String*, class*), for an Inline slot the slot holds the
    // value's bytes. inspect(child.type, child.addr) decodes it either way.
    struct InspectedChild {
        std::string name;   // declared field name, "[i]", or a map key
        std::string type;   // canonical type name
        void* addr = nullptr;
        Storage storage = Storage::Inline;
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
        explicit ValueInspector(const llvm::DataLayout& dl);

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

        // True if `type` resolves in the live type world (FQ or short name).
        bool canResolve(const std::string& type) const;

    private:
        const llvm::DataLayout& dl_;

        // Decode a cajeta.lang.String instance reachable from `slot` (which
        // holds the String pointer) into its text, quoted+escaped. Returns
        // "<null>" for a null pointer. Units 1.
        std::string decodeString(void* slot);

        // Resolved storage geometry of one array element (Unit 2). Mirrors
        // CajetaArray::getElementLlvmType: a reference-class/array element is a
        // pointer slot; String keeps its full 64-byte struct slot with the
        // String* at the base; primitives and value structs are inline.
        struct ArrayInfo {
            bool ok = false;
            std::string elemType;   // canonical element type name
            uint64_t stride = 0;    // per-element slot stride, from DataLayout
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
