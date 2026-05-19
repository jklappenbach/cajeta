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

    // Emit the FULL post-key body for one supported field type — that
    // is, the statements that consume the value tokens AND assign the
    // result to out.<field>. The caller is responsible only for the
    // key-matching guard. For primitive value types this is
    // `t = r.next(); out.<field> = <reader-call>;`; for nested-class
    // types it's a single recursive `Json.parseObjectFromReader<NestedT>(r)`
    // call — the recursive parser will consume the START_OBJECT itself.
    //
    // Returns empty string if the type is unsupported (caller emits a
    // skip-value arm instead).
    std::string readFieldAssignment(const StructurePropertyPtr& prop) {
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        const std::string& tcanon = ty->getQName()->toCanonical();
        if (tcanon == "int32") {
            return "t = r.next();\n            out." + fieldName +
                   " = r.currentNumberAsInt32();\n";
        }
        if (tcanon == "int64") {
            return "t = r.next();\n            out." + fieldName +
                   " = r.currentNumberAsInt64();\n";
        }
        if (tcanon == "boolean") {
            return "t = r.next();\n            out." + fieldName +
                   " = r.currentBoolean();\n";
        }
        if (tcanon == "float64") {
            return "t = r.next();\n            out." + fieldName +
                   " = r.currentNumberAsFloat64();\n";
        }
        if (tcanon == "cajeta.lang.String") {
            // currentBytes() returns the inner string bytes (quotes
            // stripped). Wrap them in a String view-mode instance via
            // the (int8[], int32 byteLength) constructor.
            return "t = r.next();\n"
                   "            int8[] vbytes_" + fieldName +
                       " = r.currentBytes();\n"
                   "            out." + fieldName +
                       " = heap cajeta.lang.String("
                       "vbytes_" + fieldName +
                       ", (int32) vbytes_" + fieldName + ".count());\n";
        }
        // Nested class field — recurse via Json.parseObjectFromReader<NestedT>.
        // The recursive call consumes the value's START_OBJECT and
        // END_OBJECT itself; the outer loop should NOT call r.next()
        // before it. Use the short name `Json` because the wrapper
        // class lives in cajeta.codec.json — multi-segment FQN
        // (`cajeta.codec.json.Json`) doesn't resolve through the dot
        // chain since `cajeta`/`codec`/`json` aren't classes.
        if (auto nestedClass = std::dynamic_pointer_cast<CajetaClass>(ty)) {
            return "out." + fieldName +
                   " = Json.parseObjectFromReader<" +
                   tcanon + ">(r);\n";
        }
        return "";
    }

    // Build the inner field-dispatch loop body. Used by both `parse`
    // (wrapped in a JsonReader-create preamble) and
    // `parseObjectFromReader` (called from inside another parse).
    // Expects locals `T out` and `JsonReader r` to be in scope before
    // entry, and consumes from START_OBJECT through END_OBJECT.
    std::string emitObjectLoopBody(const CajetaClassPtr& T,
                                    const std::string& indent) {
        std::ostringstream os;
        os << indent << "int32 t = r.next();\n";
        os << indent << "if (t != JsonToken.START_OBJECT) {\n";
        os << indent << "    throw heap JsonParseException(\n";
        os << indent << "        \"Tier-1 parse: expected '{'\", r.position());\n";
        os << indent << "}\n";
        os << indent << "while (true) {\n";
        os << indent << "    t = r.next();\n";
        os << indent << "    if (t == JsonToken.END_OBJECT) { return out; }\n";
        os << indent << "    if (t != JsonToken.KEY) {\n";
        os << indent << "        throw heap JsonParseException(\n";
        os << indent << "            \"Tier-1 parse: expected key\", r.position());\n";
        os << indent << "    }\n";
        os << indent << "    int8[] kb = r.currentBytes();\n";
        os << indent << "    int32 klen = (int32) kb.count();\n";
        bool first = true;
        for (auto& prop : T->getPropertyList()) {
            std::string assign = readFieldAssignment(prop);
            if (assign.empty()) continue;
            os << indent << "    " << (first ? "if" : "else if")
               << " (" << keyBytesGuard(prop->getName()) << ") {\n";
            os << indent << "        " << assign;
            os << indent << "    }\n";
            first = false;
        }
        // Unknown-key arm — consume the value's first token. v1 doesn't
        // recurse into unknown objects/arrays; that lands when a real
        // JsonReader.skipValue() is wired through.
        os << indent << "    else { t = r.next(); }\n";
        os << indent << "}\n";
        return os.str();
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
        // Fully-qualify type names in the body — the wrapper class
        // lives in Json's module (cajeta.codec.json), so short names
        // for user-package types wouldn't resolve.
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static " << Tcanon
           << " " << methodName << "(int8[] bytes, int64 length) {\n";
        os << "    " << Tcanon << " out = heap " << Tcanon << "();\n";
        os << "    JsonReader r = heap JsonReader(bytes, length);\n";
        os << emitObjectLoopBody(T, "    ");
        os << "}\n";
        return os.str();
    }

    // Variant of synthesizeParseBody for the from-existing-reader
    // entry point. Same field-dispatch loop body; signature takes a
    // JsonReader instead of bytes/length.
    std::string synthesizeParseFromReaderBody(const CajetaClassPtr& T,
                                                const std::string& methodName) {
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static " << Tcanon
           << " " << methodName << "(JsonReader r) {\n";
        os << "    " << Tcanon << " out = heap " << Tcanon << "();\n";
        os << emitObjectLoopBody(T, "    ");
        os << "}\n";
        return os.str();
    }

    // Emit a key-bytes-and-call sequence for one field. Returns empty
    // if the field type isn't yet supported by the writer arm.
    std::string writeFieldEmit(const StructurePropertyPtr& prop) {
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        const std::string& tcanon = ty->getQName()->toCanonical();
        std::ostringstream value;
        if (tcanon == "int32") {
            value << "w.writeNumber((int64) value." << fieldName << ");\n";
        } else if (tcanon == "int64") {
            value << "w.writeNumber(value." << fieldName << ");\n";
        } else if (tcanon == "boolean") {
            value << "w.writeBoolean(value." << fieldName << ");\n";
        } else if (tcanon == "float64") {
            value << "w.writeNumber(value." << fieldName << ");\n";
        } else if (tcanon == "cajeta.lang.String") {
            // Read String's bytes and byteLength fields directly; the
            // writer copies bytes through with quote/escape handling.
            value << "w.writeString(value." << fieldName
                  << ".bytes, value." << fieldName << ".byteLength);\n";
        } else if (std::dynamic_pointer_cast<CajetaClass>(ty)) {
            // Nested class field — recurse via toBytesObjectInto.
            // Short name `Json` for same-package reasons as the read side.
            value << "Json.toBytesObjectInto<"
                  << tcanon << ">(w, value." << fieldName << ");\n";
        } else {
            return "";
        }
        std::ostringstream os;
        os << "        {\n";
        os << "            int8[] k = new int8[" << fieldName.size() << "];\n";
        for (size_t i = 0; i < fieldName.size(); ++i) {
            unsigned char b = (unsigned char) fieldName[i];
            os << "            k[" << i << "] = (int8) 0x"
               << std::hex << (int) b << std::dec << ";\n";
        }
        os << "            w.key(k, " << fieldName.size() << ");\n";
        os << "            " << value.str();
        os << "        }\n";
        return os.str();
    }

    // Emit the inner `w.beginObject() ... per-field ... w.endObject()`
    // sequence. Used by both `toBytes` (which wraps it with a fresh
    // JsonWriter create + toBytes finalize) and `toBytesObjectInto`
    // (which receives the writer as a parameter and shares it with
    // the parent emit).
    std::string emitObjectWriteBody(const CajetaClassPtr& T) {
        std::ostringstream os;
        os << "    w.beginObject();\n";
        for (auto& prop : T->getPropertyList()) {
            std::string emit = writeFieldEmit(prop);
            if (!emit.empty()) os << emit;
        }
        os << "    w.endObject();\n";
        return os.str();
    }

    std::string synthesizeToBytesBody(const CajetaClassPtr& T,
                                       const std::string& methodName) {
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static int8[] " << methodName
           << "(" << Tcanon << " value) {\n";
        os << "    JsonWriter w = heap JsonWriter();\n";
        os << emitObjectWriteBody(T);
        os << "    return w.toBytes();\n";
        os << "}\n";
        return os.str();
    }

    // toBytesObjectInto variant — caller supplies the JsonWriter.
    // No fresh writer creation, no toBytes finalize; just emit the
    // begin/end-object pair around the field writes.
    std::string synthesizeToBytesObjectIntoBody(const CajetaClassPtr& T,
                                                  const std::string& methodName) {
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static void " << methodName
           << "(JsonWriter w, " << Tcanon << " value) {\n";
        os << emitObjectWriteBody(T);
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
        auto dumpIfRequested = [&](const std::string& body) {
            if (const char* dump = std::getenv("CAJETA_DUMP_IR")) {
                if (dump[0] == '1') {
                    std::cerr << "[JsonSynthesizer] " << methodName << "<"
                              << T->getQName()->toCanonical() << ">:\n"
                              << body << "\n";
                }
            }
        };
        // Only the templated variants (T-parameterized) should hit the
        // synthesizer — the non-templated parse(int8[], int64) /
        // toBytes(JsonValue) Tier-3 paths have real bodies. The
        // method-template instantiator only calls in here when an
        // instantiation is actually being performed, so by construction
        // methodName here names a templated method declared on Json.
        if (methodName == "parse") {
            out = synthesizeParseBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        if (methodName == "parseObjectFromReader") {
            out = synthesizeParseFromReaderBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        if (methodName == "toBytes") {
            out = synthesizeToBytesBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        if (methodName == "toBytesObjectInto") {
            out = synthesizeToBytesObjectIntoBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        return false;
    }

} // namespace cajeta
