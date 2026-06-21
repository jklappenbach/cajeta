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
        std::string name;      // field name == Ion symbol name (name-based bind)
        std::string canon;     // field type canonical (for the width cast)
        Decode decode;
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
            binds.push_back({prop->getName(), canon, d});
        }
        return binds;
    }

    // Synthesize `parse(int8[] bytes, int64 length) -> #T` for struct T.
    std::string synthesizeStructParseBody(const CajetaClassPtr& T) {
        const std::string Tc = T->getQName()->toCanonical();
        std::vector<Bind> binds = collectBinds(T);

        const std::string IC = "dev.cajeta.codec.ion.IonCursor";
        std::ostringstream os;
        os << "public static #" << Tc << " parse(int8[] bytes, int64 length) {\n";
        os << "    " << IC << " cur = heap " << IC << "(bytes, length);\n";
        os << "    " << Tc << " e = heap " << Tc << "();\n";
        for (auto& b : binds) {
            if (b.decode == Decode::Message) continue;   // nested → 3.2c
            const std::string slot = "s_" + b.name;
            os << "    int32 " << slot << " = cur.slotOf(\"" << b.name << "\");\n";
            os << "    if (" << slot << " >= (int32) 0) {\n";
            os << "        if (!cur.isNull(" << slot << ")) {\n";
            switch (b.decode) {
                case Decode::Int:
                    if (b.canon == "int64") {
                        os << "            e." << b.name << " = cur.readInt("
                           << slot << ");\n";
                    } else {
                        os << "            e." << b.name << " = (" << b.canon
                           << ") cur.readInt(" << slot << ");\n";
                    }
                    break;
                case Decode::Bool:
                    os << "            e." << b.name << " = cur.readBool("
                       << slot << ");\n";
                    break;
                case Decode::Str:
                    os << "            e." << b.name << " = cur.readString("
                       << slot << ");\n";
                    break;
                case Decode::Bytes:
                    os << "            e." << b.name << " = cur.readBytes("
                       << slot << ");\n";
                    break;
                case Decode::Message:
                case Decode::Unsupported:
                    break;
            }
            os << "        }\n";
            os << "    }\n";
        }
        os << "    return #e;\n";
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
        // T[] (stream) → 3.2d (not yet); T (class) → one struct.
        if (std::dynamic_pointer_cast<CajetaArray>(args[0])) return false;
        auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
        if (!T || !T->getQName()) return false;
        out = synthesizeStructParseBody(T);

        if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
            if (dump[0] == '1') {
                std::cerr << "[IonSynthesizer] " << methodName << "<"
                          << T->getQName()->toCanonical() << ">:\n" << out << "\n";
            }
        }
        return true;
    }

} // namespace cajeta
