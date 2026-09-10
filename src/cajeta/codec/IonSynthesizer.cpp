// Codec Phase 3.2 — Tier-1 Ion typed-bind synthesizer (see header).

#include "IonSynthesizer.h"
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

    // Decode strategy per field; Ion is self-describing, so only the destination matters.
    enum class Decode {
        Int,       // int8/16/32/64 + uint* — readInt, cast to the field width
        Bool,      // boolean — readBool
        Str,       // cajeta.lang.String — readString
        Bytes,     // int8[] — readBytes (blob)
        Message,   // nested struct (a class) — 3.2c
        Unsupported
    };

    struct Bind {
        std::string name;          // field name == Ion symbol name (name-based bind)
        std::string canon;         // field type canonical (for the width cast)
        Decode decode;
        CajetaClassPtr nested;     // the field's class (Message only); else null
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
            return Decode::Int;
        }
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return Decode::Message;
        return Decode::Unsupported;
    }

    // Bindable fields of T: every declared field with a supported type. Ion needs no
    // annotation - the field name IS the binding key.
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
            if (d == Decode::Message) {
                nested = std::dynamic_pointer_cast<CajetaClass>(ty);
            }
            binds.push_back({prop->getName(), canon, d, nested});
        }
        return binds;
    }

    // Emit the field binds of `T` off `cursorVar` onto `objVar`; `path` keeps emitted
    // locals unique per depth. A nested struct steps in and out on the SAME cursor.
    void emitStructBind(std::ostringstream& os, const CajetaClassPtr& T,
                        const std::string& cursorVar, const std::string& objVar,
                        const std::string& path) {
        std::vector<Bind> binds = collectBinds(T);
        for (auto& b : binds) {
            const std::string slot = "s" + path + "_" + b.name;
            os << "    int32 " << slot << " = " << cursorVar << ".slotOf(\""
               << b.name << "\");\n";
            os << "    if (" << slot << " >= (int32) 0) {\n";
            os << "        if (!" << cursorVar << ".isNull(" << slot << ")) {\n";
            switch (b.decode) {
                case Decode::Int:
                    if (b.canon == "int64") {
                        os << "            " << objVar << "." << b.name << " = "
                           << cursorVar << ".readInt(" << slot << ");\n";
                    } else {
                        os << "            " << objVar << "." << b.name << " = ("
                           << b.canon << ") " << cursorVar << ".readInt("
                           << slot << ");\n";
                    }
                    break;
                case Decode::Bool:
                    os << "            " << objVar << "." << b.name << " = "
                       << cursorVar << ".readBool(" << slot << ");\n";
                    break;
                case Decode::Str: {
                    // readString/readBytes return OWNED values: adopt into a local with
                    // `#=`, then surrender with `#`; a plain store lends a dying temp.
                    const std::string sv = "v" + path + "_" + b.name;
                    os << "            String " << sv << " #= "
                       << cursorVar << ".readString(" << slot << ");\n";
                    os << "            " << objVar << "." << b.name << " = #"
                       << sv << ";\n";
                    break;
                }
                case Decode::Bytes: {
                    const std::string bv = "v" + path + "_" + b.name;
                    os << "            int8[] " << bv << " #= "
                       << cursorVar << ".readBytes(" << slot << ");\n";
                    os << "            " << objVar << "." << b.name << " = #"
                       << bv << ";\n";
                    break;
                }
                case Decode::Message: {
                    const std::string childPath = path + "_" + b.name;
                    const std::string childObj = "o" + childPath;
                    os << "            " << cursorVar << ".stepIn(" << slot << ");\n";
                    os << "            " << b.canon << " " << childObj << " = heap "
                       << b.canon << "();\n";
                    emitStructBind(os, b.nested, cursorVar, childObj, childPath);
                    os << "            " << cursorVar << ".stepOut();\n";
                    os << "            " << objVar << "." << b.name << " = #"
                       << childObj << ";\n";
                    break;
                }
                case Decode::Unsupported:
                    break;
            }
            os << "        }\n";
            os << "    }\n";
        }
    }

    std::string synthesizeStructParseBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        const std::string IC = "dev.cajeta.codec.ion.IonCursor";
        std::ostringstream os;
        os << "public static #" << Tc << " parse(int8[] bytes, int64 length) {\n";
        os << "    " << IC << " cur = heap " << IC << "(bytes, length);\n";
        os << "    " << Tc << " e = heap " << Tc << "();\n";
        emitStructBind(os, T, "cur", "e", "");
        os << "    return #e;\n";
        os << "}\n";
        return os.str();
    }

    // Emit `parse` for a stream of back-to-back top-level E structs. A SINGLE cursor
    // walks it; `reset()` rewinds for the count pass. Assumes one or more values.
    std::string synthesizeStreamParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string IC = "dev.cajeta.codec.ion.IonCursor";
        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";
        os << "    " << IC << " cur = heap " << IC << "(bytes, length);\n";
        os << "    int32 count = 1;\n";
        os << "    while (cur.nextTopLevel()) {\n";
        os << "        count = count + 1;\n";
        os << "    }\n";
        os << "    cur.reset();\n";
        os << "    " << Ec << "[] outv = heap " << Ec << "[count];\n";
        os << "    int32 i = 0;\n";
        os << "    boolean more = true;\n";
        os << "    while (more) {\n";
        os << "        " << Ec << " e = heap " << Ec << "();\n";
        emitStructBind(os, E, "cur", "e", "");
        os << "        outv[i] = #e;\n";
        os << "        i = i + 1;\n";
        os << "        more = cur.nextTopLevel();\n";
        os << "    }\n";
        os << "    return #outv;\n";
        os << "}\n";
        return os.str();
    }

    // Emit `w.intern("...")` for every bindable field name of T, recursing into nested
    // structs. Interning must populate the symbol table BEFORE the LST is written.
    void emitInternNames(std::ostringstream& os, const CajetaClassPtr& T,
                         const std::string& w) {
        std::vector<Bind> binds = collectBinds(T);
        for (auto& b : binds) {
            os << "    " << w << ".intern(\"" << b.name << "\");\n";
            if (b.decode == Decode::Message && b.nested) {
                emitInternNames(os, b.nested, w);
            }
        }
    }

    // Emit the field encodes of `T` from `objVar` into writer `w`. Field reads are
    // hoisted to locals; a nested struct uses beginContainer/endContainer(13).
    void emitStructEncode(std::ostringstream& os, const CajetaClassPtr& T,
                          const std::string& w, const std::string& objVar,
                          const std::string& path) {
        std::vector<Bind> binds = collectBinds(T);
        for (auto& b : binds) {
            const std::string vloc = "v" + path + "_" + b.name;
            const std::string sid = w + ".intern(\"" + b.name + "\")";
            switch (b.decode) {
                case Decode::Int:
                    os << "    int64 " << vloc << " = (int64) " << objVar << "."
                       << b.name << ";\n";
                    os << "    " << w << ".writeFieldSid(" << sid << ");\n";
                    os << "    " << w << ".writeIntValue(" << vloc << ");\n";
                    break;
                case Decode::Bool:
                    os << "    boolean " << vloc << " = " << objVar << "."
                       << b.name << ";\n";
                    os << "    " << w << ".writeFieldSid(" << sid << ");\n";
                    os << "    " << w << ".writeBoolValue(" << vloc << ");\n";
                    break;
                case Decode::Str:
                    os << "    cajeta.lang.String " << vloc << " = " << objVar
                       << "." << b.name << ";\n";
                    os << "    if (" << vloc << " != null) {\n";
                    os << "        " << w << ".writeFieldSid(" << sid << ");\n";
                    os << "        int8[] sb" << path << "_" << b.name << " #= "
                       << vloc << ".toBytes();\n";
                    os << "        " << w << ".writeStringValue(sb" << path << "_"
                       << b.name << ", (int32) sb" << path << "_" << b.name
                       << ".count());\n";
                    os << "    }\n";
                    break;
                case Decode::Bytes:
                    os << "    int8[] " << vloc << " = " << objVar << "."
                       << b.name << ";\n";
                    os << "    if (" << vloc << " != null) {\n";
                    os << "        " << w << ".writeFieldSid(" << sid << ");\n";
                    os << "        " << w << ".writeBytesValue(" << vloc
                       << ", (int32) " << vloc << ".count());\n";
                    os << "    }\n";
                    break;
                case Decode::Message: {
                    const std::string childPath = path + "_" + b.name;
                    os << "    " << b.canon << " " << vloc << " = " << objVar
                       << "." << b.name << ";\n";
                    os << "    if (" << vloc << " != null) {\n";
                    os << "        " << w << ".writeFieldSid(" << sid << ");\n";
                    os << "        " << w << ".beginContainer();\n";
                    emitStructEncode(os, b.nested, w, vloc, childPath);
                    os << "        " << w << ".endContainer(13);\n";
                    os << "    }\n";
                    break;
                }
                case Decode::Unsupported:
                    break;
            }
        }
    }

    std::string synthesizeStructEncodeBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        const std::string IWr = "dev.cajeta.codec.ion.IonWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Tc << " value) {\n";
        os << "    " << IWr << " w = heap " << IWr << "();\n";
        emitInternNames(os, T, "w");
        os << "    w.writeBvm();\n";
        os << "    w.writeLocalSymbolTable();\n";
        os << "    w.beginContainer();\n";
        emitStructEncode(os, T, "w", "value", "");
        os << "    w.endContainer(13);\n";
        os << "    return w.toBytes();\n";
        os << "}\n";
        return os.str();
    }

    // Emit `toBytes(E[] values)`: BVM + LST once, then each struct back-to-back.
    std::string synthesizeStreamEncodeBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string IWr = "dev.cajeta.codec.ion.IonWriter";
        std::ostringstream os;
        os << "public static #int8[] toBytes(" << Ec << "[] values) {\n";
        os << "    " << IWr << " w = heap " << IWr << "();\n";
        emitInternNames(os, E, "w");
        os << "    w.writeBvm();\n";
        os << "    w.writeLocalSymbolTable();\n";
        os << "    int32 n = (int32) values.count();\n";
        os << "    int32 i = 0;\n";
        os << "    while (i < n) {\n";
        os << "        " << Ec << " ev = values[i];\n";
        os << "        w.beginContainer();\n";
        emitStructEncode(os, E, "w", "ev", "_e");
        os << "        w.endContainer(13);\n";
        os << "        i = i + 1;\n";
        os << "    }\n";
        os << "    return w.toBytes();\n";
        os << "}\n";
        return os.str();
    }

    } // namespace

    bool synthesizeIonMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            const std::vector<CajetaTypePtr>& paramTypes,
            std::string& out) {
        if (!parent || !parent->getQName()) return false;
        if (parent->getQName()->toCanonical()
                != "dev.cajeta.codec.ion.Ion") {
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
            out = isParse ? synthesizeStreamParseBody(E)
                          : synthesizeStreamEncodeBody(E);
            label = E->getQName()->toCanonical() + "[]";
        } else {
            auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!T || !T->getQName()) return false;
            out = isParse ? synthesizeStructParseBody(T)
                          : synthesizeStructEncodeBody(T);
            label = T->getQName()->toCanonical();
        }

        if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
            if (dump[0] == '1') {
                std::cerr << "[IonSynthesizer] " << methodName << "<"
                          << label << ">:\n" << out << "\n";
            }
        }
        return true;
    }

} // namespace cajeta
