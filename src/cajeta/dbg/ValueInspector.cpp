#include "cajeta/dbg/ValueInspector.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <cstdint>
#include <cstdio>

#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/dbg/DebugVars.h"

namespace cajeta::dbg {

std::string escapeAndQuote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[5];
                    std::snprintf(buf, sizeof buf, "\\x%02X",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

ValueInspector::ValueInspector(const llvm::DataLayout& dl,
                               const ResolvedTypeSymbols* symbols)
    : ValueInspector(dl, globalDebugTypeTable(), symbols) {}

ValueInspector::ValueInspector(const llvm::DataLayout& dl,
                               const DebugTypeTable& table,
                               const ResolvedTypeSymbols* symbols)
    : dl_(dl), table_(table), symbols_(symbols) {}

// The one narrowing seam (§2.1.3/2.1.5). Applies only to Pointer-storage
// object/collection rows: value types have no vtable word and their declared
// type is exact; leaves (String included) never narrow. The slot deref is the
// SAME one objectChildren always did — no new memory is trusted, only the
// already-read instance's first word is consulted, and an unmatched word
// changes nothing (§2.1.4).
ValueInspector::ResolvedObject
ValueInspector::resolveObject(const std::string& declared, void* addr) {
    ResolvedObject out;
    out.type = declared;
    out.rec = table_.find(declared);
    if (!out.rec || !addr) return out;
    if (out.rec->kind != TypeKind::Object &&
        out.rec->kind != TypeKind::Collection) return out;

    out.inst = out.rec->isValueType ? addr : *reinterpret_cast<void**>(addr);
    if (!out.inst || out.rec->isValueType || !symbols_) return out;

    uint64_t slot0 = *reinterpret_cast<uint64_t*>(out.inst);
    auto it = symbols_->vtableByAddr.find(slot0);
    if (it == symbols_->vtableByAddr.end()) return out;   // silent fallback
    const TypeRecord* runtime = table_.find(it->second.canonical);
    if (!runtime || (runtime->kind != TypeKind::Object &&
                     runtime->kind != TypeKind::Collection)) return out;

    // A secondary ($as$) vtable means this is a base-view interior pointer:
    // rebase to the object base so the runtime type's offsets apply.
    out.rec = runtime;
    out.type = it->second.canonical;
    out.inst = reinterpret_cast<char*>(out.inst)
             - it->second.subObjectByteOffset;
    return out;
}

std::string ValueInspector::runtimeType(const std::string& declared,
                                        void* addr) {
    return resolveObject(declared, addr).type;
}

void ValueInspector::narrowRow(InspectedChild& child) {
    if (child.storage != Storage::Pointer || !child.addr) return;
    child.type = runtimeType(child.type, child.addr);
}

bool ValueInspector::canResolve(const std::string& type) const {
    return isPrimitiveTypeName(type) || table_.find(type) != nullptr;
}

std::string ValueInspector::decodeString(void* slot) {
    if (!slot) return "<null>";
    // The slot holds the String pointer; the instance carries the fields.
    void* inst = *reinterpret_cast<void**>(slot);
    if (!inst) return "<null>";
    // The String ABI rides the table (spec §2.1.4), so the decode survives a
    // cache hit where no type world exists to re-derive it from.
    const StringAbi& abi = table_.stringAbi();
    if (!abi.valid) return "<string?>";

    char* base = reinterpret_cast<char*>(inst);
    uint32_t lenTag = *reinterpret_cast<uint32_t*>(base + abi.offLenTag);
    uint32_t byteLen = lenTag & 0x1FFFFFFFu;  // high bits are rc/borrow/static.

    // Cap an implausible length rather than read unbounded memory (§2.4.3).
    constexpr uint32_t kMaxDecode = 4096;
    bool truncated = byteLen > kMaxDecode;
    uint32_t take = truncated ? kMaxDecode : byteLen;

    std::string text;
    if (byteLen <= 12) {
        // Inline: the ≤12 payload bytes span {aux, base} contiguously.
        const char* p = base + abi.offAux;
        text.assign(p, p + take);
    } else {
        // Windowed: aux = window byte offset into the root array's data,
        // base field = root array header {i64 count, data}; text at data+aux.
        int32_t aux = *reinterpret_cast<int32_t*>(base + abi.offAux);
        void* root = *reinterpret_cast<void**>(base + abi.offBase);
        if (!root) return "<null>";
        const char* data = reinterpret_cast<char*>(root) + 8 + aux;
        text.assign(data, data + take);
    }

    std::string quoted = escapeAndQuote(text);
    if (truncated) quoted.insert(quoted.size() - 1, "…");  // before closing "
    return quoted;
}

// Element geometry straight off the array's table record — the stride stored
// there was resolved from the element's LLVM type when the table was built, so
// it is the stride the JIT'd code stores at. A type the table does not carry
// yields ok=false: the decoder shows no children rather than walking a guessed
// stride over live memory.
ValueInspector::ArrayInfo
ValueInspector::resolveArrayElement(const std::string& arrayType) {
    ArrayInfo info;
    const TypeRecord* rec = table_.find(arrayType);
    if (!rec || rec->kind != TypeKind::Array || rec->elem.stride == 0)
        return info;
    info.ok = true;
    info.elemType = rec->elem.type;
    info.stride = rec->elem.stride;
    info.storage = rec->elem.storage;
    return info;
}

bool ValueInspector::openArray(void* addr, char** data, int64_t* length) {
    if (!addr) return false;
    // The slot holds the array header pointer (`T[]` is a heap reference).
    void* header = *reinterpret_cast<void**>(addr);
    if (!header) return false;
    // Header: { i64 size, [0 x T] data }; length at offset 0, data past it.
    int64_t len = *reinterpret_cast<int64_t*>(header);
    if (len < 0) len = 0;
    *length = len;
    *data = reinterpret_cast<char*>(header) + 8;
    return true;
}

std::string ValueInspector::arraySummary(const ArrayInfo& info, char* data,
                                          int64_t length) {
    // >5 elements collapse to a count (§4.1.3).
    if (length > 5) return "{" + std::to_string(length) + " elements}";
    constexpr size_t kMaxInline = 60;
    std::string out = "[";
    for (int64_t i = 0; i < length; i++) {
        if (i) out += ", ";
        void* slot = data + static_cast<uint64_t>(i) * info.stride;
        out += inspect(info.elemType, slot).summary;
        if (out.size() > kMaxInline)  // fall back to the count past the cap.
            return "{" + std::to_string(length) + " elements}";
    }
    out += "]";
    return out;
}

std::vector<InspectedChild>
ValueInspector::objectChildren(const std::string& type, void* addr) {
    std::vector<InspectedChild> out;
    // Runtime narrowing (§2.1.5): the effective record is the RUNTIME type's,
    // and `inst` is already rebased for a base-view interior pointer.
    ResolvedObject ro = resolveObject(type, addr);
    if (!ro.rec || !ro.inst) return out;

    for (const auto& f : ro.rec->fields) {
        InspectedChild child;
        child.name = f.name;
        child.type = f.type;
        child.storage = f.storage;
        // The record's offset came from the DataLayout when the table was
        // built — never index*8 — so a field after an interior secondary-vtable
        // word lands on its own bytes.
        child.addr = reinterpret_cast<char*>(ro.inst) + f.offset;
        narrowRow(child);
        out.push_back(std::move(child));
    }

    // Static rows, inline after the instance fields (§4.1.3), under the
    // RUNTIME type's view (§4.1.5) — ro.rec is already narrowed. The address
    // is the session-resolved global; an unresolved symbol drops the row.
    if (symbols_) {
        for (const auto& sf : ro.rec->statics) {
            auto it = symbols_->staticAddrs.find(sf.symbol);
            if (it == symbols_->staticAddrs.end() || !it->second) continue;
            // The static global stores primitives inline and everything else
            // as a pointer slot (getOrCreateStaticFieldGlobal). A @ValueType
            // static would need a deref-first shape the row model lacks —
            // skipped rather than misread.
            const TypeRecord* tr = table_.find(sf.type);
            bool isPrim = isPrimitiveTypeName(sf.type);
            if (!isPrim && tr && tr->isValueType) continue;
            InspectedChild child;
            child.name = sf.name;
            child.type = sf.type;
            child.addr = it->second;
            child.storage = isPrim ? Storage::Inline : Storage::Pointer;
            child.isStatic = true;
            narrowRow(child);
            out.push_back(std::move(child));
        }
    }
    return out;
}

namespace {
    // Read a small integer field (a backing size/capacity) by primitive type.
    // -1 for a non-integer (a fail-safe signal the view checks).
    int64_t readIntByType(const std::string& t, void* addr) {
        if (!addr) return -1;
        if (t == "int32")  return *reinterpret_cast<int32_t*>(addr);
        if (t == "int64")  return *reinterpret_cast<int64_t*>(addr);
        if (t == "int16")  return *reinterpret_cast<int16_t*>(addr);
        if (t == "int8")   return *reinterpret_cast<int8_t*>(addr);
        if (t == "uint32") return *reinterpret_cast<uint32_t*>(addr);
        if (t == "uint64") return static_cast<int64_t>(*reinterpret_cast<uint64_t*>(addr));
        if (t == "uint16") return *reinterpret_cast<uint16_t*>(addr);
        if (t == "uint8")  return *reinterpret_cast<uint8_t*>(addr);
        return -1;
    }

    const InspectedChild* findChild(const std::vector<InspectedChild>& cs,
                                    const std::string& name) {
        for (const auto& c : cs) if (c.name == name) return &c;
        return nullptr;
    }
} // namespace

std::optional<ChildPage>
ValueInspector::collectionChildren(const std::string& type, void* addr,
                                   size_t start, size_t pageSize) {
    // The record carries the collection kind — the logical view is selected
    // from the table, not by matching the type name at a stop.
    const TypeRecord* rec = table_.find(type);
    if (!rec) return std::nullopt;
    if (rec->collectionKind == CollectionKind::ArrayList)
        return arrayListChildren(type, addr, start, pageSize);
    if (rec->collectionKind == CollectionKind::HashMap)
        return hashMapChildren(type, addr, start, pageSize);
    return std::nullopt;
}

std::optional<ChildPage>
ValueInspector::arrayListChildren(const std::string& type, void* addr,
                                  size_t start, size_t pageSize) {
    // Read the backing store BY DECLARED NAME (§8.3): `data` (the T[]) and
    // `sizeCount` (the logical length). Any missing field → fail safe.
    auto fields = objectChildren(type, addr);
    const auto* dataF = findChild(fields, "data");
    const auto* sizeF = findChild(fields, "sizeCount");
    if (!dataF || !sizeF || !isPrimitiveTypeName(sizeF->type)) return std::nullopt;

    int64_t size = readIntByType(sizeF->type, sizeF->addr);
    if (size < 0) return std::nullopt;

    ArrayInfo info = resolveArrayElement(dataF->type);
    char* dataBase = nullptr;
    int64_t headerLen = 0;
    if (!info.ok || !openArray(dataF->addr, &dataBase, &headerLen))
        return std::nullopt;

    // Enumerate data[0..sizeCount) — the logical elements, not the capacity —
    // clamped to the real backing length for safety.
    int64_t count = std::min<int64_t>(size, headerLen);
    if (pageSize == 0) pageSize = kDefaultPageSize;

    ChildPage page;
    page.nextStart = start;
    if (static_cast<int64_t>(start) >= count) return page;
    uint64_t end = std::min<uint64_t>(start + pageSize, static_cast<uint64_t>(count));
    for (uint64_t i = start; i < end; i++) {
        InspectedChild child;
        child.name = "[" + std::to_string(i) + "]";
        child.type = info.elemType;
        child.storage = info.storage;
        child.addr = dataBase + i * info.stride;
        narrowRow(child);   // element rows report their RUNTIME type (§2.1.5)
        page.children.push_back(std::move(child));
    }
    page.nextStart = static_cast<size_t>(end);
    page.remaining = static_cast<size_t>(count) - static_cast<size_t>(end);
    return page;
}

std::optional<ChildPage>
ValueInspector::hashMapChildren(const std::string& type, void* addr,
                                size_t start, size_t pageSize) {
    // Backing by declared name: `slots` (MapEntry<K,V>[] inline), `ctrl` (the
    // SwissTable control bytes, one per slot), `cap` (slot count). Missing any
    // → fail safe to the object-field view.
    auto fields = objectChildren(type, addr);
    const auto* slotsF = findChild(fields, "slots");
    const auto* ctrlF = findChild(fields, "ctrl");
    const auto* capF = findChild(fields, "cap");
    if (!slotsF || !ctrlF || !capF) return std::nullopt;

    int64_t cap = readIntByType(capF->type, capF->addr);
    char* ctrlBase = nullptr;
    int64_t ctrlLen = 0;
    if (!openArray(ctrlF->addr, &ctrlBase, &ctrlLen)) return std::nullopt;
    ArrayInfo slotsInfo = resolveArrayElement(slotsF->type);
    char* slotsBase = nullptr;
    int64_t slotsLen = 0;
    if (!slotsInfo.ok || !openArray(slotsF->addr, &slotsBase, &slotsLen))
        return std::nullopt;

    // A slot is live when its control byte has the high bit clear (FULL); EMPTY
    // (0x80) and DELETED (0xFE) both have it set. Only the first `cap` ctrl
    // bytes are real slots (the tail 16 mirror the head).
    int64_t n = std::min<int64_t>(cap, std::min<int64_t>(ctrlLen, slotsLen));
    std::vector<int64_t> live;
    for (int64_t i = 0; i < n; i++)
        if ((static_cast<uint8_t>(ctrlBase[i]) & 0x80u) == 0) live.push_back(i);

    if (pageSize == 0) pageSize = kDefaultPageSize;
    ChildPage page;
    page.nextStart = start;
    if (start >= live.size()) return page;
    size_t end = std::min<size_t>(start + pageSize, live.size());
    for (size_t idx = start; idx < end; idx++) {
        char* slotAddr = slotsBase + live[idx] * slotsInfo.stride;
        // slots[i] is an inline @ValueType MapEntry {key, val}.
        auto entry = objectChildren(slotsInfo.elemType, slotAddr);
        const auto* keyF = findChild(entry, "key");
        const auto* valF = findChild(entry, "val");
        if (!keyF || !valF) continue;   // layout drift → skip, never misread.
        InspectedChild child;
        child.name = inspect(keyF->type, keyF->addr).summary;  // labelled by key
        child.type = valF->type;
        child.addr = valF->addr;
        child.storage = valF->storage;
        narrowRow(child);   // value rows report their RUNTIME type (§2.1.5)
        page.children.push_back(std::move(child));
    }
    page.nextStart = end;
    page.remaining = live.size() - end;
    return page;
}

std::string ValueInspector::objectSummary(const std::string& type, void* addr) {
    // A brief field peek: the first few scalar (primitive) fields as {x=3, y=4},
    // length-capped; {…} when there is no cheap scalar (§4.1.4).
    auto fields = objectChildren(type, addr);
    constexpr size_t kMaxFields = 4;
    constexpr size_t kMaxLen = 60;
    std::string out = "{";
    size_t shown = 0;
    for (const auto& f : fields) {
        if (f.isStatic) continue;   // the peek is the INSTANCE's state (§4.1.3)
        if (!isPrimitiveTypeName(f.type)) continue;  // cheap scalars only
        if (shown) out += ", ";
        out += f.name + "=" + inspect(f.type, f.addr).summary;
        if (++shown >= kMaxFields || out.size() > kMaxLen) break;
    }
    if (shown == 0) return "{…}";
    out += "}";
    return out;
}

InspectedValue ValueInspector::inspect(const std::string& type, void* addr) {
    InspectedValue r;

    // Scalar leaf: rendered by width, exactly as today (§4.1.1).
    if (isPrimitiveTypeName(type)) {
        r.kind = ValueKind::Leaf;
        r.summary = formatValue(type, addr);
        return r;
    }

    // An array is an aggregate regardless of whether its name resolves in the
    // registry — the `[]` suffix is authoritative.
    if (!type.empty() && type.back() == ']') {
        r.kind = ValueKind::Aggregate;
        char* data = nullptr;
        int64_t length = 0;
        ArrayInfo info = resolveArrayElement(type);
        if (!info.ok || !openArray(addr, &data, &length)) {
            r.summary = "<null>";
        } else {
            r.summary = arraySummary(info, data, length);
        }
        return r;
    }

    const TypeRecord* rec = table_.find(type);
    if (!rec) {
        // Not carried by the table: an honest unknown, never a guessed layout.
        r.kind = ValueKind::Unknown;
        r.summary = "<unknown>";
        return r;
    }

    // String is a leaf that renders its text (§2.4) — identified by the record's
    // own flag, not by matching a stdlib type name at a stop.
    if (rec->kind == TypeKind::Leaf) {
        r.kind = ValueKind::Leaf;
        r.summary = rec->isString ? decodeString(addr)
                                  : formatValue(rec->canonical, addr);
        return r;
    }

    // Any other class/interface is an aggregate.
    r.kind = ValueKind::Aggregate;

    // A registered collection summarizes by its logical contents: an ArrayList
    // inline-or-counts its elements like an array; a HashMap counts its entries.
    if (rec->collectionKind != CollectionKind::None) {
        if (auto page = collectionChildren(type, addr, 0, 6)) {
            int64_t count = static_cast<int64_t>(page->children.size()) +
                            static_cast<int64_t>(page->remaining);
            if (rec->collectionKind == CollectionKind::HashMap) {
                r.summary = "{" + std::to_string(count) + " entries}";
            } else if (count > 5) {
                r.summary = "{" + std::to_string(count) + " elements}";
            } else {
                std::string out = "[";
                for (size_t i = 0; i < page->children.size(); i++) {
                    if (i) out += ", ";
                    out += inspect(page->children[i].type,
                                   page->children[i].addr).summary;
                }
                out += "]";
                r.summary = out;
            }
            return r;
        }
    }

    // Otherwise a brief object field peek.
    r.summary = objectSummary(type, addr);
    return r;
}

ChildPage ValueInspector::children(const std::string& type, void* addr,
                                   size_t start, size_t pageSize) {
    ChildPage page;
    page.nextStart = start;

    // Objects: fields decoded in one shot (a class has few fields, no paging).
    if (type.empty() || type.back() != ']') {
        if (isPrimitiveTypeName(type)) return page;  // leaf, no children
        const TypeRecord* rec = table_.find(type);
        // A leaf (String renders its text) and an uncarried type both have no
        // children to enumerate.
        if (!rec || rec->kind == TypeKind::Leaf) return page;
        // A registered collection gets its logical, paged view; otherwise (or
        // on a layout mismatch) fall back to the raw object fields.
        if (auto coll = collectionChildren(type, addr, start, pageSize))
            return *coll;
        if (start == 0) page.children = objectChildren(type, addr);
        return page;
    }

    ArrayInfo info = resolveArrayElement(type);
    char* data = nullptr;
    int64_t length = 0;
    if (!info.ok || !openArray(addr, &data, &length)) return page;

    if (pageSize == 0) pageSize = kDefaultPageSize;
    uint64_t begin = static_cast<uint64_t>(start);
    if (begin >= static_cast<uint64_t>(length)) return page;
    uint64_t end = begin + pageSize;
    if (end > static_cast<uint64_t>(length)) end = static_cast<uint64_t>(length);

    for (uint64_t i = begin; i < end; i++) {
        InspectedChild child;
        child.name = "[" + std::to_string(i) + "]";
        child.type = info.elemType;
        child.storage = info.storage;
        // The child address is always the slot (data + i*stride); a Pointer
        // slot holds the instance pointer, an Inline slot holds the bytes.
        // inspect(child.type, child.addr) decodes either uniformly.
        child.addr = data + i * info.stride;
        narrowRow(child);   // element rows report their RUNTIME type (§2.1.5)
        page.children.push_back(std::move(child));
    }
    page.nextStart = static_cast<size_t>(end);
    page.remaining = static_cast<size_t>(length) - static_cast<size_t>(end);
    return page;
}

} // namespace cajeta::dbg
