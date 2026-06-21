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

    // Canonical of param `i`, or empty if out of range.
    std::string paramCanonAt(
            const std::vector<CajetaTypePtr>& paramTypes, size_t i) {
        if (i >= paramTypes.size()) return std::string();
        auto& p = paramTypes[i];
        if (!p || !p->getQName()) return std::string();
        return p->getQName()->toCanonical();
    }

    // How a bound field decodes off the Ion cursor. The cursor returns typed
    // values directly (Ion is self-describing), so there is no per-field wire
    // type to track — only the destination Cajeta type. float (tc 4) is parked
    // (P-ION-FLOAT — no int-bits↔float reinterpret seam); nested struct (Message)
    // lands in 3.2c (shared symbol table).
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

    // Classify a field's type into a decode strategy.
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

    // Collect the bindable fields of T (every declared field with a supported
    // type — Ion needs no annotation, the field name IS the binding key).
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

    // Emit the field binds of struct `T` against cursor `cursorVar`, setting them
    // on `objVar`. `path` keeps emitted locals unique across nesting depth. A
    // nested struct field descends with `stepIn` / `stepOut` on the SAME cursor —
    // the single-cursor / shared-symbol-table model (see codecs-plan §3.2c).
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
                case Decode::Str:
                    os << "            " << objVar << "." << b.name << " = "
                       << cursorVar << ".readString(" << slot << ");\n";
                    break;
                case Decode::Bytes:
                    os << "            " << objVar << "." << b.name << " = "
                       << cursorVar << ".readBytes(" << slot << ");\n";
                    break;
                case Decode::Message: {
                    const std::string childPath = path + "_" + b.name;
                    const std::string childObj = "o" + childPath;
                    os << "            " << cursorVar << ".stepIn(" << slot << ");\n";
                    os << "            " << b.canon << " " << childObj << " = heap "
                       << b.canon << "();\n";
                    emitStructBind(os, b.nested, cursorVar, childObj, childPath);
                    os << "            " << cursorVar << ".stepOut();\n";
                    os << "            " << objVar << "." << b.name << " = "
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

    // Synthesize `parse(int8[] bytes, int64 length) -> #T` for struct T.
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

    // Synthesize `parse(int8[] bytes, int64 length) -> #E[]` for a stream of
    // back-to-back top-level E structs (Ion's analog of protobuf's delimited
    // framing — Ion values are self-delimiting via their type-descriptor length).
    // A SINGLE cursor (one shared symbol table) walks the stream with
    // `nextTopLevel()`; `reset()` rewinds for the count pass. Assumes ≥1
    // top-level value.
    std::string synthesizeStreamParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();
        const std::string IC = "dev.cajeta.codec.ion.IonCursor";
        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";
        os << "    " << IC << " cur = heap " << IC << "(bytes, length);\n";
        // Pass 1: count top-level values.
        os << "    int32 count = 1;\n";
        os << "    while (cur.nextTopLevel()) {\n";
        os << "        count = count + 1;\n";
        os << "    }\n";
        os << "    cur.reset();\n";
        // Pass 2: allocate + bind each.
        os << "    " << Ec << "[] outv = heap " << Ec << "[count];\n";
        os << "    int32 i = 0;\n";
        os << "    boolean more = true;\n";
        os << "    while (more) {\n";
        os << "        " << Ec << " e = heap " << Ec << "();\n";
        emitStructBind(os, E, "cur", "e", "");
        os << "        outv[i] = e;\n";
        os << "        i = i + 1;\n";
        os << "        more = cur.nextTopLevel();\n";
        os << "    }\n";
        os << "    return outv;\n";
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
        if (methodName != "parse") return false;
        if (paramTypes.size() != 2
                || paramCanonAt(paramTypes, 0) != "int8[]"
                || paramCanonAt(paramTypes, 1) != "int64") {
            return false;
        }
        // T[] (stream) → back-to-back top-level structs; T (class) → one struct.
        std::string label;
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(args[0])) {
            auto E = std::dynamic_pointer_cast<CajetaClass>(arr->getElementType());
            if (!E || !E->getQName()) return false;
            out = synthesizeStreamParseBody(E);
            label = E->getQName()->toCanonical() + "[]";
        } else {
            auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!T || !T->getQName()) return false;
            out = synthesizeStructParseBody(T);
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
