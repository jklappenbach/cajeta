//
// debug-type-sidecar: the layout facts variable inspection needs, detached from
// the compiler's type world.
//
// `ValueInspector` resolves a stopped value through `CajetaType::of` — a world
// that exists only after a compile. On a whole-program cache HIT the debug
// launch loads cached bitcode and never builds a Compiler, so that world is
// empty and every non-primitive renders `<unknown>`. This table holds one
// record per debug-reachable type — resolved byte offsets, element strides,
// inline-vs-pointer storage — built ONCE from the live world on a cold build,
// serialized into the slot sidecar, and reloaded on a hit (spec §2).
//
// Offsets are stored RESOLVED, not as (struct, index) pairs: the consumer needs
// neither CajetaType nor a live StructType lookup warm. This is sound because
// the JIT host target DataLayout is fixed across the cold build and the later
// warm load of the same slot (spec §1.3).
//
// Mirrors globalDbgLocTable(): one process-global instance, populated in place
// by the active debug compile, read by the DAP server.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm { class DataLayout; }

namespace cajeta::dbg {

    // How a value's bytes sit relative to its slot address. A pointer slot holds
    // a pointer to a heap instance (reference classes, String); an inline slot
    // holds the value's bytes directly (primitives, value structs). The same
    // distinction ValueInspector draws (spec §2.1.2).
    enum class Storage { Inline, Pointer };

    // Leaf: decoded by value (primitives, String — String by its own ABI).
    // Array/Object/Collection: expandable, decoded from the record below.
    enum class TypeKind { Leaf, Array, Object, Collection };

    // A stdlib collection with a logical debug view (elements, not backing).
    enum class CollectionKind { None, ArrayList, HashMap };

    // One non-static field, at its resolved byte offset from the instance base.
    struct FieldRecord {
        std::string name;       // declared field name
        std::string type;       // canonical type name
        uint64_t offset = 0;    // DataLayout byte offset within the instance
        Storage storage = Storage::Inline;
    };

    // A static field: decoded via its GLOBAL, not an instance offset. The
    // symbol (`<canonical>.<fieldName>`) is stable across runs; the session
    // resolves it to an address once at launch (runtime-type-inspection §4).
    struct StaticFieldRecord {
        std::string name;
        std::string type;
        std::string symbol;
    };

    // One vtable global: reading an instance's slot-0 word and matching it to
    // an entry names the RUNTIME type. A secondary (`$as$`) vtable's entry
    // carries its sub-object byte offset so a base-view interior pointer can
    // be rebased to the object base (runtime-type-inspection §2.1.1/2.1.3).
    struct VtableEntry {
        std::string canonical;
        uint64_t subObjectByteOffset = 0;
    };

    // An array's element geometry: the slot stride the JIT'd code stores at.
    struct ElemRecord {
        std::string type;
        uint64_t stride = 0;
        Storage storage = Storage::Inline;
    };

    struct TypeRecord {
        std::string canonical;
        TypeKind kind = TypeKind::Leaf;
        bool isValueType = false;   // inline vs pointer at the top level
        // A Leaf that is cajeta.lang.String — decoded by the table's String ABI
        // rather than by width. Carried as a FACT, not matched by name at a
        // stop: the decoder never string-compares against a stdlib FQN.
        bool isString = false;
        std::vector<FieldRecord> fields;   // Object/Collection, layout order
        // Static fields, inherited-then-own (matching the instance-field
        // convention); displayed inline after instance fields.
        std::vector<StaticFieldRecord> statics;
        ElemRecord elem;                   // Array
        CollectionKind collectionKind = CollectionKind::None;
    };

    // The String decode ABI: byte offsets within a cajeta.lang.String instance
    // of the length/tag word and the two payload fields, derived exactly as
    // deriveEntryArgsABI derives them (spec §2.1.4 — String keeps a Leaf record
    // and is decoded by these facts, not by field walking). Carried on the table
    // so the decode survives a cache hit, where the type world is gone.
    struct StringAbi {
        bool valid = false;
        uint64_t size = 0;
        uint64_t offLenTag = 0;
        uint64_t offAux = 0;
        uint64_t offBase = 0;
    };

    // Bounds on the closure walk (spec §5.1.3). Whatever a bound drops is
    // recorded in bounded() and logged — a reachable type is never silently
    // truncated into a `<unknown>` with no explanation.
    struct BuildOptions {
        size_t maxRecords = 65536;   // fits the whole compiled world (§3.1.2)
        size_t maxDepth = 64;
    };

    class DebugTypeTable {
    public:
        // Root types the closure expands from: every declared debug-local type
        // (codegen registers these at each __cajeta_dbg_local emission site).
        // Duplicates are harmless.
        void addRoot(const std::string& canonical);
        const std::vector<std::string>& roots() const { return roots_; }

        // Walk the reachable closure from the roots through the LIVE type world
        // (cold build only) and fill a record per type: field offsets from
        // getFieldLlvmIndex + getStructLayout, element strides from the element
        // LLVM type, storage from the value-type rule. Existing records are kept
        // (a rebuild is additive), so a second call after more roots extends the
        // table. Types that do not resolve are skipped, not faked.
        void buildFromTypeWorld(const llvm::DataLayout& dl,
                                const BuildOptions& opts = BuildOptions{});

        void put(TypeRecord rec);

        // File `rec` under an explicit lookup key (an alias: the asked-for
        // name or a short name, where key != rec.canonical). put() files under
        // the canonical name; the sidecar loader restores alias entries with
        // this.
        void putAs(const std::string& key, TypeRecord rec);

        // Look up by canonical name. NEVER faults: a miss is nullptr, which the
        // decoder renders as `<unknown>`/no children (spec §2.2.3, §5.1.1).
        const TypeRecord* find(const std::string& canonical) const;

        size_t size() const { return records_.size(); }
        bool empty() const { return records_.empty(); }
        void clear();

        // Every carried type name, ascending — the serialization surface.
        std::vector<std::string> names() const;

        // The String decode ABI (filled by buildFromTypeWorld, carried by the
        // sidecar). `valid` is false when String was not resolvable, in which
        // case a String renders as `<string?>` rather than reading garbage.
        const StringAbi& stringAbi() const { return stringAbi_; }
        void setStringAbi(StringAbi abi) { stringAbi_ = abi; }

        // Reachable types a bound dropped, in discovery order (spec §5.1.3).
        const std::vector<std::string>& bounded() const { return bounded_; }

        // The vtable map: symbol -> {runtime canonical, sub-object offset}.
        // Filled by buildFromTypeWorld from the REAL globals' names; restored
        // warm by the sidecar loader via putVtable.
        const std::map<std::string, VtableEntry>& vtables() const {
            return vtables_;
        }
        void putVtable(const std::string& symbol, VtableEntry entry);

    private:
        std::unordered_map<std::string, TypeRecord> records_;
        std::vector<std::string> roots_;
        std::vector<std::string> bounded_;
        std::map<std::string, VtableEntry> vtables_;
        StringAbi stringAbi_;
    };

    // Process-global table for the active debug compile / debug session:
    // populated from the type world cold, from the sidecar warm, read by the
    // inspection bridge either way. Single-threaded codegen, no synchronization.
    DebugTypeTable& globalDebugTypeTable();

    // Type-table sidecar (debug-type-sidecar §3.1) — the persistence pair for
    // the whole-program cache slot, in the dbgloc sidecar's serializer style
    // (versioned header line, one tab-separated record per line, tab/newline/
    // backslash escaped). write returns false on I/O failure. load returns
    // false — and leaves `into` EMPTY, never partial (§3.1.2/3.1.3) — on a
    // missing file, an unknown schema major, or any truncation/corruption;
    // callers treat false as "no sidecar" (a slot miss under -g, Unit 4).
    bool writeTypeSidecar(const std::string& path, const DebugTypeTable& table);
    bool loadTypeSidecar(const std::string& path, DebugTypeTable& into);

} // namespace cajeta::dbg
