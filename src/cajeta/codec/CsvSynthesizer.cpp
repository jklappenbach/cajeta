// Codec Phase 1.2b — Tier-1 CSV typed-bind synthesizer (see header).

#include "CsvSynthesizer.h"
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

    // Minimal escape for a header name embedded in a "..." literal.
    std::string escapeCajetaString(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        return out;
    }

    // The reader-driven value decode for a supported primitive field type, or
    // empty if the type isn't bound in b2. `fv` names the field's value bytes.
    std::string decodeExpr(const std::string& canon, const std::string& fv) {
        // Short class name — the body is reparented into Csv's package
        // (cajeta.codec.csv), so CsvReader resolves same-package (mirrors the
        // JSON synthesizer's short `JsonIndex.decode*` calls).
        if (canon == "int32") {
            return "(int32) CsvReader.parseI64(" + fv + ")";
        }
        if (canon == "int64") {
            return "CsvReader.parseI64(" + fv + ")";
        }
        if (canon == "float64") {
            return "CsvReader.parseF64(" + fv + ")";
        }
        if (canon == "boolean") {
            return "CsvReader.parseBool(" + fv + ")";
        }
        return "";   // String handled inline (ownership transfer); else skip.
    }

    // Synthesize `parse(int8[] bytes, int64 length) -> #E[]` for element E.
    std::string synthesizeArrayParseBody(const CajetaClassPtr& E) {
        const std::string Ec = E->getQName()->toCanonical();

        // Bindable fields: those whose type is a supported primitive or String.
        struct Bind { std::string name; std::string canon; bool isString; };
        std::vector<Bind> binds;
        for (auto& prop : E->getPropertyList()) {
            if (!prop) continue;
            auto ty = prop->getType();
            if (!ty || !ty->getQName()) continue;
            const std::string tc = ty->getQName()->toCanonical();
            bool isString = (tc == "cajeta.lang.String");
            if (isString || tc == "int32" || tc == "int64"
                    || tc == "float64" || tc == "boolean") {
                binds.push_back({prop->getName(), tc, isString});
            }
        }

        std::ostringstream os;
        os << "public static #" << Ec << "[] parse(int8[] bytes, int64 length) {\n";

        // Pass 1: header → column index map + data-row count.
        os << "    cajeta.codec.csv.CsvReader rh = "
              "heap cajeta.codec.csv.CsvReader(bytes, length);\n";
        for (auto& b : binds) {
            os << "    int32 col_" << b.name << " = (int32) -1;\n";
        }
        os << "    boolean hasHdr = rh.nextRow();\n";
        os << "    if (hasHdr) {\n";
        os << "        int32 hc = rh.fieldCount();\n";
        os << "        int32 hi = 0;\n";
        os << "        while (hi < hc) {\n";
        os << "            int8[] hb = rh.field(hi);\n";
        os << "            cajeta.lang.String hn = "
              "heap cajeta.lang.String(#hb, (int32) hb.count());\n";
        bool first = true;
        for (auto& b : binds) {
            os << "            " << (first ? "if" : "else if")
               << " (hn.equals(\"" << escapeCajetaString(b.name) << "\")) "
               << "{ col_" << b.name << " = hi; }\n";
            first = false;
        }
        os << "            hi = hi + 1;\n";
        os << "        }\n";
        os << "    }\n";
        os << "    int32 rowCount = 0;\n";
        os << "    boolean cn = rh.nextRow();\n";
        os << "    while (cn) { rowCount = rowCount + 1; cn = rh.nextRow(); }\n";

        // Pass 2: a fresh reader, skip the header, bind each row.
        os << "    " << Ec << "[] outv = heap " << Ec << "[rowCount];\n";
        os << "    cajeta.codec.csv.CsvReader rd = "
              "heap cajeta.codec.csv.CsvReader(bytes, length);\n";
        os << "    boolean sk = rd.nextRow();\n";
        os << "    int32 ri = 0;\n";
        os << "    boolean more = rd.nextRow();\n";
        os << "    while (more) {\n";
        os << "        " << Ec << " e = heap " << Ec << "();\n";
        for (auto& b : binds) {
            os << "        if (col_" << b.name << " >= (int32) 0) {\n";
            os << "            int8[] fv_" << b.name
               << " = rd.field(col_" << b.name << ");\n";
            if (b.isString) {
                os << "            int32 fl_" << b.name
                   << " = (int32) fv_" << b.name << ".count();\n";
                os << "            e." << b.name
                   << " = heap cajeta.lang.String(#fv_" << b.name
                   << ", fl_" << b.name << ");\n";
            } else {
                os << "            e." << b.name << " = "
                   << decodeExpr(b.canon, "fv_" + b.name) << ";\n";
            }
            os << "        }\n";
        }
        os << "        outv[ri] = #e;\n";
        os << "        ri = ri + 1;\n";
        os << "        more = rd.nextRow();\n";
        os << "    }\n";
        os << "    return outv;\n";
        os << "}\n";
        return os.str();
    }

    } // namespace

    bool synthesizeCsvMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            const std::vector<CajetaTypePtr>& paramTypes,
            std::string& out) {
        if (!parent || !parent->getQName()) return false;
        if (parent->getQName()->toCanonical() != "cajeta.codec.csv.Csv") {
            return false;
        }
        if (args.size() != 1) return false;
        if (methodName != "parse") return false;
        if (paramTypes.size() != 2
                || paramCanonAt(paramTypes, 0) != "int8[]"
                || paramCanonAt(paramTypes, 1) != "int64") {
            return false;
        }
        // b2 primary form: T[] — bind every data row to element T.
        // CajetaArray inherits CajetaClass, so check the array form first.
        auto arr = std::dynamic_pointer_cast<CajetaArray>(args[0]);
        if (!arr) return false;
        auto E = std::dynamic_pointer_cast<CajetaClass>(arr->getElementType());
        if (!E || !E->getQName()) return false;

        out = synthesizeArrayParseBody(E);
        if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
            if (dump[0] == '1') {
                std::cerr << "[CsvSynthesizer] parse<"
                          << E->getQName()->toCanonical() << "[]>:\n"
                          << out << "\n";
            }
        }
        return true;
    }

} // namespace cajeta
