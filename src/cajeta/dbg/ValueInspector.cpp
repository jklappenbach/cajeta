#include "cajeta/dbg/ValueInspector.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <cstdint>
#include <cstdio>

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

    // Any other class/interface is an aggregate (fields in Unit 3).
    r.kind = ValueKind::Aggregate;
    r.summary = "{…}";
    return r;
}

ChildPage ValueInspector::children(const std::string& type, void* addr,
                                   size_t start, size_t pageSize) {
    ChildPage page;
    page.nextStart = start;

    // Arrays only in Unit 2 (object/collection children are Units 3/7).
    if (type.empty() || type.back() != ']') return page;

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
