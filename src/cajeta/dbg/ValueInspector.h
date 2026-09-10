// The debugger's type-introspection bridge: classify, summarize and enumerate a
// value at a live address. Every layout fact comes from the DebugTypeTable, never
// from CajetaType, so an empty table decodes to `<unknown>`, never a guess.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cajeta/dbg/DebugTypeTable.h"

namespace llvm { class DataLayout; }

namespace cajeta::dbg {

    enum class ValueKind { Unknown, Leaf, Aggregate };

    // A classification plus a collapsed summary; children are enumerated lazily.
    struct InspectedValue {
        ValueKind kind = ValueKind::Unknown;
        std::string summary;   // e.g. "5", "\"hi\"", "{3 elements}", "<null>"
    };

    // One child row. `addr` is the child's SLOT, uniform with a frame local: a
    // Pointer slot holds the instance pointer, an Inline slot holds the bytes.
    struct InspectedChild {
        std::string name;   // declared field name, "[i]", or a map key
        std::string type;   // canonical type name
        void* addr = nullptr;
        Storage storage = Storage::Inline;
        // For a static row `addr` is the session-resolved GLOBAL, not an offset.
        bool isStatic = false;
    };

    // One window of an aggregate's children; a large array is never eager.
    struct ChildPage {
        std::vector<InspectedChild> children;
        size_t remaining = 0;   // elements after this window not yet returned
        size_t nextStart = 0;   // start index for the follow-up page
    };

    class ValueInspector {
    public:
        // Decodes against the process-global table; `dl` only names the session.
        explicit ValueInspector(const llvm::DataLayout& dl,
                                const ResolvedTypeSymbols* symbols = nullptr);

        // Decodes against a caller-supplied table, whose type names need not exist
        // in the live type world. With `symbols`, a reference row's slot-0 word
        // narrows it to its runtime type; null means declared-type decode only.
        ValueInspector(const llvm::DataLayout& dl, const DebugTypeTable& table,
                       const ResolvedTypeSymbols* symbols = nullptr);

        static constexpr size_t kDefaultPageSize = 100;

        // Summarizes `type` at slot `addr`, dereferencing a reference; never faults.
        InspectedValue inspect(const std::string& type, void* addr);

        // One page of an aggregate's children from slot `addr`; empty, never a fault.
        ChildPage children(const std::string& type, void* addr,
                           size_t start = 0, size_t pageSize = kDefaultPageSize);

        // True if `type` is decodable: a primitive, or carried by the table.
        bool canResolve(const std::string& type) const;

        // `declared`, narrowed through the instance's slot-0 vtable word when the
        // symbol map knows it; falls back to `declared` rather than guessing.
        std::string runtimeType(const std::string& declared, void* addr);

    private:
        const llvm::DataLayout& dl_;
        const DebugTypeTable& table_;
        const ResolvedTypeSymbols* symbols_ = nullptr;

        // The one narrowing seam: a reference row's runtime record and rebased base.
        struct ResolvedObject {
            const TypeRecord* rec = nullptr;  // effective record (may be null)
            void* inst = nullptr;             // instance base, rebased
            std::string type;                 // effective canonical name
        };
        ResolvedObject resolveObject(const std::string& declared, void* addr);

        // Narrows one child row's reported type; Pointer-storage rows only.
        void narrowRow(InspectedChild& child);

        // The String at `*slot`, quoted and escaped; "<null>" or "<string?>".
        std::string decodeString(void* slot);

        // Storage geometry of one array element, from the array's table record.
        // ok=false when there is no record: show no children, never a guessed stride.
        struct ArrayInfo {
            bool ok = false;
            std::string elemType;   // canonical element type name
            uint64_t stride = 0;    // per-element slot stride
            Storage storage = Storage::Inline;
        };
        ArrayInfo resolveArrayElement(const std::string& arrayType);

        // Opens an array slot: header deref, length at offset 0, `data` past the
        // header. False for a null pointer, leaving *data and *length untouched.
        bool openArray(void* addr, char** data, int64_t* length);

        // Up to 5 elements rendered inline, else "{N elements}"; length-capped.
        std::string arraySummary(const ArrayInfo& info, char* data,
                                 int64_t length);

        // `type`'s non-static fields, inherited first, at their DataLayout offsets
        // off the instance; `addr` is the slot, dereferenced only for a class.
        std::vector<InspectedChild> objectChildren(const std::string& type,
                                                   void* addr);

        // The first few scalar fields as "{x=3, y=4}", or "{…}" when there are none.
        std::string objectSummary(const std::string& type, void* addr);

        // The logical view of a registered collection: live entries, not capacity.
        // nullopt (unregistered, or a backing field missing) falls back to fields.
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

    // Renders `text` quoted and escaped: "a\nb" -> "\"a\\nb\"". Pure.
    std::string escapeAndQuote(const std::string& text);

} // namespace cajeta::dbg
