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

    // How a bound field decodes off the cursor: inferred from its Cajeta type,
    // overridden by `encoding`, and a compile error where nothing maps.
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

    // The `encoding = "..."` option on @ProtoField; absent is Default.
    enum class Encoding { Default, Zigzag, Fixed };

    struct Bind {
        int number;            // explicit @ProtoField wire number
        std::string name;      // field name
        std::string canon;     // scalar: the field type canonical; repeated: the element's
        Decode decode;         // scalar: the kind. repeated: the ELEMENT kind
        bool repeated = false; // an array field (except int8[], which is bytes)
        bool packed = false;   // repeated numeric written as one LEN record
    };

    bool isSignedIntCanon(const std::string& c) {
        return c == "int8" || c == "int16" || c == "int32" || c == "int64";
    }

    bool isIntCanon(const std::string& c) {
        return isSignedIntCanon(c) || c == "uint8" || c == "uint16"
            || c == "uint32" || c == "uint64";
    }

    // protobuf has only fixed32/fixed64, so narrower fields ride the 32-bit form.
    bool isWide64Canon(const std::string& c) {
        return c == "int64" || c == "uint64";
    }

    // Classifies a NON-array type, and a repeated field's element type with it.
    Decode classifyScalar(const CajetaTypePtr& ty, const std::string& canon) {
        if (canon == "cajeta.lang.String") return Decode::StringLen;
        if (canon == "boolean") return Decode::BoolVarint;
        // Raw IEEE-754 bits, reinterpreted: a `(int32)` cast would wire 0.5 as 0.
        if (canon == "float32") return Decode::Float32Bits;
        if (canon == "float64") return Decode::Float64Bits;
        if (canon == "int8" || canon == "int16" || canon == "int32"
                || canon == "int64" || canon == "uint8" || canon == "uint16"
                || canon == "uint32" || canon == "uint64") {
            return Decode::IntVarint;
        }
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return Decode::MessageLen;
        return Decode::Unsupported;
    }

    // Classifies a field type; the only array reaching here is int8[] (`bytes`).
    Decode classify(const CajetaTypePtr& ty, const std::string& canon) {
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            auto el = arr->getElementType();
            if (el && el->getQName()
                    && el->getQName()->toCanonical() == "int8") {
                return Decode::BytesLen;
            }
            return Decode::Unsupported;
        }
        return classifyScalar(ty, canon);
    }

    // Packable = carried by a numeric wire type; a varying length needs LEN.
    bool isPackableKind(Decode d) {
        return d == Decode::IntVarint || d == Decode::ZigzagVarint
            || d == Decode::Fixed32Int || d == Decode::Fixed64Int
            || d == Decode::BoolVarint || d == Decode::Float32Bits
            || d == Decode::Float64Bits;
    }

    // The @ProtoField(N) wire number on a field, or -1 if absent.
    int protoFieldNumber(const StructurePropertyPtr& prop) {
        if (!prop) return -1;
        if (auto ann = prop->findAnnotation("ProtoField")) {
            return (int) ann->getInt("value", -1);
        }
        return -1;
    }

    // Applies `encoding`; one the type cannot carry is a compile error.
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

    // ---- repeated-field emit ------------------------------------------------
    // Decodes a repeated field outside any slot guard: absent reads as empty.
    void emitRepeatedParse(std::ostringstream& os, const Bind& b) {
        const std::string N = "(int32) " + std::to_string(b.number);
        const std::string r = "r_" + b.name;      // raw values off the cursor
        const std::string a = "a_" + b.name;      // the typed array we build
        const std::string n = "n_" + b.name;
        const std::string i = "i_" + b.name;
        const std::string v = "v_" + b.name;

        std::string reader;
        std::string rawElem;
        switch (b.decode) {
            case Decode::IntVarint:   reader = "readRepeatedVarint";  rawElem = "int64"; break;
            case Decode::ZigzagVarint:reader = "readRepeatedZigzag";  rawElem = "int64"; break;
            case Decode::BoolVarint:  reader = "readRepeatedVarint";  rawElem = "int64"; break;
            case Decode::Fixed32Int:  reader = "readRepeatedFixed32"; rawElem = "int32"; break;
            case Decode::Float32Bits: reader = "readRepeatedFixed32"; rawElem = "int32"; break;
            case Decode::Fixed64Int:  reader = "readRepeatedFixed64"; rawElem = "int64"; break;
            case Decode::Float64Bits: reader = "readRepeatedFixed64"; rawElem = "int64"; break;
            default: break;
        }

        if (!reader.empty()) {
            // `#=` because every `readRepeated*` returns `#T[]`: the binding takes a title.
            os << "    " << rawElem << "[] " << r << " #= cur." << reader
               << "(" << N << ");\n";
            const bool direct = (b.decode == Decode::IntVarint
                                 || b.decode == Decode::ZigzagVarint
                                 || b.decode == Decode::Fixed32Int
                                 || b.decode == Decode::Fixed64Int)
                                && b.canon == rawElem;
            if (direct) {
                os << "    e." << b.name << " = #" << r << ";\n";
                return;
            }
            os << "    int32 " << n << " = (int32) " << r << ".count();\n";
            os << "    " << b.canon << "[] " << a << " = heap " << b.canon
               << "[" << n << "];\n";
            os << "    int32 " << i << " = 0;\n";
            os << "    while (" << i << " < " << n << ") {\n";
            os << "        " << rawElem << " " << v << " = " << r << "[" << i << "];\n";
            if (b.decode == Decode::BoolVarint) {
                os << "        " << a << "[" << i << "] = " << v << " != (int64) 0;\n";
            } else if (b.decode == Decode::Float32Bits) {
                os << "        " << a << "[" << i << "] = Cajeta.bitsToF32(" << v << ");\n";
            } else if (b.decode == Decode::Float64Bits) {
                os << "        " << a << "[" << i << "] = Cajeta.bitsToF64(" << v << ");\n";
            } else {
                os << "        " << a << "[" << i << "] = (" << b.canon << ") " << v << ";\n";
            }
            os << "        " << i << " = " << i << " + 1;\n";
            os << "    }\n";
            os << "    e." << b.name << " = #" << a << ";\n";
            return;
        }

        // LEN-framed elements are one per occurrence, so these walk the slots.
        const std::string s = "s_" + b.name;
        const std::string bv = "b_" + b.name;
        os << "    int32 " << n << " = cur.repeatedCount(" << N << ");\n";
        os << "    " << b.canon << "[] " << a << " = heap " << b.canon
           << "[" << n << "];\n";
        os << "    int32 " << i << " = 0;\n";
        os << "    while (" << i << " < " << n << ") {\n";
        os << "        int32 " << s << " = cur.slotOfNth(" << N << ", " << i << ");\n";
        os << "        int8[] " << bv << " #= cur.readBytes(" << s << ");\n";
        if (b.decode == Decode::StringLen) {
            os << "        int32 bn_" << b.name << " = (int32) " << bv << ".count();\n";
            os << "        cajeta.lang.String str_" << b.name
               << " = heap cajeta.lang.String(#" << bv << ", bn_" << b.name << ");\n";
            os << "        " << a << "[" << i << "] = #str_" << b.name << ";\n";
        } else {
            os << "        " << b.canon << " m_" << b.name << " #= Protobuf.parse<"
               << b.canon << ">(" << bv << ", (int64) " << bv << ".count());\n";
            os << "        " << a << "[" << i << "] = #m_" << b.name << ";\n";
        }
        os << "        " << i << " = " << i << " + 1;\n";
        os << "    }\n";
        os << "    e." << b.name << " = #" << a << ";\n";
    }

    // Encodes a repeated field; a null array writes nothing and decodes empty.
    void emitRepeatedEncode(std::ostringstream& os, const Bind& b) {
        const std::string N = "(int32) " + std::to_string(b.number);
        const std::string a = "a_" + b.name;
        const std::string n = "n_" + b.name;
        const std::string i = "i_" + b.name;
        const std::string v = "v_" + b.name;
        const std::string t = "t_" + b.name;

        os << "    " << b.canon << "[] " << a << " = value." << b.name << ";\n";
        os << "    if (" << a << " != null) {\n";
        os << "        int32 " << n << " = (int32) " << a << ".count();\n";

        if (b.packed) {
            // Packed wants one run, so a mismatched element widens into scratch.
            std::string writer;
            std::string wantElem;
            switch (b.decode) {
                case Decode::IntVarint:   writer = "writePackedVarintField";  wantElem = "int64"; break;
                case Decode::ZigzagVarint:writer = "writePackedZigzagField";  wantElem = "int64"; break;
                case Decode::BoolVarint:  writer = "writePackedVarintField";  wantElem = "int64"; break;
                case Decode::Fixed32Int:  writer = "writePackedFixed32Field"; wantElem = "int32"; break;
                case Decode::Float32Bits: writer = "writePackedFixed32Field"; wantElem = "int32"; break;
                case Decode::Fixed64Int:  writer = "writePackedFixed64Field"; wantElem = "int64"; break;
                case Decode::Float64Bits: writer = "writePackedFixed64Field"; wantElem = "int64"; break;
                default: break;
            }
            const bool direct = (b.decode == Decode::IntVarint
                                 || b.decode == Decode::ZigzagVarint
                                 || b.decode == Decode::Fixed32Int
                                 || b.decode == Decode::Fixed64Int)
                                && b.canon == wantElem;
            if (direct) {
                os << "        w." << writer << "(" << N << ", " << a
                   << ", " << n << ");\n";
            } else {
                os << "        " << wantElem << "[] " << t << " = heap "
                   << wantElem << "[" << n << "];\n";
                os << "        int32 " << i << " = 0;\n";
                os << "        while (" << i << " < " << n << ") {\n";
                os << "            " << b.canon << " " << v << " = " << a << "[" << i << "];\n";
                if (b.decode == Decode::BoolVarint) {
                    os << "            int64 bx_" << b.name << " = (int64) 0;\n";
                    os << "            if (" << v << ") { bx_" << b.name << " = (int64) 1; }\n";
                    os << "            " << t << "[" << i << "] = bx_" << b.name << ";\n";
                } else if (b.decode == Decode::Float32Bits) {
                    os << "            " << t << "[" << i << "] = Cajeta.f32ToBits(" << v << ");\n";
                } else if (b.decode == Decode::Float64Bits) {
                    os << "            " << t << "[" << i << "] = Cajeta.f64ToBits(" << v << ");\n";
                } else {
                    os << "            " << t << "[" << i << "] = (" << wantElem << ") " << v << ";\n";
                }
                os << "            " << i << " = " << i << " + 1;\n";
                os << "        }\n";
                os << "        w." << writer << "(" << N << ", " << t
                   << ", " << n << ");\n";
            }
            os << "    }\n";
            return;
        }

        // Unpacked: one tagged record per element, by the scalar writer call.
        os << "        int32 " << i << " = 0;\n";
        os << "        while (" << i << " < " << n << ") {\n";
        os << "            " << b.canon << " " << v << " = " << a << "[" << i << "];\n";
        switch (b.decode) {
            case Decode::IntVarint:
                os << "            w.writeVarintField(" << N << ", (int64) " << v << ");\n";
                break;
            case Decode::ZigzagVarint:
                os << "            w.writeZigzagField(" << N << ", (int64) " << v << ");\n";
                break;
            case Decode::BoolVarint:
                os << "            int64 bx_" << b.name << " = (int64) 0;\n";
                os << "            if (" << v << ") { bx_" << b.name << " = (int64) 1; }\n";
                os << "            w.writeVarintField(" << N << ", bx_" << b.name << ");\n";
                break;
            case Decode::Fixed32Int:
                os << "            w.writeFixed32Field(" << N << ", (int32) " << v << ");\n";
                break;
            case Decode::Fixed64Int:
                os << "            w.writeFixed64Field(" << N << ", (int64) " << v << ");\n";
                break;
            case Decode::Float32Bits:
                os << "            w.writeFixed32Field(" << N << ", Cajeta.f32ToBits(" << v << "));\n";
                break;
            case Decode::Float64Bits:
                os << "            w.writeFixed64Field(" << N << ", Cajeta.f64ToBits(" << v << "));\n";
                break;
            case Decode::StringLen:
                os << "            if (" << v << " != null) {\n";
                os << "                int8[] sb_" << b.name << " #= " << v << ".toBytes();\n";
                os << "                w.writeLenField(" << N << ", sb_" << b.name
                   << ", (int32) sb_" << b.name << ".count());\n";
                os << "            }\n";
                break;
            case Decode::MessageLen:
                os << "            if (" << v << " != null) {\n";
                os << "                int8[] mb_" << b.name << " #= Protobuf.toBytes<"
                   << b.canon << ">(" << v << ");\n";
                os << "                w.writeLenField(" << N << ", mb_" << b.name
                   << ", (int32) mb_" << b.name << ".count());\n";
                os << "            }\n";
                break;
            default:
                break;
        }
        os << "            " << i << " = " << i << " + 1;\n";
        os << "        }\n";
        os << "    }\n";
    }

    // Synthesize `parse(int8[] bytes, int64 length) -> #T` for message T.
    std::string synthesizeMessageParseBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();

        std::vector<Bind> binds = collectBinds(T);

        const std::string PC = "dev.cajeta.codec.protobuf.ProtobufCursor";
        std::ostringstream os;
        os << "public static #" << Tc << " parse(int8[] bytes, int64 length) {\n";
        os << "    " << PC << " cur = heap " << PC << "(bytes, length);\n";
        os << "    " << Tc << " e = heap " << Tc << "();\n";
        for (auto& b : binds) {
            // Repeated fields bind unconditionally, outside the slot guard.
            if (b.repeated) { emitRepeatedParse(os, b); continue; }
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
                       << " = Cajeta.bitsToF32(cur.readFixed32("
                       << slot << "));\n";
                    break;
                case Decode::Float64Bits:
                    os << "        e." << b.name
                       << " = Cajeta.bitsToF64(cur.readFixed64("
                       << slot << "));\n";
                    break;
                case Decode::StringLen: {
                    const std::string bv = "b_" + b.name;
                    os << "        int8[] " << bv << " #= cur.readBytes("
                       << slot << ");\n";
                    // Count BEFORE the ctor adopts #bv; the local then transfers.
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
                    os << "        int8[] " << bv << " #= cur.readBytes("
                       << slot << ");\n";
                    os << "        e." << b.name << " = #" << bv << ";\n";
                    break;
                }
                case Decode::MessageLen: {
                    // The SHORT `Protobuf` receiver: qualified NULL_OPERANDs here.
                    const std::string bv = "b_" + b.name;
                    os << "        int8[] " << bv << " #= cur.readBytes("
                       << slot << ");\n";
                    os << "        " << b.canon << " m_" << b.name
                       << " #= Protobuf.parse<" << b.canon << ">(" << bv
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

    // Synthesizes `parse(int8[], int64) -> #E[]` over varint-prefixed frames.
    std::string synthesizeStreamParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";
        // A frame is checked to fit before counting, or truncation overruns.
        os << "    int32 count = 0;\n";
        os << "    int64 p = (int64) 0;\n";
        os << "    while (p < length) {\n";
        os << "        int64 fl = ProtobufWire.decodeVarint(bytes, p, length);\n";
        os << "        int64 hl = ProtobufWire.varintLen(bytes, p, length);\n";
        os << "        if (fl < (int64) 0 || p + hl + fl > length) {\n";
        os << "            throw heap dev.cajeta.codec.protobuf."
              "ProtobufParseException(\n";
        os << "                \"truncated protobuf delimited frame\", p);\n";
        os << "        }\n";
        os << "        p = p + hl + fl;\n";
        os << "        count = count + 1;\n";
        os << "    }\n";
        os << "    " << Ec << "[] outv = heap " << Ec << "[count];\n";
        os << "    p = (int64) 0;\n";
        os << "    int32 i = 0;\n";
        os << "    while (p < length) {\n";
        os << "        int64 fl = ProtobufWire.decodeVarint(bytes, p, length);\n";
        os << "        int64 hl = ProtobufWire.varintLen(bytes, p, length);\n";
        os << "        int64 start = p + hl;\n";
        os << "        int32 fln = (int32) fl;\n";
        os << "        int8[] frame = heap int8[fln];\n";
        os << "        int32 k = 0;\n";
        os << "        while (k < fln) {\n";
        // The index is hoisted: an inline `bytes[start+(cast)k]` miscompiles.
        os << "            int64 si = start + (int64) k;\n";
        os << "            int8 fb = bytes[si];\n";
        os << "            frame[k] = fb;\n";
        os << "            k = k + 1;\n";
        os << "        }\n";
        os << "        " << Ec << " m #= Protobuf.parse<" << Ec << ">(frame, fl);\n";
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
            std::string canon = ty->getQName()->toCanonical();

            // Every array is a repeated field except int8[], which is `bytes`.
            bool repeated = false;
            CajetaTypePtr scalarTy = ty;
            if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
                auto el = arr->getElementType();
                const std::string elCanon = (el && el->getQName())
                    ? el->getQName()->toCanonical() : std::string();
                if (elCanon != "int8") {
                    if (elCanon.empty()) {
                        reportOrThrow(prop->getDeclLine(), prop->getDeclColumn(),
                            "CAJETA_ERROR_PROTO_FIELD_TYPE",
                            "@ProtoField(" + std::to_string(number) + ") on "
                            + T->getQName()->toCanonical() + "."
                            + prop->getName()
                            + " — repeated field has no resolvable element type");
                        continue;
                    }
                    repeated = true;
                    scalarTy = el;
                    canon = elCanon;      // binds now describe the ELEMENT
                }
            }

            Decode d = repeated
                ? classifyScalar(scalarTy, canon)
                : classify(ty, canon);
            if (d == Decode::Unsupported) {
                // A numbered field with no wire mapping fails the build, never drops.
                reportOrThrow(prop->getDeclLine(), prop->getDeclColumn(),
                    "CAJETA_ERROR_PROTO_FIELD_TYPE",
                    "@ProtoField(" + std::to_string(number) + ") on "
                    + T->getQName()->toCanonical() + "." + prop->getName()
                    + " — no protobuf wire mapping for type '" + canon
                    + "'. Supported: integer, boolean, float32/float64, String, "
                      "int8[] (bytes), a nested message class, and arrays of "
                      "those (repeated).");
                continue;
            }
            d = applyEncoding(T, prop, canon, d);

            // `packed` unset defaults to PACKED here: parsers must accept both.
            auto ann = prop->findAnnotation("ProtoField");
            const bool packedDeclared = ann && ann->findArg("packed");
            const bool packedAsked = packedDeclared && ann->getBool("packed", false);

            if (packedDeclared && !repeated) {
                reportOrThrow(prop->getDeclLine(), prop->getDeclColumn(),
                    "CAJETA_ERROR_PROTO_ENCODING",
                    "@ProtoField(packed = ...) on "
                    + T->getQName()->toCanonical() + "." + prop->getName()
                    + " — packed applies only to a repeated (array) field");
            } else if (packedAsked && !isPackableKind(d)) {
                reportOrThrow(prop->getDeclLine(), prop->getDeclColumn(),
                    "CAJETA_ERROR_PROTO_ENCODING",
                    "@ProtoField(packed = true) on "
                    + T->getQName()->toCanonical() + "." + prop->getName()
                    + " — packed applies only to repeated numeric elements, "
                      "not '" + canon + "'");
            }

            // Non-numeric elements are never packed, declared or not.
            bool packed = repeated && isPackableKind(d)
                && (packedDeclared ? packedAsked : true);

            binds.push_back({number, prop->getName(), canon, d,
                             repeated, packed});
        }
        return binds;
    }

    // Synthesizes `toBytes(T)`; accesses hoist to locals (field-as-arg miscompiles).
    std::string synthesizeMessageEncodeBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        std::vector<Bind> binds = collectBinds(T);
        const std::string PW = "dev.cajeta.codec.protobuf.ProtobufWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Tc << " value) {\n";
        os << "    " << PW << " w = heap " << PW << "();\n";
        for (auto& b : binds) {
            if (b.repeated) { emitRepeatedEncode(os, b); continue; }
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
                    os << "    int32 " << fv << " = Cajeta.f32ToBits(value."
                       << b.name << ");\n";
                    os << "    w.writeFixed32Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::Float64Bits:
                    os << "    int64 " << fv << " = Cajeta.f64ToBits(value."
                       << b.name << ");\n";
                    os << "    w.writeFixed64Field(" << tag << ", " << fv << ");\n";
                    break;
                case Decode::StringLen:
                    os << "    cajeta.lang.String " << fv << " = value." << b.name << ";\n";
                    os << "    if (" << fv << " != null) {\n";
                    os << "        int8[] sb_" << b.name << " #= " << fv << ".toBytes();\n";
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
                    os << "        int8[] mb_" << b.name << " #= Protobuf.toBytes<"
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

    // Synthesizes `toBytes(E[]) -> #int8[]`, framing each element delimited.
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
        os << "        int8[] mb #= Protobuf.toBytes<" << Ec << ">(ev);\n";
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
