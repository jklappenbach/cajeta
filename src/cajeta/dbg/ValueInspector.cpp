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

InspectedValue ValueInspector::inspect(const std::string& type, void* addr) {
    InspectedValue r;

    // Scalar leaf: rendered by width, exactly as today (§4.1.1).
    if (isPrimitiveTypeName(type)) {
        r.kind = ValueKind::Leaf;
        r.summary = formatValue(type, addr);
        return r;
    }

    // An array is an aggregate regardless of whether its name resolves in the
    // registry — the `[]` suffix is authoritative (children in Unit 2).
    if (!type.empty() && type.back() == ']') {
        r.kind = ValueKind::Aggregate;
        r.summary = "{…}";  // refined by array summary in §2.2.3.
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

} // namespace cajeta::dbg
