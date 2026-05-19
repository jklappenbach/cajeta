// Phase 4b — Tier 1 JSON codegen synthesizer (see header).

#include "JsonSynthesizer.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaType.h"
#include "../type/StructureProperty.h"
#include "../type/QualifiedName.h"

#include <iostream>
#include <sstream>

namespace cajeta {

    namespace {

    // Emit a literal byte-comparison guard for a key name. Returns the
    // body source for the if-condition (without surrounding `if (...)`),
    // assuming `int8[] kb` and `int32 klen` are in scope and hold the
    // current KEY token's bytes / length.
    //
    // For key "id" → `klen == 2 && kb[0] == (int8) 0x69 && kb[1] == (int8) 0x64`.
    std::string keyBytesGuard(const std::string& key) {
        std::ostringstream os;
        os << "klen == " << key.size();
        for (size_t i = 0; i < key.size(); ++i) {
            unsigned char b = (unsigned char) key[i];
            os << " && kb[" << i << "] == (int8) 0x"
               << std::hex << (int) b << std::dec;
        }
        return os.str();
    }

    // Emit `out.<field> = <reader-call>;` for one supported field type.
    // Returns empty string if the type is unsupported (caller should
    // emit a skip-value arm instead).
    std::string readFieldAssignment(const StructurePropertyPtr& prop) {
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        const std::string& tcanon = ty->getQName()->toCanonical();
        if (tcanon == "int32") {
            return "out." + fieldName + " = r.currentNumberAsInt32();\n";
        }
        if (tcanon == "int64") {
            return "out." + fieldName + " = r.currentNumberAsInt64();\n";
        }
        if (tcanon == "boolean") {
            // JsonReader emits BOOLEAN as one token kind; currentBoolean()
            // disambiguates true vs false.
            return "out." + fieldName + " = r.currentBoolean();\n";
        }
        if (tcanon == "cajeta.lang.String") {
            // currentBytes() returns the inner string bytes (quotes
            // stripped by scanStringSpan). Wrap them in a String via
            // the (int8[], int32 byteLength) view-mode constructor;
            // the bytes are an owned #int8[] returned by the reader,
            // so the String holds the buffer for its own lifetime.
            // The .count() field accessor on the buffer returns int64;
            // the ctor wants int32 byteLength so narrow.
            return "{\n"
                   "            int8[] vbytes = r.currentBytes();\n"
                   "            out." + fieldName +
                       " = heap cajeta.lang.String("
                       "vbytes, (int32) vbytes.count());\n"
                   "        }\n";
        }
        // Future: float64, nested classes, arrays.
        return "";
    }

    // Build the synthesized `parse` method body for T. The method
    // signature is `public static <__T> T parse(int8[] bytes, int64
    // length) { ... }` with __T as a vacuous template parameter (it
    // doesn't appear in the body — bodies use concrete type names —
    // but keeping the <__T> on the signature satisfies the
    // method-template-instantiation machinery that expects the
    // synthesized declaration to carry method type parameters).
    std::string synthesizeParseBody(const CajetaClassPtr& T,
                                     const std::string& methodName) {
        // Use the short type name (e.g. `Box`) in the body — the wrapper
        // class is parsed in Json's module context (cajeta.codec.json),
        // which doesn't see user-package classes by default. The caller
        // (MethodTemplateInstantiator) wraps our snippet in that module's
        // preamble (synthesizeMethodPreamble pulls in module->getImports())
        // but the user's class T isn't in those imports. We sidestep this
        // by emitting an explicit `import` prefix as the first line of
        // the synthesized snippet — the instantiator concatenates it
        // INSIDE the wrapper class (between the brace and the method),
        // which doesn't parse as an import — so we need a different
        // approach.
        //
        // Workaround: fully-qualify in the signature (e.g. `test.Box`)
        // — the parser accepts dotted type names in declaration positions.
        // For the body, use a local-typed-as-pointer trick or just emit
        // the dotted name everywhere.
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static " << Tcanon
           << " " << methodName << "(int8[] bytes, int64 length) {\n";
        os << "    " << Tcanon << " out = heap " << Tcanon << "();\n";
        os << "    JsonReader r = heap JsonReader(bytes, length);\n";
        os << "    int32 t = r.next();\n";
        os << "    if (t != JsonToken.START_OBJECT) {\n";
        os << "        throw heap JsonParseException(\n";
        os << "            \"Tier-1 parse: expected '{'\", r.position());\n";
        os << "    }\n";
        os << "    while (true) {\n";
        os << "        t = r.next();\n";
        os << "        if (t == JsonToken.END_OBJECT) { return out; }\n";
        os << "        if (t != JsonToken.KEY) {\n";
        os << "            throw heap JsonParseException(\n";
        os << "                \"Tier-1 parse: expected key\", r.position());\n";
        os << "        }\n";
        os << "        int8[] kb = r.currentBytes();\n";
        os << "        int32 klen = (int32) kb.count();\n";
        bool first = true;
        for (auto& prop : T->getPropertyList()) {
            std::string assign = readFieldAssignment(prop);
            if (assign.empty()) continue;
            os << "        " << (first ? "if" : "else if")
               << " (" << keyBytesGuard(prop->getName()) << ") {\n";
            os << "            t = r.next();\n";
            os << "            " << assign;
            os << "        }\n";
            first = false;
        }
        if (!first) {
            os << "        else { t = r.next(); }\n";
        } else {
            os << "        t = r.next();\n";
        }
        os << "    }\n";
        os << "}\n";
        return os.str();
    }

    } // namespace

    bool synthesizeJsonMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            std::string& out) {
        if (!parent || !parent->getQName()) return false;
        if (parent->getQName()->toCanonical() != "cajeta.codec.json.Json") {
            return false;
        }
        if (args.size() != 1) return false;
        // T must be a class type (primitives forbidden by the spec).
        auto T = std::dynamic_pointer_cast<CajetaClass>(args[0]);
        if (!T) return false;
        if (methodName == "parse" || methodName == "parseT") {
            out = synthesizeParseBody(T, methodName);
            if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
                if (dump[0] == '1') {
                    std::cerr << "[JsonSynthesizer] " << methodName << "<"
                              << T->getQName()->toCanonical() << ">:\n"
                              << out << "\n";
                }
            }
            return true;
        }
        // toBytes lands in commit 2; until then we leave methodSource
        // unchanged and the captured throw-body fires.
        return false;
    }

} // namespace cajeta
