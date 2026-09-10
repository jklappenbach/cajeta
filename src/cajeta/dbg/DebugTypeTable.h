// The layout facts variable inspection needs, detached from the compiler's type
// world: one record per debug-reachable type with RESOLVED byte offsets, built
// cold from the live world, written to the slot sidecar, reloaded on a cache hit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm { class DataLayout; }

namespace cajeta::dbg {

    // How bytes sit at a slot: Pointer holds a pointer, Inline holds the bytes.
    enum class Storage { Inline, Pointer };

    // Leaf decodes by value; the other kinds expand from the record below.
    enum class TypeKind { Leaf, Array, Object, Collection };

    // A stdlib collection with a logical debug view (elements, not backing).
    enum class CollectionKind { None, ArrayList, HashMap };

    struct FieldRecord {
        std::string name;       // declared field name
        std::string type;       // canonical type name
        uint64_t offset = 0;    // DataLayout byte offset within the instance
        Storage storage = Storage::Inline;
    };

    // Decoded via its GLOBAL: `symbol` is stable, resolved once at launch.
    struct StaticFieldRecord {
        std::string name;
        std::string type;
        std::string symbol;
    };

    // An instance's slot-0 word matched here names its RUNTIME type; a secondary
    // (`$as$`) vtable's offset rebases a base-view pointer to the object base.
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
        // Carried as a FACT: the decoder never string-compares a stdlib FQN.
        bool isString = false;
        std::vector<FieldRecord> fields;   // Object/Collection, layout order
        // Inherited then own, displayed inline after the instance fields.
        std::vector<StaticFieldRecord> statics;
        ElemRecord elem;                   // Array
        CollectionKind collectionKind = CollectionKind::None;
    };

    // String's length/tag and payload offsets, carried so decode survives a hit.
    struct StringAbi {
        bool valid = false;
        uint64_t size = 0;
        uint64_t offLenTag = 0;
        uint64_t offAux = 0;
        uint64_t offBase = 0;
    };

    // Resolved once at init so decode does no lookups; a bad symbol is absent.
    struct ResolvedTypeSymbols {
        std::map<uint64_t, VtableEntry> vtableByAddr;
        std::unordered_map<std::string, void*> staticAddrs;
        bool empty() const {
            return vtableByAddr.empty() && staticAddrs.empty();
        }
    };

    // Bounds on the closure walk; whatever a bound drops lands in bounded().
    struct BuildOptions {
        size_t maxRecords = 65536;   // fits the whole compiled world (§3.1.2)
        size_t maxDepth = 64;
    };

    class DebugTypeTable {
    public:
        // A root the closure expands from; duplicates are harmless.
        void addRoot(const std::string& canonical);
        const std::vector<std::string>& roots() const { return roots_; }

        // One record per type reachable from the roots, through the LIVE type world
        // (cold only). Additive, and an unresolvable type is skipped, never faked.
        void buildFromTypeWorld(const llvm::DataLayout& dl,
                                const BuildOptions& opts = BuildOptions{});

        void put(TypeRecord rec);

        // Files `rec` under an alias key; put() files under the canonical name.
        void putAs(const std::string& key, TypeRecord rec);

        // Never faults: a miss is nullptr, which decodes as `<unknown>`.
        const TypeRecord* find(const std::string& canonical) const;

        size_t size() const { return records_.size(); }
        bool empty() const { return records_.empty(); }
        void clear();

        // Every carried type name, ascending — the serialization surface.
        std::vector<std::string> names() const;

        // `valid` false means String was unresolvable and renders `<string?>`.
        const StringAbi& stringAbi() const { return stringAbi_; }
        void setStringAbi(StringAbi abi) { stringAbi_ = abi; }

        // Reachable types a bound dropped, in discovery order (spec §5.1.3).
        const std::vector<std::string>& bounded() const { return bounded_; }

        // symbol -> {runtime canonical, sub-object offset}; restored via putVtable.
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

    // The process-global table, cold from the type world and warm from the
    // sidecar. Single-threaded codegen, so no synchronization.
    DebugTypeTable& globalDebugTypeTable();

    // The sidecar persistence pair: a versioned header plus one escaped,
    // tab-separated record per line. load returns false and leaves `into` EMPTY —
    // never partial — on a missing file, unknown schema major, or corruption.
    bool writeTypeSidecar(const std::string& path, const DebugTypeTable& table);
    bool loadTypeSidecar(const std::string& path, DebugTypeTable& into);

} // namespace cajeta::dbg
