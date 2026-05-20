// Phase 4b — Tier 1 JSON codegen synthesizer (see header).

#include "JsonSynthesizer.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaType.h"
#include "../type/StructureProperty.h"
#include "../type/QualifiedName.h"

#include <iostream>
#include <sstream>

namespace cajeta {

    namespace {

    // True if this field is excluded from JSON serialization +
    // deserialization via `@JsonIgnore`. The synthesizer skips ignored
    // fields entirely on both read and write — no key-arm emitted on
    // read (the field falls into the catch-all skip-value arm
    // naturally), no key-value pair emitted on write.
    bool isJsonIgnored(const StructurePropertyPtr& prop) {
        return prop && prop->findAnnotation("JsonIgnore") != nullptr;
    }

    // Effective wire key for the field. `@JsonProperty("custom_name")`
    // overrides the declared name; otherwise the declared name is the
    // key as before. An empty `@JsonProperty()` annotation (no value)
    // falls back to the declared name too. Annotations declared on
    // multiple fields with the same effective key are user error —
    // v1 doesn't reject (the JSON would be ambiguous on read), but a
    // future commit can add the lint.
    std::string effectiveJsonKey(const StructurePropertyPtr& prop) {
        if (!prop) return std::string();
        if (auto ann = prop->findAnnotation("JsonProperty")) {
            std::string renamed = ann->getString("value");
            if (!renamed.empty()) return renamed;
        }
        return prop->getName();
    }

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
    // Emit the single-element read into the local `cajeta.collection.
    // ArrayList<E>` accumulator `tmp_<field>`. The caller has already
    // consumed the value's first token via `t = r.next()`, so for
    // primitive readers (`currentNumberAsInt32` etc.) the reader is
    // positioned on the value token. Nested-class elements need to
    // hand the reader to `Json.parseObjectFromReader<E>` instead — but
    // that helper expects to consume the START_OBJECT itself, so the
    // caller's `t = r.next()` must NOT have advanced past it. The
    // array loop handles that by reading the token, checking for
    // END_ARRAY, and only calling the element reader on a real value
    // — for nested-class elements the loop pattern differs (peek the
    // token, dispatch on it) and is emitted by the array-read
    // builder, not this helper.
    //
    // Returns the statement(s) to add one element to the accumulator
    // for value tokens. Empty string for unsupported element types.
    std::string readArrayElementToList(const std::string& fieldName,
                                        const std::string& elementCanon,
                                        bool elementIsClass) {
        const std::string list = "tmp_" + fieldName;
        if (elementCanon == "int32") {
            return list + ".add(r.currentNumberAsInt32());\n";
        }
        if (elementCanon == "int64") {
            return list + ".add(r.currentNumberAsInt64());\n";
        }
        if (elementCanon == "boolean") {
            return list + ".add(r.currentBoolean());\n";
        }
        if (elementCanon == "float64") {
            return list + ".add(r.currentNumberAsFloat64());\n";
        }
        if (elementCanon == "cajeta.lang.String") {
            // Reader leaves currentBytes pointed at the STRING
            // payload (quotes stripped). Wrap in a view-mode
            // cajeta.lang.String.
            return "int8[] sb_" + fieldName + " = r.currentBytes();\n"
                   "                    " + list + ".add(heap cajeta.lang.String("
                   "sb_" + fieldName +
                   ", (int32) sb_" + fieldName + ".count()));\n";
        }
        if (elementIsClass) {
            // For nested-class elements we need the caller to NOT
            // pre-consume the START_OBJECT token, so this branch is
            // routed via a separate code path in
            // readArrayField. Returning empty here signals "use the
            // class-element loop body."
            return "";
        }
        return "";
    }

    // Build the value-side body for an array-typed field. Caller has
    // already matched the key; this block consumes from the value's
    // first token (which the array-read fragment fetches itself with
    // `t = r.next()`) through to assignment of `out.<fieldName>`. For
    // primitive / String element types the loop's body reads one
    // value and appends to the accumulator; for nested-class elements
    // the loop's body recurses into `Json.parseObjectFromReader<E>`
    // which expects the START_OBJECT token to NOT have been consumed
    // yet, so we peek-by-token and dispatch.
    //
    // `cajeta.collection.ArrayList<E>` is the accumulator. After the
    // END_ARRAY the entries are copied into a freshly-allocated
    // `E[]` sized to the accumulator's count(). For E=class, the
    // generated `new E[sz]` allocates an array of pointers; each
    // pointer is set from the corresponding accumulator entry.
    std::string readArrayField(const std::string& fieldName,
                                CajetaTypePtr elementType) {
        if (!elementType || !elementType->getQName()) return "";
        const std::string& etcanon = elementType->getQName()->toCanonical();
        bool elementIsClass =
            std::dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        std::string elementAdd = readArrayElementToList(
            fieldName, etcanon, elementIsClass);
        if (elementAdd.empty() && !elementIsClass) return "";

        std::ostringstream os;
        os << "t = r.next();\n";
        os << "            if (t != JsonToken.START_ARRAY) {\n";
        os << "                throw heap JsonParseException(\n";
        os << "                    \"Tier-1 parse: expected '['\", r.position());\n";
        os << "            }\n";
        os << "            cajeta.collection.ArrayList<" << etcanon
           << "> tmp_" << fieldName
           << " = heap cajeta.collection.ArrayList<" << etcanon << ">();\n";
        os << "            boolean done_" << fieldName << " = false;\n";
        os << "            while (!done_" << fieldName << ") {\n";
        if (elementIsClass) {
            // Peek-by-token: read the next token; if END_ARRAY, done;
            // otherwise it must be START_OBJECT — but
            // parseObjectFromReader expects the reader positioned
            // BEFORE START_OBJECT and consumes it itself, so we have
            // to inline a token check + nested loop here rather than
            // re-using parseObjectFromReader directly. The cheap
            // route: a sibling synthesized helper that takes a
            // JsonReader already pointing AT the START_OBJECT works
            // too — that's what parseObjectFromReader is meant to be,
            // and matches the design in synthesizeParseFromReaderBody
            // (it reads START_OBJECT then KEY-dispatches). So this
            // branch can just call parseObjectFromReader<E>(r) AFTER
            // checking for END_ARRAY by peeking — but the reader
            // doesn't expose a peek. The pull-only contract means we
            // do consume the token; if it's START_OBJECT, we can't
            // un-consume. The fix is to push a synthesized
            // parseObjectFromAlreadyStartedReader<E> helper, but
            // that's a larger restructuring. For v1 we accept the
            // limitation: nested-class arrays are deferred until a
            // peek surface lands on JsonReader. Emit a throw so the
            // user sees a clear deferral message rather than silent
            // misbehavior.
            os << "                throw heap JsonParseException(\n";
            os << "                    \"Tier-1 parse: nested-class arrays not "
                  "implemented in v1 (needs JsonReader peek)\", r.position());\n";
        } else {
            os << "                t = r.next();\n";
            os << "                if (t == JsonToken.END_ARRAY) {\n";
            os << "                    done_" << fieldName << " = true;\n";
            os << "                } else {\n";
            os << "                    " << elementAdd;
            os << "                }\n";
        }
        os << "            }\n";
        // Assign the fresh int32[] / etc. DIRECTLY into out.<field>
        // — no intermediate local. The intermediate would register a
        // drop entry that fires at this function's scope exit (before
        // the caller can read out.<field>), and __cajeta_free_array
        // would atomically claim the buffer out of the live-allocation
        // set BEFORE the field auto-drop on `out` has a chance to no-op
        // on it. Result: the caller's `b.field[i]` reads freed memory.
        // Going field-direct keeps the buffer's only "named" reference
        // on the heap class, so the live-set claim runs first through
        // the class's auto-field-drop walk where it belongs.
        os << "            int32 sz_" << fieldName << " = tmp_"
           << fieldName << ".size();\n";
        os << "            out." << fieldName << " = new " << etcanon
           << "[sz_" << fieldName << "];\n";
        os << "            int32 ii_" << fieldName << " = 0;\n";
        os << "            while (ii_" << fieldName << " < sz_"
           << fieldName << ") {\n";
        os << "                out." << fieldName << "[ii_"
           << fieldName << "] = tmp_" << fieldName
           << ".get(ii_" << fieldName << ");\n";
        os << "                ii_" << fieldName << " = ii_"
           << fieldName << " + 1;\n";
        os << "            }\n";
        return os.str();
    }

    std::string readFieldAssignment(const StructurePropertyPtr& prop) {
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        // Array-typed fields take a different path — they're CajetaArray
        // which inherits from CajetaClass, so the catch-all class branch
        // below would treat them as nested objects. Check first.
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            return readArrayField(fieldName, arr->getElementType());
        }
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
            // @JsonIgnore: drop the per-field arm entirely so JSON
            // input with this key falls through to the unknown-key
            // skip arm (the field stays at its default).
            if (isJsonIgnored(prop)) continue;
            std::string assign = readFieldAssignment(prop);
            if (assign.empty()) continue;
            os << indent << "    " << (first ? "if" : "else if")
               << " (" << keyBytesGuard(effectiveJsonKey(prop)) << ") {\n";
            os << indent << "        " << assign;
            os << indent << "    }\n";
            first = false;
        }
        // Unknown-key arm — consume the value's first token. v1 doesn't
        // recurse into unknown objects/arrays; that lands when a real
        // JsonReader.skipValue() is wired through. When no per-field
        // arms emitted (e.g. every field carries @JsonIgnore, or T has
        // no parseable fields at all), use bare `{ ... }` instead of
        // `else { ... }` — a dangling `else` is a parse error.
        if (first) {
            os << indent << "    { t = r.next(); }\n";
        } else {
            os << indent << "    else { t = r.next(); }\n";
        }
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

    // Emit the value-side body for an array field: begin-array,
    // walk every element with the appropriate per-type writer
    // call, end-array. Element kinds match the reader's: int32,
    // int64, boolean, float64, cajeta.lang.String, and nested
    // class types. v1 assumes the field reference is non-null;
    // a null array trips the universal `count()` deref at
    // runtime. A `value.<field> != null` guard with a `w.writeNull()`
    // fallback is a planned follow-up alongside the @JsonInclude
    // annotation work.
    std::string writeArrayValue(const std::string& fieldName,
                                 CajetaTypePtr elementType) {
        if (!elementType || !elementType->getQName()) return "";
        const std::string& etcanon = elementType->getQName()->toCanonical();
        bool elementIsClass =
            std::dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        // Load the array element into a typed local first, then hand
        // that local to the writer call. Going `w.writeNumber(value.ns
        // [wi])` directly hands the GEP slot-pointer to the call site;
        // MethodCallExpression's arg-coerce path doesn't reach into
        // ArrayIndexExpression-typed slot pointers, so the JIT verifier
        // rejects with "Call parameter type does not match function
        // signature" for ints/floats wider than the array's element
        // type. The temp-local pattern routes through the
        // assignment-side load path which DOES materialize the value.
        // (int32 happened to work pre-fix because the cast `(int64)`
        // on it triggered a load along the way.)
        std::string elemVar = "ev_" + fieldName;
        std::string writeOne;
        if (etcanon == "int32") {
            writeOne = "int32 " + elemVar + " = value." + fieldName +
                       "[wi_" + fieldName + "]; "
                       "w.writeNumber((int64) " + elemVar + ");";
        } else if (etcanon == "int64") {
            writeOne = "int64 " + elemVar + " = value." + fieldName +
                       "[wi_" + fieldName + "]; "
                       "w.writeNumber(" + elemVar + ");";
        } else if (etcanon == "boolean") {
            writeOne = "boolean " + elemVar + " = value." + fieldName +
                       "[wi_" + fieldName + "]; "
                       "w.writeBoolean(" + elemVar + ");";
        } else if (etcanon == "float64") {
            writeOne = "float64 " + elemVar + " = value." + fieldName +
                       "[wi_" + fieldName + "]; "
                       "w.writeNumber(" + elemVar + ");";
        } else if (etcanon == "cajeta.lang.String") {
            writeOne = "cajeta.lang.String " + elemVar + " = value." +
                       fieldName + "[wi_" + fieldName + "]; "
                       "w.writeString(" + elemVar + ".bytes, " +
                       elemVar + ".byteLength);";
        } else if (elementIsClass) {
            writeOne = etcanon + " " + elemVar + " = value." +
                       fieldName + "[wi_" + fieldName + "]; "
                       "Json.toBytesObjectInto<" + etcanon + ">(w, " +
                       elemVar + ");";
        } else {
            return "";
        }
        std::ostringstream os;
        os << "w.beginArray();\n";
        os << "            int32 wn_" << fieldName
           << " = (int32) value." << fieldName << ".count();\n";
        os << "            int32 wi_" << fieldName << " = 0;\n";
        os << "            while (wi_" << fieldName << " < wn_"
           << fieldName << ") {\n";
        os << "                " << writeOne << "\n";
        os << "                wi_" << fieldName << " = wi_"
           << fieldName << " + 1;\n";
        os << "            }\n";
        os << "            w.endArray();\n";
        return os.str();
    }

    // Emit a key-bytes-and-call sequence for one field. Returns empty
    // if the field type isn't yet supported by the writer arm.
    std::string writeFieldEmit(const StructurePropertyPtr& prop) {
        // @JsonIgnore: don't emit a key/value pair for this field.
        if (isJsonIgnored(prop)) return "";
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        // Array-typed fields take the dedicated array path — same
        // reason as readFieldAssignment: CajetaArray inherits CajetaClass
        // and the catch-all class branch would treat them as nested
        // objects.
        std::ostringstream value;
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            std::string arrEmit = writeArrayValue(fieldName, arr->getElementType());
            if (arrEmit.empty()) return "";
            value << arrEmit;
        } else {
            const std::string& tcanon = ty->getQName()->toCanonical();
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
        }
        // Use the @JsonProperty-renamed key when present; otherwise the
        // declared field name. Cajeta-level field access (`value.<fieldName>`)
        // still uses the declared name — only the wire-bytes change.
        std::string wireKey = effectiveJsonKey(prop);
        std::ostringstream os;
        os << "        {\n";
        os << "            int8[] k = new int8[" << wireKey.size() << "];\n";
        for (size_t i = 0; i < wireKey.size(); ++i) {
            unsigned char b = (unsigned char) wireKey[i];
            os << "            k[" << i << "] = (int8) 0x"
               << std::hex << (int) b << std::dec << ";\n";
        }
        os << "            w.key(k, " << wireKey.size() << ");\n";
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

    // Param-type canonical at index `i`, or empty if out of range.
    std::string paramCanonAt(
            const std::vector<CajetaTypePtr>& paramTypes, size_t i) {
        if (i >= paramTypes.size()) return std::string();
        auto& p = paramTypes[i];
        if (!p || !p->getQName()) return std::string();
        return p->getQName()->toCanonical();
    }

    } // namespace

    bool synthesizeJsonMethodSource(
            const CajetaClassPtr& parent,
            const std::string& methodName,
            const std::vector<CajetaTypePtr>& args,
            const std::vector<CajetaTypePtr>& paramTypes,
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
        // Match by name AND param-type signature so overloads (e.g.
        // `parse<T>(String)` which delegates to `parse<T>(int8[],
        // int64)`) keep their hand-written delegation bodies. Without
        // the param check the synthesizer would silently overwrite
        // every overload of the same name with the byte-walking body
        // even when its formals don't have `bytes`/`length` in scope.
        if (methodName == "parse"
                && paramTypes.size() == 2
                && paramCanonAt(paramTypes, 0) == "int8[]"
                && paramCanonAt(paramTypes, 1) == "int64") {
            out = synthesizeParseBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        if (methodName == "parseObjectFromReader"
                && paramTypes.size() == 1
                && paramCanonAt(paramTypes, 0) == "cajeta.codec.json.JsonReader") {
            out = synthesizeParseFromReaderBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        // Type-position match: the param's canonical is EITHER the
        // template-parameter placeholder name "T" (the unsubstituted
        // form, which is what the captured methodSource carries) OR
        // the concrete substituted canonical (in case a future pass
        // pre-substitutes parameter types before the synthesizer runs).
        // Cajeta-style convention is single-letter "T" for the type
        // parameter; user-named ones (e.g. `<R>`) would need expansion
        // here if they cropped up — none do in v1.
        auto isTPosition = [&](const std::string& canon) -> bool {
            return canon == "T"
                || canon == T->getQName()->toCanonical();
        };
        if (methodName == "toBytes"
                && paramTypes.size() == 1
                && isTPosition(paramCanonAt(paramTypes, 0))) {
            out = synthesizeToBytesBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        if (methodName == "toBytesObjectInto"
                && paramTypes.size() == 2
                && paramCanonAt(paramTypes, 0) == "cajeta.codec.json.JsonWriter"
                && isTPosition(paramCanonAt(paramTypes, 1))) {
            out = synthesizeToBytesObjectIntoBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        return false;
    }

} // namespace cajeta
