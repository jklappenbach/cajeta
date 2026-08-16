// Codec Phase 5.1c — Tier-1 Avro typed-bind synthesizer (see header).

#include "AvroSynthesizer.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaType.h"
#include "../type/StructureProperty.h"
#include "../type/QualifiedName.h"

#include <iostream>
#include <sstream>

namespace cajeta {

    namespace {

    std::string paramCanonAt(
            const std::vector<CajetaTypePtr>& paramTypes, size_t i) {
        if (i >= paramTypes.size()) return std::string();
        auto& p = paramTypes[i];
        if (!p || !p->getQName()) return std::string();
        return p->getQName()->toCanonical();
    }

    // Avro decode strategy per field type. float/double parked (P-AVRO-FLOAT).
    enum class Decode { Long, Bool, Str, Bytes, Record, Unsupported };

    struct Bind {
        std::string name;
        std::string canon;
        Decode decode;
        CajetaClassPtr nested;
    };

    Decode classify(const CajetaTypePtr& ty, const std::string& canon) {
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            auto el = arr->getElementType();
            if (el && el->getQName()
                    && el->getQName()->toCanonical() == "int8") {
                return Decode::Bytes;
            }
            return Decode::Unsupported;
        }
        if (canon == "cajeta.lang.String") return Decode::Str;
        if (canon == "boolean") return Decode::Bool;
        if (canon == "int8" || canon == "int16" || canon == "int32"
                || canon == "int64" || canon == "uint8" || canon == "uint16"
                || canon == "uint32" || canon == "uint64") {
            return Decode::Long;
        }
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return Decode::Record;
        return Decode::Unsupported;
    }

    std::vector<Bind> collectBinds(const CajetaClassPtr& T) {
        std::vector<Bind> binds;
        for (auto& prop : T->getPropertyList()) {
            if (!prop) continue;
            auto ty = prop->getType();
            if (!ty || !ty->getQName()) continue;
            const std::string canon = ty->getQName()->toCanonical();
            Decode d = classify(ty, canon);
            if (d == Decode::Unsupported) continue;
            CajetaClassPtr nested;
            if (d == Decode::Record) nested = std::dynamic_pointer_cast<CajetaClass>(ty);
            binds.push_back({prop->getName(), canon, d, nested});
        }
        return binds;
    }

    // Emit positional reads of T's fields against cursor `cur` onto `objVar`.
    // Nested records recurse inline (Avro encodes them as consecutive fields).
    void emitRecordDecode(std::ostringstream& os, const CajetaClassPtr& T,
                          const std::string& cur, const std::string& objVar,
                          const std::string& path) {
        std::vector<Bind> binds = collectBinds(T);
        for (auto& b : binds) {
            switch (b.decode) {
                case Decode::Long:
                    if (b.canon == "int64") {
                        os << "    " << objVar << "." << b.name << " = "
                           << cur << ".readLong();\n";
                    } else {
                        os << "    " << objVar << "." << b.name << " = ("
                           << b.canon << ") " << cur << ".readLong();\n";
                    }
                    break;
                case Decode::Bool:
                    os << "    " << objVar << "." << b.name << " = "
                       << cur << ".readBoolean();\n";
                    break;
                case Decode::Str: {
                    // Owned #String return: hoist, then surrender into the
                    // field with '#' (a plain store lends a dying temp).
                    const std::string sv = "v" + path + "_" + b.name;
                    os << "    String " << sv << " #= " << cur << ".readString();\n";
                    os << "    " << objVar << "." << b.name << " = #" << sv << ";\n";
                    break;
                }
                case Decode::Bytes: {
                    const std::string bv = "v" + path + "_" + b.name;
                    os << "    int8[] " << bv << " #= " << cur << ".readBytes();\n";
                    os << "    " << objVar << "." << b.name << " = #" << bv << ";\n";
                    break;
                }
                case Decode::Record: {
                    const std::string childPath = path + "_" + b.name;
                    const std::string childObj = "o" + childPath;
                    os << "    " << b.canon << " " << childObj << " = heap "
                       << b.canon << "();\n";
                    emitRecordDecode(os, b.nested, cur, childObj, childPath);
                    os << "    " << objVar << "." << b.name << " = #"
                       << childObj << ";\n";
                    break;
                }
                case Decode::Unsupported:
                    break;
            }
        }
    }

    std::string synthesizeRecordParseBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        const std::string AC = "dev.cajeta.codec.avro.AvroCursor";
        std::ostringstream os;
        os << "public static #" << Tc << " parse(int8[] bytes, int64 length) {\n";
        os << "    " << AC << " cur = heap " << AC << "(bytes, (int64) 0);\n";
        os << "    " << Tc << " e = heap " << Tc << "();\n";
        emitRecordDecode(os, T, "cur", "e", "");
        os << "    return #e;\n";
        os << "}\n";
        return os.str();
    }

    std::string synthesizeContainerParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string AC = "dev.cajeta.codec.avro.AvroCursor";
        const std::string AK = "dev.cajeta.codec.avro.AvroContainer";
        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";
        os << "    " << AK << " ct = heap " << AK << "(bytes, length);\n";
        os << "    int32 total = 0;\n";
        os << "    boolean c1 = ct.nextBlock();\n";
        os << "    while (c1) {\n";
        os << "        total = total + (int32) ct.currentObjectCount();\n";
        os << "        c1 = ct.nextBlock();\n";
        os << "    }\n";
        os << "    ct.reset();\n";
        os << "    " << Ec << "[] outv = heap " << Ec << "[total];\n";
        os << "    int32 i = 0;\n";
        os << "    boolean more = ct.nextBlock();\n";
        os << "    while (more) {\n";
        os << "        int8[] data #= ct.currentData();\n";
        os << "        " << AC << " cur = heap " << AC << "(data, (int64) 0);\n";
        os << "        int64 cnt = ct.currentObjectCount();\n";
        os << "        int64 j = (int64) 0;\n";
        os << "        while (j < cnt) {\n";
        os << "            " << Ec << " e = heap " << Ec << "();\n";
        emitRecordDecode(os, E, "cur", "e", "");
        os << "            outv[i] = #e;\n";
        os << "            i = i + 1;\n";
        os << "            j = j + 1;\n";
        os << "        }\n";
        os << "        more = ct.nextBlock();\n";
        os << "    }\n";
        os << "    return #outv;\n";
        os << "}\n";
        return os.str();
    }

    // ---- encode (5.3) ------------------------------------------------------

    std::string simpleName(const CajetaClassPtr& T) {
        const std::string c = T->getQName()->toCanonical();
        auto p = c.rfind('.');
        return p == std::string::npos ? c : c.substr(p + 1);
    }

    // Emit positional writes of T's fields against writer `w` from `objVar`.
    // `path` keeps hoisted array-field locals unique across nesting.
    void emitRecordEncode(std::ostringstream& os, const CajetaClassPtr& T,
                          const std::string& w, const std::string& objVar,
                          const std::string& path) {
        // Every field is hoisted to a local before being passed — reading a
        // field off an array-element alias and passing it directly as a method
        // arg miscompiles (matches the proven Ion stream-encode pattern).
        std::vector<Bind> binds = collectBinds(T);
        for (auto& b : binds) {
            const std::string lv = "av" + path + "_" + b.name;
            switch (b.decode) {
                case Decode::Long:
                    os << "    int64 " << lv << " = (int64) " << objVar << "."
                       << b.name << ";\n";
                    os << "    " << w << ".writeLong(" << lv << ");\n";
                    break;
                case Decode::Bool:
                    os << "    boolean " << lv << " = " << objVar << "."
                       << b.name << ";\n";
                    os << "    " << w << ".writeBoolean(" << lv << ");\n";
                    break;
                case Decode::Str:
                    os << "    cajeta.lang.String " << lv << " = " << objVar
                       << "." << b.name << ";\n";
                    os << "    " << w << ".writeString(" << lv << ");\n";
                    break;
                case Decode::Bytes:
                    os << "    int8[] " << lv << " = " << objVar << "."
                       << b.name << ";\n";
                    os << "    " << w << ".writeBytes(" << lv << ", (int32) "
                       << lv << ".count());\n";
                    break;
                case Decode::Record:
                    emitRecordEncode(os, b.nested, w,
                                     objVar + "." + b.name, path + "_" + b.name);
                    break;
                case Decode::Unsupported:
                    break;
            }
        }
    }

    std::string buildSchemaJson(const CajetaClassPtr& T);

    std::string avroTypeJson(const Bind& b) {
        switch (b.decode) {
            case Decode::Long: return "\"long\"";
            case Decode::Bool: return "\"boolean\"";
            case Decode::Str:  return "\"string\"";
            case Decode::Bytes: return "\"bytes\"";
            case Decode::Record: return buildSchemaJson(b.nested);
            default: return "\"null\"";
        }
    }

    // Real Avro record schema JSON — makes the written OCF ecosystem-readable.
    std::string buildSchemaJson(const CajetaClassPtr& T) {
        std::ostringstream js;
        js << "{\"type\":\"record\",\"name\":\"" << simpleName(T)
           << "\",\"fields\":[";
        std::vector<Bind> binds = collectBinds(T);
        bool first = true;
        for (auto& b : binds) {
            if (!first) js << ",";
            first = false;
            js << "{\"name\":\"" << b.name << "\",\"type\":"
               << avroTypeJson(b) << "}";
        }
        js << "]}";
        return js.str();
    }

    // Escape a string for embedding as a Cajeta double-quoted literal.
    std::string escapeForCajeta(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\\' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    std::string synthesizeRecordEncodeBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        const std::string AW = "dev.cajeta.codec.avro.AvroWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Tc << " value) {\n";
        os << "    " << AW << " w = heap " << AW << "();\n";
        emitRecordEncode(os, T, "w", "value", "");
        os << "    return w.result();\n";
        os << "}\n";
        return os.str();
    }

    std::string synthesizeContainerEncodeBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string AW = "dev.cajeta.codec.avro.AvroWriter";
        const std::string ACW = "dev.cajeta.codec.avro.AvroContainerWriter";
        const std::string schema = escapeForCajeta(buildSchemaJson(E));
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Ec << "[] value) {\n";
        os << "    " << AW << " body = heap " << AW << "();\n";
        os << "    int32 n = (int32) value.count();\n";
        os << "    int32 i = 0;\n";
        os << "    while (i < n) {\n";
        os << "        " << Ec << " e = value[i];\n";
        emitRecordEncode(os, E, "body", "e", "");
        os << "        i = i + 1;\n";
        os << "    }\n";
        os << "    int8[] data #= body.result();\n";
        os << "    int64 dlen = body.size();\n";
        os << "    cajeta.lang.String schema = \"" << schema << "\";\n";
        // A FQ static call corrupts synthesized codegen (P-COMPILER-FQN); build
        // the OCF via a FQ constructor instead.
        os << "    " << ACW << " cw = heap " << ACW
           << "(data, dlen, (int64) n, schema);\n";
        os << "    return cw.result();\n";
        os << "}\n";
        return os.str();
    }

    } // namespace

    bool synthesizeAvroMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            const std::vector<CajetaTypePtr>& paramTypes,
            std::string& out) {
        if (!parent || !parent->getQName()) return false;
        if (parent->getQName()->toCanonical() != "dev.cajeta.codec.avro.Avro") {
            return false;
        }
        if (args.size() != 1) return false;
        const bool isParse = (methodName == "parse");
        const bool isEncode = (methodName == "toBytes");
        if (!isParse && !isEncode) return false;
        if (isParse) {
            if (paramTypes.size() != 2
                    || paramCanonAt(paramTypes, 0) != "int8[]"
                    || paramCanonAt(paramTypes, 1) != "int64") {
                return false;
            }
        } else {
            if (paramTypes.size() != 1) return false;
        }
        std::string label;
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(args[0])) {
            auto E = std::dynamic_pointer_cast<CajetaClass>(arr->getElementType());
            if (!E || !E->getQName()) return false;
            out = isParse ? synthesizeContainerParseBody(E)
                          : synthesizeContainerEncodeBody(E);
            label = E->getQName()->toCanonical() + "[]";
        } else {
            auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!T || !T->getQName()) return false;
            out = isParse ? synthesizeRecordParseBody(T)
                          : synthesizeRecordEncodeBody(T);
            label = T->getQName()->toCanonical();
        }
        if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
            if (dump[0] == '1') {
                std::cerr << "[AvroSynthesizer] " << methodName << "<"
                          << label << ">:\n" << out << "\n";
            }
        }
        return true;
    }

} // namespace cajeta
