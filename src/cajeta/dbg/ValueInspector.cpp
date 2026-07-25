#include "cajeta/dbg/ValueInspector.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <cstdint>
#include <cstdio>
#include <set>

#include "cajeta/dbg/DebugVars.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

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

ValueInspector::ValueInspector(const llvm::DataLayout& dl) : dl_(dl) {}

bool ValueInspector::canResolve(const std::string& type) const {
    return cajeta::CajetaType::of(type) != nullptr;
}

namespace {
    // String field byte offsets from the live layout (spec §2.4), derived
    // exactly as deriveEntryArgsABI does — nothing hardcoded.
    struct StrABI {
        bool valid = false;
        int64_t offLenTag = 0, offAux = 0, offBase = 0;
    };
    StrABI stringABI(const llvm::DataLayout& dl) {
        StrABI abi;
        auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(
            cajeta::CajetaType::of("cajeta.lang.String"));
        if (!klass) return abi;
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(klass->getLlvmType());
        if (!st || st->isOpaque() || st->getNumElements() < 4) return abi;
        const llvm::StructLayout* sl = dl.getStructLayout(st);
        abi.offLenTag = static_cast<int64_t>(sl->getElementOffset(1));
        abi.offAux    = static_cast<int64_t>(sl->getElementOffset(2));
        abi.offBase   = static_cast<int64_t>(sl->getElementOffset(3));
        abi.valid = true;
        return abi;
    }
} // namespace

std::string ValueInspector::decodeString(void* slot) {
    if (!slot) return "<null>";
    // The slot holds the String pointer; the instance carries the fields.
    void* inst = *reinterpret_cast<void**>(slot);
    if (!inst) return "<null>";
    StrABI abi = stringABI(dl_);
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

// Geometry of one array element, mirroring CajetaArray::getElementLlvmType so
// the stride the bridge walks is the stride the JIT'd code stores at. Resolved
// from the element type name only (array types are not interned in the name
// map, so `CajetaType::of("int32[]")` is null — we strip and resolve the
// element). No CajetaArray is constructed: that would mutate the live type
// registry mid-stop.
ValueInspector::ArrayInfo
ValueInspector::resolveArrayElement(const std::string& arrayType) {
    ArrayInfo info;
    if (arrayType.empty() || arrayType.back() != ']') return info;
    auto lb = arrayType.find_last_of('[');
    if (lb == std::string::npos) return info;
    std::string elemName = arrayType.substr(0, lb);
    if (elemName.empty()) return info;

    // A nested array element (`int32[][]` → `int32[]`) is itself a heap
    // reference: a pointer slot.
    if (elemName.back() == ']') {
        info.ok = true;
        info.elemType = elemName;
        info.storage = Storage::Pointer;
        info.stride = dl_.getPointerSize();
        return info;
    }

    auto elem = cajeta::CajetaType::of(elemName);
    if (!elem) {
        // Unresolved user type: assume a pointer slot so we only ever read a
        // pointer, never fabricate an inline width.
        info.ok = true;
        info.elemType = elemName;
        info.storage = Storage::Pointer;
        info.stride = dl_.getPointerSize();
        return info;
    }
    info.elemType = elem->toCanonical();

    if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(elem)) {
        CajetaTypeFlags flags = klass->getTypeFlags();
        bool pointerSlot =
            klass->isWildcardInstantiation() ||
            (!klass->isInterface() && (flags & STRUCT_FLAG) == 0 &&
             (flags & PRIMITIVE_FLAG) == 0);
        if (pointerSlot) {
            info.storage = Storage::Pointer;
            info.stride = dl_.getPointerSize();
        } else {
            // STRUCT_FLAG classes keep their own struct slot. A value type is
            // stored inline; String (a reference class with a struct slot)
            // keeps the String* at the slot base — a pointer read.
            llvm::Type* elemLlvm = klass->getLlvmType();
            if (!elemLlvm) return info;  // ok stays false
            info.storage = klass->isValueType() ? Storage::Inline
                                                : Storage::Pointer;
            info.stride = dl_.getTypeAllocSize(elemLlvm);
        }
    } else {
        // Primitive: inline bytes.
        llvm::Type* elemLlvm = elem->getLlvmType();
        if (!elemLlvm) return info;  // ok stays false
        info.storage = Storage::Inline;
        info.stride = dl_.getTypeAllocSize(elemLlvm);
    }
    info.ok = info.stride > 0;
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

// Inline-vs-pointer storage of a field/element by its declared type, mirroring
// how the layout stores it: a value type and a primitive are inline bytes; a
// String, reference class, or array holds the instance/header pointer at the
// slot base. inspect(type, addr) decodes either uniformly.
static Storage storageOfType(const cajeta::CajetaTypePtr& t) {
    if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(t))
        return klass->isValueType() ? Storage::Inline : Storage::Pointer;
    return Storage::Inline;  // primitive
}

// Collect a class's non-static fields in layout order — inherited (ancestors,
// recursively) before own — deduped so a shared vbase field appears once.
static void collectFields(cajeta::CajetaClass* cls,
                          std::vector<cajeta::StructurePropertyPtr>& out,
                          std::set<void*>& seen) {
    if (!cls) return;
    for (auto& parent : cls->getSuperClasses())
        collectFields(parent.get(), out, seen);
    for (auto& prop : cls->getPropertyList()) {
        if (prop->isStatic()) continue;
        if (seen.insert(prop.get()).second) out.push_back(prop);
    }
}

std::vector<InspectedChild>
ValueInspector::objectChildren(const std::string& type, void* addr) {
    std::vector<InspectedChild> out;
    auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        cajeta::CajetaType::of(type));
    if (!klass || !addr) return out;

    // A reference class slot holds the instance pointer; a value type is stored
    // inline (the slot IS the instance). Null pointer → no children, no deref.
    void* inst = klass->isValueType() ? addr : *reinterpret_cast<void**>(addr);
    if (!inst) return out;

    auto* st = llvm::dyn_cast_or_null<llvm::StructType>(klass->getLlvmType());
    if (!st || st->isOpaque()) return out;
    const llvm::StructLayout* sl = dl_.getStructLayout(st);

    std::vector<cajeta::StructurePropertyPtr> fields;
    std::set<void*> seen;
    collectFields(klass.get(), fields, seen);

    for (const auto& f : fields) {
        int slot = klass->getFieldLlvmIndex(f);
        if (slot < 0 || slot >= static_cast<int>(st->getNumElements())) continue;
        InspectedChild child;
        child.name = f->getName();
        child.type = f->getType() ? f->getType()->toCanonical() : "";
        child.storage = storageOfType(f->getType());
        // Address from the DataLayout offset end-to-end — never index*8 — so a
        // field after an interior secondary-vtable word lands on its own bytes.
        child.addr = reinterpret_cast<char*>(inst) + sl->getElementOffset(slot);
        out.push_back(std::move(child));
    }
    return out;
}

namespace {
    // Fully-qualified stdlib collection types with a logical debug view.
    const char* kArrayListFqn = "cajeta.collection.ArrayList";
    const char* kHashMapFqn = "cajeta.collection.HashMap";

    // The base type name with any generic arguments stripped:
    // "cajeta.collection.ArrayList<int32>" → "cajeta.collection.ArrayList".
    std::string typeBase(const std::string& t) {
        auto lt = t.find('<');
        return lt == std::string::npos ? t : t.substr(0, lt);
    }

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
    const std::string base = typeBase(type);
    if (base == kArrayListFqn) return arrayListChildren(type, addr, start, pageSize);
    if (base == kHashMapFqn)   return hashMapChildren(type, addr, start, pageSize);
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

    auto ct = cajeta::CajetaType::of(type);
    if (!ct) {
        r.kind = ValueKind::Unknown;
        r.summary = "<unknown>";
        return r;
    }

    // String is a leaf that renders its text (§2.4).
    if (ct->toCanonical() == "cajeta.lang.String") {
        r.kind = ValueKind::Leaf;
        r.summary = decodeString(addr);
        return r;
    }

    // Any other class/interface is an aggregate.
    r.kind = ValueKind::Aggregate;

    // A registered collection summarizes by its logical contents: an ArrayList
    // inline-or-counts its elements like an array; a HashMap counts its entries.
    const std::string base = typeBase(type);
    if (base == kArrayListFqn || base == kHashMapFqn) {
        if (auto page = collectionChildren(type, addr, 0, 6)) {
            int64_t count = static_cast<int64_t>(page->children.size()) +
                            static_cast<int64_t>(page->remaining);
            if (base == kHashMapFqn) {
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
        auto ct = cajeta::CajetaType::of(type);
        // String is a leaf (its value is its text), not an expandable object.
        if (!ct || ct->toCanonical() == "cajeta.lang.String") return page;
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
        page.children.push_back(std::move(child));
    }
    page.nextStart = static_cast<size_t>(end);
    page.remaining = static_cast<size_t>(length) - static_cast<size_t>(end);
    return page;
}

} // namespace cajeta::dbg
