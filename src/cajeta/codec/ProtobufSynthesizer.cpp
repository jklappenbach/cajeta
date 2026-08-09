// Codec Phase 2.3 — Tier-1 protobuf typed-bind synthesizer (see header).

#include "ProtobufSynthesizer.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaType.h"
#include "../type/StructureProperty.h"
#include "../type/QualifiedName.h"
#include "../error/Diagnostics.h"

#include <iostream>
#include <sstream>

namespace cajeta {

    namespace {

    // Canonical of param `i`, or empty if out of range.
    std::string paramCanonAt(
            const std::vector<CajetaTypePtr>& paramTypes, size_t i) {
        if (i >= paramTypes.size()) return std::string();
        auto& p = paramTypes[i];
        if (!p || !p->getQName()) return std::string();
        return p->getQName()->toCanonical();
    }

    // How a bound field decodes off the cursor. Wire type is inferred from the
    // Cajeta field type (see the @ProtoField doc): integer/bool → VARINT,
    // String/bytes/message → LEN, float32/float64 → I32/I64 carrying raw
    // IEEE-754 bits (via Float32/Float64.toBits, which reinterpret rather than
    // convert). @ProtoField's `encoding` option then overrides the integer
    // choice with zigzag or fixed-width. A field type with no mapping is a
    // compile error, never a silent omission.
    enum class Decode {
        IntVarint,    // int8/16/32/64 + uint* — readVarint, cast to the field width
        ZigzagVarint, // sint32/sint64 — readZigzag, cast to the field width
        Fixed32Int,   // fixed32/sfixed32 — readFixed32 (wire type I32)
        Fixed64Int,   // fixed64/sfixed64 — readFixed64 (wire type I64)
        BoolVarint,   // boolean — readVarint != 0
        Float32Bits,  // float32 — I32 carrying raw IEEE-754 bits
        Float64Bits,  // float64 — I64 carrying raw IEEE-754 bits
        StringLen,    // cajeta.lang.String — readBytes → String
        BytesLen,     // int8[] — readBytes directly
        MessageLen,   // nested message (a class) — readBytes → recurse parse<Sub>
        Unsupported
    };

    // The `encoding = "..."` option on @ProtoField. Absent → Default, which
    // must reproduce pre-option behavior byte for byte.
    enum class Encoding { Default, Zigzag, Fixed };

    struct Bind {
        int number;            // explicit @ProtoField wire number
        std::string name;      // field name
        std::string canon;     // field type canonical (for the width cast)
        Decode decode;
    };

    bool isSignedIntCanon(const std::string& c) {
        return c == "int8" || c == "int16" || c == "int32" || c == "int64";
    }

    bool isIntCanon(const std::string& c) {
        return isSignedIntCanon(c) || c == "uint8" || c == "uint16"
            || c == "uint32" || c == "uint64";
    }

    // 32- vs 64-bit for the fixed forms. protobuf offers fixed32 and fixed64
    // only, so narrower fields ride in the 32-bit form.
    bool isWide64Canon(const std::string& c) {
        return c == "int64" || c == "uint64";
    }

    // Classify a field's type into a decode strategy.
    Decode classify(const CajetaTypePtr& ty, const std::string& canon) {
        // int8[] (LEN payload) — an array of int8.
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            auto el = arr->getElementType();
            if (el && el->getQName()
                    && el->getQName()->toCanonical() == "int8") {
                return Decode::BytesLen;
            }
            return Decode::Unsupported;
        }
        if (canon == "cajeta.lang.String") return Decode::StringLen;
        if (canon == "boolean") return Decode::BoolVarint;
        // protobuf `float` / `double` are I32 / I64 carrying raw IEEE-754 bits.
        // Float32/Float64.toBits reinterprets rather than converting — a
        // `(int32) someFloat` would truncate 0.5 to 0 and put that on the wire.
        if (canon == "float32") return Decode::Float32Bits;
        if (canon == "float64") return Decode::Float64Bits;
        if (canon == "int8" || canon == "int16" || canon == "int32"
                || canon == "int64" || canon == "uint8" || canon == "uint16"
                || canon == "uint32" || canon == "uint64") {
            return Decode::IntVarint;
        }
        // A non-String class field is a nested message — decode its LEN payload
        // and recurse through the synthesizer (Protobuf.parse<Sub>). Primitives
        // (already handled above) are not CajetaClass, so they don't reach here.
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return Decode::MessageLen;
        return Decode::Unsupported;
    }

    // The @ProtoField(N) wire number on a field, or -1 if absent.
    int protoFieldNumber(const StructurePropertyPtr& prop) {
        if (!prop) return -1;
        if (auto ann = prop->findAnnotation("ProtoField")) {
            return (int) ann->getInt("value", -1);
        }
        return -1;
    }

    // Apply @ProtoField's `encoding` option to the type-inferred decode kind.
    // An option the field's type cannot carry is a compile error, not a silent
    // fallback to the default — a wrong encoding is a wire-format bug that would
    // otherwise surface as garbage at the far end.
    Decode applyEncoding(const CajetaClassPtr& T,
                         const StructurePropertyPtr& prop,
                         const std::string& canon, Decode base) {
        auto ann = prop ? prop->findAnnotation("ProtoField") : nullptr;
        if (!ann) return base;
        std::string enc = ann->getString("encoding");
        if (enc.empty()) return base;

        const std::string where = T->getQName()->toCanonical() + "."
            + prop->getName() + " (" + canon + ")";
        const int line = prop->getDeclLine();
        const int col = prop->getDeclColumn();

        if (enc == "zigzag") {
            if (base != Decode::IntVarint || !isSignedIntCanon(canon)) {
                reportOrThrow(line, col, "CAJETA_ERROR_PROTO_ENCODING",
                    "@ProtoField(encoding = \"zigzag\") on " + where
                    + " — zigzag maps protobuf sint32/sint64 and applies only "
                      "to a signed integer field");
                return base;
            }
            return Decode::ZigzagVarint;
        }
        if (enc == "fixed") {
            if (base != Decode::IntVarint || !isIntCanon(canon)) {
                reportOrThrow(line, col, "CAJETA_ERROR_PROTO_ENCODING",
                    "@ProtoField(encoding = \"fixed\") on " + where
                    + " — fixed maps protobuf fixed32/fixed64 and applies only "
                      "to an integer field");
                return base;
            }
            return isWide64Canon(canon) ? Decode::Fixed64Int
                                        : Decode::Fixed32Int;
        }
        reportOrThrow(line, col, "CAJETA_ERROR_PROTO_ENCODING",
            "@ProtoField(encoding = \"" + enc + "\") on " + where
            + " — unknown encoding; expected \"zigzag\" or \"fixed\"");
        return base;
    }

    std::vector<Bind> collectBinds(const CajetaClassPtr& T);

    // Synthesize `parse(int8[] bytes, int64 length) -> #T` for message T.
    std::string synthesizeMessageParseBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();

        // Shared with the encode arm, so the two can never disagree on which
        // fields bind or how each is encoded.
        std::vector<Bind> binds = collectBinds(T);

        const std::string PC = "dev.cajeta.codec.protobuf.ProtobufCursor";
        std::ostringstream os;
        os << "public static #" << Tc << " parse(int8[] bytes, int64 length) {\n";
        os << "    " << PC << " cur = heap " << PC << "(bytes, length);\n";
        os << "    " << Tc << " e = heap " << Tc << "();\n";
        for (auto& b : binds) {
            const std::string slot = "s_" + b.name;
            os << "    int32 " << slot << " = cur.slotOf((int32) "
               << b.number << ");\n";
            os << "    if (" << slot << " >= (int32) 0) {\n";
            switch (b.decode) {
                case Decode::IntVarint:
                    if (b.canon == "int64") {
                        os << "        e." << b.name << " = cur.readVarint("
                           << slot << ");\n";
                    } else {
                        os << "        e." << b.name << " = (" << b.canon
                           << ") cur.readVarint(" << slot << ");\n";
                    }
                    break;
                case Decode::ZigzagVarint:
                    if (b.canon == "int64") {
                        os << "        e." << b.name << " = cur.readZigzag("
                           << slot << ");\n";
                    } else {
                        os << "        e." << b.name << " = (" << b.canon
                           << ") cur.readZigzag(" << slot << ");\n";
                    }
                    break;
                case Decode::Fixed32Int:
                    if (b.canon == "int32") {
                        os << "        e." << b.name << " = cur.readFixed32("
                           << slot << ");\n";
                    } else {
                        os << "        e." << b.name << " = (" << b.canon
                           << ") cur.readFixed32(" << slot << ");\n";
                    }
                    break;
                case Decode::Fixed64Int:
                    if (b.canon == "int64") {
                        os << "        e." << b.name << " = cur.readFixed64("
                           << slot << ");\n";
                    } else {
                        os << "        e." << b.name << " = (" << b.canon
                           << ") cur.readFixed64(" << slot << ");\n";
                    }
                    break;
                case Decode::BoolVarint:
                    os << "        e." << b.name << " = cur.readVarint("
                       << slot << ") != (int64) 0;\n";
                    break;
                case Decode::Float32Bits:
                    os << "        e." << b.name
                       << " = Float32.fromBits(cur.readFixed32("
                       << slot << "));\n";
                    break;
                case Decode::Float64Bits:
                    os << "        e." << b.name
                       << " = Float64.fromBits(cur.readFixed64("
                       << slot << "));\n";
                    break;
                case Decode::StringLen: {
                    const std::string bv = "b_" + b.name;
                    os << "        int8[] " << bv << " = cur.readBytes("
                       << slot << ");\n";
                    // Length BEFORE the ctor adopts #bv; the String local
                    // then surrenders its title into the field ('#') — the
                    // 0.9 rules treat a plain store as a lend of a dying temp.
                    os << "        int32 n_" << b.name << " = (int32) "
                       << bv << ".count();\n";
                    os << "        String s_" << b.name
                       << " = heap cajeta.lang.String(#" << bv
                       << ", n_" << b.name << ");\n";
                    os << "        e." << b.name << " = #s_" << b.name << ";\n";
                    break;
                }
                case Decode::BytesLen: {
                    const std::string bv = "b_" + b.name;
                    os << "        int8[] " << bv << " = cur.readBytes("
                       << slot << ");\n";
                    os << "        e." << b.name << " = #" << bv << ";\n";
                    break;
                }
                case Decode::MessageLen: {
                    // Nested message: read the LEN payload, recurse through the
                    // synthesizer. Same-class static call → short `Protobuf`
                    // receiver (a fully-qualified static call NULL_OPERANDs).
                    const std::string bv = "b_" + b.name;
                    os << "        int8[] " << bv << " = cur.readBytes("
                       << slot << ");\n";
                    os << "        " << b.canon << " m_" << b.name
                       << " = Protobuf.parse<" << b.canon << ">(" << bv
                       << ", (int64) " << bv << ".count());\n";
                    os << "        e." << b.name << " = #m_" << b.name << ";\n";
                    break;
                }
                case Decode::Unsupported:
                    break;
            }
            os << "    }\n";
        }
        os << "    return #e;\n";
        os << "}\n";
        return os.str();
    }

    // Synthesize `parse(int8[] bytes, int64 length) -> #E[]` for a length-
    // delimited stream of E messages (each a varint-length-prefixed frame — the
    // de-facto protobuf "delimited" framing). Two passes: count frames, then bind
    // each via the single-message `parse<E>` over a copied slice.
    std::string synthesizeStreamParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";
        // Pass 1: count frames.
        os << "    int32 count = 0;\n";
        os << "    int64 p = (int64) 0;\n";
        os << "    while (p < length) {\n";
        os << "        int64 fl = ProtobufWire.decodeVarint(bytes, p);\n";
        os << "        int64 hl = ProtobufWire.varintLen(bytes, p);\n";
        os << "        p = p + hl + fl;\n";
        os << "        count = count + 1;\n";
        os << "    }\n";
        // Pass 2: allocate + bind each frame.
        os << "    " << Ec << "[] outv = heap " << Ec << "[count];\n";
        os << "    p = (int64) 0;\n";
        os << "    int32 i = 0;\n";
        os << "    while (p < length) {\n";
        os << "        int64 fl = ProtobufWire.decodeVarint(bytes, p);\n";
        os << "        int64 hl = ProtobufWire.varintLen(bytes, p);\n";
        os << "        int64 start = p + hl;\n";
        os << "        int32 fln = (int32) fl;\n";
        os << "        int8[] frame = heap int8[fln];\n";
        os << "        int32 k = 0;\n";
        os << "        while (k < fln) {\n";
        // hoist the compound index to a named local (inline `bytes[start+(cast)k]`
        // in a hot loop miscompiles — the compound-index-expr gotcha).
        os << "            int64 si = start + (int64) k;\n";
        os << "            int8 fb = bytes[si];\n";
        os << "            frame[k] = fb;\n";
        os << "            k = k + 1;\n";
        os << "        }\n";
        os << "        " << Ec << " m = Protobuf.parse<" << Ec << ">(frame, fl);\n";
        os << "        outv[i] = #m;\n";
        os << "        i = i + 1;\n";
        os << "        p = start + fl;\n";
        os << "    }\n";
        os << "    return #outv;\n";
        os << "}\n";
        return os.str();
    }

    // Collect the bindable @ProtoField fields of T (shared by encode + decode).
    std::vector<Bind> collectBinds(const CajetaClassPtr& T) {
        std::vector<Bind> binds;
        for (auto& prop : T->getPropertyList()) {
            if (!prop) continue;
            int number = protoFieldNumber(prop);
            if (number < 0) continue;
            auto ty = prop->getType();
            if (!ty || !ty->getQName()) continue;
            const std::string canon = ty->getQName()->toCanonical();
            Decode d = classify(ty, canon);
            if (d == Decode::Unsupported) {
                // Previously `continue` — the field was dropped from both the
                // parse and the encode arm with no diagnostic anywhere. A
                // @ProtoField the author explicitly numbered would simply not
                // appear on the wire, and the far end would see it as absent
                // and substitute a default. Failing the build is the only
                // honest answer: the author asked for a field the codec cannot
                // carry, and silence turns that into lost data.
                reportOrThrow(prop->getDeclLine(), prop->getDeclColumn(),
                    "CAJETA_ERROR_PROTO_FIELD_TYPE",
                    "@ProtoField(" + std::to_string(number) + ") on "
                    + T->getQName()->toCanonical() + "." + prop->getName()
                    + " — no protobuf wire mapping for type '" + canon
                    + "'. Supported: integer, boolean, float32/float64, String, "
                      "int8[] (bytes), and a nested message class.");
                continue;
            }
            d = applyEncoding(T, prop, canon, d);
            binds.push_back({number, prop->getName(), canon, d});
        }
        return binds;
    }

    // Synthesize `toBytes(T value) -> #int8[]` for message T — the encode mirror
    // of synthesizeMessageParseBody. Field accesses are hoisted to locals before
    // the writer call (the field-arg-as-method-arg codegen gotcha).
    std::string synthesizeMessageEncodeBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        std::vector<Bind> binds = collectBinds(T);
        const std::string PW = "dev.cajeta.codec.protobuf.ProtobufWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Tc << " value) {\n";
        os << "    " << PW << " w = heap " << PW << "();\n";
        for (auto& b : binds) {
            const std::string fv = "f_" + b.name;
            const std::string tag = "(int32) " + std::to_string(b.number);
            switch (b.decode) {
                case Decode::IntVarint:
                    os << "    int64 " << fv << " = (int64) value." << b.name << ";\n";
                    os << "    w.writeVarintField(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::ZigzagVarint:
                    os << "    int64 " << fv << " = (int64) value." << b.name << ";\n";
                    os << "    w.writeZigzagField(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::Fixed32Int:
                    os << "    int32 " << fv << " = (int32) value." << b.name << ";\n";
                    os << "    w.writeFixed32Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::Fixed64Int:
                    os << "    int64 " << fv << " = (int64) value." << b.name << ";\n";
                    os << "    w.writeFixed64Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::BoolVarint:
                    // `if`, not a ternary — `boolean ? intLit : intLit` miscompiles.
                    os << "    int64 " << fv << " = (int64) 0;\n";
                    os << "    if (value." << b.name << ") { " << fv << " = (int64) 1; }\n";
                    os << "    w.writeVarintField(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::Float32Bits:
                    os << "    int32 " << fv << " = Float32.toBits(value."
                       << b.name << ");\n";
                    os << "    w.writeFixed32Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::Float64Bits:
                    os << "    int64 " << fv << " = Float64.toBits(value."
                       << b.name << ");\n";
                    os << "    w.writeFixed64Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::StringLen:
                    os << "    cajeta.lang.String " << fv << " = value." << b.name << ";\n";
                    os << "    if (" << fv << " != null) {\n";
                    os << "        int8[] sb_" << b.name << " = " << fv << ".toBytes();\n";
                    os << "        w.writeLenField(" << tag << ", sb_" << b.name
                       << ", (int32) sb_" << b.name << ".count());\n";
                    os << "    }\n";
                    break;
                case Decode::BytesLen:
                    os << "    int8[] " << fv << " = value." << b.name << ";\n";
                    os << "    if (" << fv << " != null) {\n";
                    os << "        w.writeLenField(" << tag << ", " << fv
                       << ", (int32) " << fv << ".count());\n";
                    os << "    }\n";
                    break;
                case Decode::MessageLen:
                    os << "    " << b.canon << " " << fv << " = value." << b.name << ";\n";
                    os << "    if (" << fv << " != null) {\n";
                    os << "        int8[] mb_" << b.name << " = Protobuf.toBytes<"
                       << b.canon << ">(" << fv << ");\n";
                    os << "        w.writeLenField(" << tag << ", mb_" << b.name
                       << ", (int32) mb_" << b.name << ".count());\n";
                    os << "    }\n";
                    break;
                case Decode::Unsupported:
                    break;
            }
        }
        os << "    return w.toBytes();\n";
        os << "}\n";
        return os.str();
    }

    // Synthesize `toBytes(E[] values) -> #int8[]` — encode each element and frame
    // it length-delimited (the stream mirror of synthesizeStreamParseBody).
    std::string synthesizeStreamEncodeBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string PW = "dev.cajeta.codec.protobuf.ProtobufWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Ec << "[] values) {\n";
        os << "    " << PW << " w = heap " << PW << "();\n";
        os << "    int32 n = (int32) values.count();\n";
        os << "    int32 i = 0;\n";
        os << "    while (i < n) {\n";
        os << "        " << Ec << " ev = values[i];\n";
        os << "        int8[] mb = Protobuf.toBytes<" << Ec << ">(ev);\n";
        os << "        w.writeDelimited(mb, (int32) mb.count());\n";
        os << "        i = i + 1;\n";
        os << "    }\n";
        os << "    return w.toBytes();\n";
        os << "}\n";
        return os.str();
    }

    } // namespace

    bool synthesizeProtobufMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            const std::vector<CajetaTypePtr>& paramTypes,
            std::string& out) {
        if (!parent || !parent->getQName()) return false;
        if (parent->getQName()->toCanonical()
                != "dev.cajeta.codec.protobuf.Protobuf") {
            return false;
        }
        if (args.size() != 1) return false;
        const bool isParse = (methodName == "parse");
        const bool isEncode = (methodName == "toBytes");
        if (!isParse && !isEncode) return false;
        // parse(int8[], int64); toBytes(T) — one value param matching args[0].
        if (isParse) {
            if (paramTypes.size() != 2
                    || paramCanonAt(paramTypes, 0) != "int8[]"
                    || paramCanonAt(paramTypes, 1) != "int64") {
                return false;
            }
        } else {
            if (paramTypes.size() != 1) return false;
        }
        // T[] (array) → length-delimited stream; T (class) → one message.
        std::string label;
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(args[0])) {
            auto E = std::dynamic_pointer_cast<CajetaClass>(arr->getElementType());
            if (!E || !E->getQName()) return false;
            out = isParse ? synthesizeStreamParseBody(E)
                          : synthesizeStreamEncodeBody(E);
            label = E->getQName()->toCanonical() + "[]";
        } else {
            auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!T || !T->getQName()) return false;
            out = isParse ? synthesizeMessageParseBody(T)
                          : synthesizeMessageEncodeBody(T);
            label = T->getQName()->toCanonical();
        }
        if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
            if (dump[0] == '1') {
                std::cerr << "[ProtobufSynthesizer] " << methodName << "<"
                          << label << ">:\n" << out << "\n";
            }
        }
        return true;
    }

} // namespace cajeta
