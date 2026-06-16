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

    // True if this field is excluded from JSON deserialization
    // (READ side). See `docs/stdlib/codec/Json.md` § Field-
    // level annotations for the asymmetric @JsonIgnore semantics:
    //
    //   @JsonIgnore                              — both directions skipped (bare).
    //   @JsonIgnore(onRead=true)                 — read-skip only.
    //   @JsonIgnore(onWrite=true)                — write-skip only.
    //   @JsonIgnore(onRead=true, onWrite=true)   — both (same as bare).
    //
    // The bare-annotation default is "skip both" — most users mean
    // "completely ignore this field." When ANY arg is supplied,
    // unspecified directions default to false (don't skip), so the
    // user only has to call out the direction they care about.
    bool isJsonIgnoredOnRead(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ann = prop->findAnnotation("JsonIgnore");
        if (!ann) return false;
        if (ann->getArgs().empty()) return true;   // bare = both directions.
        return ann->getBool("onRead", false);
    }

    bool isJsonIgnoredOnWrite(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ann = prop->findAnnotation("JsonIgnore");
        if (!ann) return false;
        if (ann->getArgs().empty()) return true;   // bare = both directions.
        return ann->getBool("onWrite", false);
    }

    // Back-compat helper for call sites that don't yet need the
    // per-direction split (`isJsonRequired` overrides). True only
    // when BOTH directions are skipped — a partially-ignored field
    // with @JsonRequired still has a meaningful read direction.
    bool isJsonIgnored(const StructurePropertyPtr& prop) {
        return isJsonIgnoredOnRead(prop) && isJsonIgnoredOnWrite(prop);
    }

    // True if `@JsonRequired` is set on the field — the parse body
    // throws `JsonParseException` after END_OBJECT if no key arm fired
    // for this field. Tracks via a per-field boolean local
    // (`req_seen_<fieldName>`) flipped to true inside the matching
    // arm. Ignored fields can't be required (would never be seen) —
    // we silently treat @JsonIgnore as overriding @JsonRequired so a
    // user who annotates both ends up with the @JsonIgnore behavior.
    // True if the field's static type is `cajeta.lang.Optional<T>`.
    // The synthesizer routes Optional fields through a dedicated
    // arm that distinguishes "key absent" (field stays at default
    // null) from "key present, value null" (Optional.empty()) from
    // "key present, value present" (Optional.of(v)). Inner T must
    // be a supported primitive (int32 / int64 / boolean / float64)
    // or `cajeta.lang.String` in v1; nested-class Optional<T>
    // lands when the inner-type dispatch shares more code with the
    // non-Optional arm.
    bool isOptionalField(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ty = prop->getType();
        if (!ty) return false;
        auto cls = std::dynamic_pointer_cast<CajetaClass>(ty);
        if (!cls || !cls->getQName()) return false;
        // The type-name carries the template-args suffix for
        // instantiated classes (e.g. "Optional<int32>"). Match on
        // the bare-name prefix so both the template form
        // ("Optional") and the instantiation form ("Optional<…>")
        // hit. Package is always "cajeta.lang" for the stdlib type.
        const std::string& name = cls->getQName()->getTypeName();
        bool nameMatch = (name == "Optional")
            || name.compare(0, 9, "Optional<") == 0;
        return nameMatch
            && cls->getQName()->getPackageName() == "cajeta.lang";
    }

    // Inner type of an `Optional<T>` field, or null when the field
    // isn't an Optional or the type-args aren't populated yet.
    CajetaTypePtr optionalInnerType(const StructurePropertyPtr& prop) {
        if (!isOptionalField(prop)) return nullptr;
        auto cls = std::dynamic_pointer_cast<CajetaClass>(prop->getType());
        if (!cls) return nullptr;
        auto& args = cls->getTypeArguments();
        if (args.empty()) return nullptr;
        return args[0];
    }

    // True if `@JsonRaw` is set — the field's wire bytes pass
    // through both directions unchanged. Read side captures via
    // `r.currentRawBytes()`; write side emits via `w.writeRaw`.
    // The field's static type must be `int8[]` in v1 — the bytes
    // are the wire form including any quotes / delimiters.
    bool isJsonRaw(const StructurePropertyPtr& prop) {
        return prop && prop->findAnnotation("JsonRaw") != nullptr;
    }

    bool isJsonRequired(const StructurePropertyPtr& prop) {
        return prop
            && !isJsonIgnoredOnRead(prop)
            && prop->findAnnotation("JsonRequired") != nullptr;
    }

    // Apply a class-level naming strategy to a field's declared name
    // to derive the wire key. All five strategies from
    // `docs/stdlib/codec/Json.md` § Class-level naming:
    //
    //   - `"SNAKE_CASE"`  — `firstName` → `first_name`
    //   - `"KEBAB_CASE"`  — `firstName` → `first-name`
    //   - `"PASCAL_CASE"` — `firstName` → `FirstName`
    //   - `"CAMEL_CASE"`  — `firstName` → `firstName` (identity for
    //                       cajeta's camelCase field convention).
    //                       The strategy exists so user code can be
    //                       explicit about its policy and reviewers
    //                       can spot the wire-key contract at a
    //                       glance.
    //   - `"IDENTITY"`    — explicit no-op.
    //
    // Empty string (no @JsonNamingStrategy annotation) is also no-op.
    // Matching is exact case-sensitive on the annotation string.
    std::string applyNamingStrategy(const std::string& strategy,
                                     const std::string& declaredName) {
        // No-op strategies (and the no-annotation case).
        if (strategy.empty() || strategy == "IDENTITY" || strategy == "CAMEL_CASE") {
            return declaredName;
        }
        if (strategy == "PASCAL_CASE") {
            if (declaredName.empty()) return declaredName;
            std::string out = declaredName;
            if (out[0] >= 'a' && out[0] <= 'z') {
                out[0] = (char) (out[0] - 'a' + 'A');
            }
            return out;
        }
        if (strategy == "SNAKE_CASE" || strategy == "KEBAB_CASE") {
            char sep = (strategy == "SNAKE_CASE") ? '_' : '-';
            std::string out;
            out.reserve(declaredName.size() + 4);
            for (size_t i = 0; i < declaredName.size(); ++i) {
                char c = declaredName[i];
                if (i > 0 && c >= 'A' && c <= 'Z') {
                    out.push_back(sep);
                    out.push_back((char) (c - 'A' + 'a'));
                } else if (c >= 'A' && c <= 'Z') {
                    out.push_back((char) (c - 'A' + 'a'));
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }
        // Unrecognized strategy name — leave as-is. Future strategies
        // (UPPER_SNAKE_CASE, LOWER_CASE) drop in here as additional
        // branches.
        return declaredName;
    }

    // Effective wire key for the field. Precedence:
    //   1. `@JsonProperty("custom_name")` on the field — wins
    //      unconditionally (per-field overrides class-level).
    //   2. Class-level `@JsonNamingStrategy("SNAKE_CASE" | "KEBAB_CASE")`
    //      transform applied to the declared name.
    //   3. The declared name verbatim.
    //
    // Empty `@JsonProperty()` (no value arg) falls through to the
    // strategy layer rather than overwriting with empty.
    std::string effectiveJsonKey(const StructurePropertyPtr& prop,
                                  const std::string& classStrategy) {
        if (!prop) return std::string();
        if (auto ann = prop->findAnnotation("JsonProperty")) {
            std::string renamed = ann->getString("value");
            if (!renamed.empty()) return renamed;
        }
        return applyNamingStrategy(classStrategy, prop->getName());
    }

    // Class-level naming strategy from `@JsonNamingStrategy("...")`.
    // Returns empty when absent. Field-level @JsonProperty always
    // wins over this.
    std::string classNamingStrategy(const CajetaClassPtr& T) {
        if (!T) return std::string();
        if (auto ann = T->findAnnotation("JsonNamingStrategy")) {
            return ann->getString("value");
        }
        return std::string();
    }

    // True when class-level `@JsonStrict` is set — read-side rejects
    // unknown keys with a JsonParseException instead of silently
    // consuming the value (the default v1 behavior).
    bool classIsStrict(const CajetaClassPtr& T) {
        return T && T->findAnnotation("JsonStrict") != nullptr;
    }

    // @JsonInclude policy on a field. All four spec values
    // (docs/stdlib/codec/Json.md § Field-level annotations):
    //
    //   - "ALWAYS" (default) — emit key+value unconditionally
    //   - "NON_NULL"         — emit only when the field reference
    //                          is not null. Only meaningful on
    //                          reference-typed fields (class, array,
    //                          String); a no-op on primitives.
    //   - "NEVER"            — never emit. Read-only field (can be
    //                          parsed, never echoed back). Pair with
    //                          @JsonIgnore for full read+write skip
    //                          if that's the intent.
    //   - "NON_DEFAULT"      — emit only when the field's value
    //                          differs from its type's default:
    //                          0 for int*, 0.0 for float*, false
    //                          for boolean, null for class refs,
    //                          null for arrays, null for String
    //                          (String is a class ref post Phase
    //                          2b-β).
    //
    // Returns the string verbatim from the annotation. Empty string
    // when the annotation is absent.
    std::string jsonIncludePolicy(const StructurePropertyPtr& prop) {
        if (!prop) return std::string();
        if (auto ann = prop->findAnnotation("JsonInclude")) {
            return ann->getString("value");
        }
        return std::string();
    }

    // True if the field's static type is reference-shaped (its slot
    // can hold null). Used by the @JsonInclude(NON_NULL) gate.
    // Primitives are never null and so the gate is a no-op there.
    bool fieldIsReferenceTyped(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ty = prop->getType();
        if (!ty || !ty->getQName()) return false;
        if (std::dynamic_pointer_cast<CajetaArray>(ty)) return true;
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return true;
        return false;
    }

    // Aliases declared on the field via `@JsonAlias({"a", "b", ...})`.
    // Returned in the order the user listed them. Empty if no
    // annotation. The aliases extend the read-side key match — write
    // still uses the primary (declared name or @JsonProperty) key.
    std::vector<std::string> jsonAliases(const StructurePropertyPtr& prop) {
        std::vector<std::string> out;
        if (!prop) return out;
        if (auto ann = prop->findAnnotation("JsonAlias")) {
            const auto& list = ann->getStringList("value");
            for (auto& s : list) out.push_back(s);
            if (out.empty()) {
                // Single-value form: `@JsonAlias("userId")`.
                std::string single = ann->getString("value");
                if (!single.empty()) out.push_back(single);
            }
        }
        return out;
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
            // payload (quotes stripped). Wrap in a String instance,
            // transferring the buffer via `#sb_` so the local's drop
            // entry deactivates before scope exit — otherwise the
            // freshly-constructed String dangles.
            return "int8[] sb_" + fieldName + " = r.currentBytes();\n"
                   "                    int32 sl_" + fieldName +
                   " = (int32) sb_" + fieldName + ".count();\n"
                   "                    " + list + ".add(heap cajeta.lang.String("
                   "#sb_" + fieldName +
                   ", sl_" + fieldName + "));\n";
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
    // generated `heap E[sz]` allocates an array of pointers; each
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
            // Nested-class arrays: peek the next token via
            // `r.peek()` (added Phase 4b commit 10). If it's
            // END_ARRAY, consume and exit the loop; otherwise the
            // peeked token IS the start of an element object, and
            // we hand the reader (with the cached peek still in
            // place) to `Json.parseObjectFromReader<E>(r)`. That
            // helper's first `r.next()` returns the cached
            // START_OBJECT and the inner parse proceeds normally.
            //
            // The recursive-parse result MUST go through an explicit
            // temp local before the `tmp.add(...)` call —
            // `tmp.add(Json.parse...)` inline as a single chained
            // expression has a codegen miss where the call to add()
            // is silently dropped (the arg expression evaluates but
            // the outer method dispatch never fires). Until that's
            // root-caused, route through the temp pattern.
            os << "                JsonToken pt_" << fieldName
               << " = r.peek();\n";
            os << "                if (pt_" << fieldName
               << " == JsonToken.END_ARRAY) {\n";
            os << "                    r.next();\n"
               << "                    done_" << fieldName << " = true;\n";
            os << "                } else {\n";
            // Transfer the parsed element via `#` so the temp local's
            // drop entry deactivates on the add — without it, the
            // intermediate's drop fires at this method's scope exit
            // BEFORE the caller can read `out.items[i]`, freeing the
            // Inner that `out.items[i]` references.
            os << "                    " << etcanon << " elem_"
               << fieldName
               << " = Json.parseObjectFromReader<" << etcanon
               << ">(r);\n";
            os << "                    tmp_" << fieldName
               << ".add(#elem_" << fieldName << ");\n";
            os << "                }\n";
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
           << fieldName << ".count();\n";
        os << "            out." << fieldName << " = heap " << etcanon
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
        // @JsonRaw passes the wire bytes through unchanged. Field
        // must be int8[]; the captured bytes include any quotes /
        // structural delimiters so a round-trip through
        // JsonWriter.writeRaw is byte-stable on primitives.
        if (isJsonRaw(prop)) {
            return "t = r.next();\n            out." + fieldName +
                   " = r.currentRawBytes();\n";
        }
        // Optional<T> field. Distinguishes "key absent" (handled
        // by the per-field arm NOT firing — field stays at null,
        // the default for class refs) from "key present, value
        // null" (Optional(false, default)) from "key present,
        // value V" (Optional(true, V)). Inner T must be a
        // primitive or cajeta.lang.String in v1.
        if (isOptionalField(prop)) {
            auto inner = optionalInnerType(prop);
            if (!inner || !inner->getQName()) return "";
            const std::string& innerCanon = inner->getQName()->toCanonical();
            std::string readInner;     // reads `inner` into local `v`
            std::string defaultInner;  // safe default for the empty-Optional value slot
            if (innerCanon == "int32") {
                readInner = "int32 v = r.currentNumberAsInt32();";
                defaultInner = "(int32) 0";
            } else if (innerCanon == "int64") {
                readInner = "int64 v = r.currentNumberAsInt64();";
                defaultInner = "(int64) 0";
            } else if (innerCanon == "boolean") {
                readInner = "boolean v = r.currentBoolean();";
                defaultInner = "false";
            } else if (innerCanon == "float64") {
                readInner = "float64 v = r.currentNumberAsFloat64();";
                defaultInner = "(float64) 0.0";
            } else if (innerCanon == "cajeta.lang.String") {
                // Same `#vb` transfer reasoning as the field-level
                // branch — the String ctor takes ownership of the
                // buffer; without `#`, the local's drop fires at
                // scope exit and the String dangles.
                readInner =
                    "int8[] vb = r.currentBytes();\n"
                    "                int32 vl = (int32) vb.count();\n"
                    "                cajeta.lang.String v = heap cajeta.lang.String("
                    "#vb, vl);";
                // Default for empty Optional<String> is `null` —
                // String is a class ref, null is the type-default,
                // and that avoids the spurious zero-length-String
                // allocation that would otherwise be a wasted view.
                defaultInner = "null";
            } else {
                // Unsupported inner type — leave the field at default.
                return "";
            }
            // For class-ref inner types (String) we move ownership
            // of the local `v` into the Optional via `#v` so the
            // local's scope-exit drop doesn't reclaim the wrapped
            // String out from under the field. Primitive inner
            // types don't need the `#` — they're value-copied.
            bool innerIsRef = (innerCanon == "cajeta.lang.String");
            std::string presentArg = innerIsRef ? "#v" : "v";
            std::ostringstream os;
            os << "t = r.next();\n"
               << "            if (t == JsonToken.NULL) {\n"
               << "                out." << fieldName
               << " = heap cajeta.lang.Optional<" << innerCanon
               << ">(false, " << defaultInner << ");\n"
               << "            } else {\n"
               << "                " << readInner << "\n"
               << "                out." << fieldName
               << " = heap cajeta.lang.Optional<" << innerCanon
               << ">(true, " << presentArg << ");\n"
               << "            }\n";
            return os.str();
        }
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
            // stripped) as an owned #int8[]. Wrap them in a String
            // instance via the (int8[], int32 byteLength) constructor,
            // transferring the buffer via `#` so the local's drop
            // entry deactivates — otherwise scope exit frees the
            // buffer the freshly-constructed String now points at,
            // and any subsequent read of the field dangles.
            return "t = r.next();\n"
                   "            int8[] vbytes_" + fieldName +
                       " = r.currentBytes();\n"
                   "            int32 vlen_" + fieldName +
                       " = (int32) vbytes_" + fieldName + ".count();\n"
                   "            out." + fieldName +
                       " = heap cajeta.lang.String("
                       "#vbytes_" + fieldName +
                       ", vlen_" + fieldName + ");\n";
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
        std::string strategy = classNamingStrategy(T);
        bool strict = classIsStrict(T);
        std::ostringstream os;
        os << indent << "JsonToken t = r.next();\n";
        os << indent << "if (t != JsonToken.START_OBJECT) {\n";
        os << indent << "    throw heap JsonParseException(\n";
        os << indent << "        \"Tier-1 parse: expected '{'\", r.position());\n";
        os << indent << "}\n";
        // Pre-loop: declare a `boolean req_seen_<field>` per
        // @JsonRequired field, initialized to false. Each matching key
        // arm flips its flag true; after END_OBJECT we walk the flags
        // and throw on any still-false.
        std::vector<std::string> requiredKeys;
        for (auto& prop : T->getPropertyList()) {
            if (isJsonRequired(prop)) {
                std::string key = effectiveJsonKey(prop, strategy);
                requiredKeys.push_back(key);
                os << indent << "boolean req_seen_" << prop->getName()
                   << " = false;\n";
            }
        }
        os << indent << "while (true) {\n";
        os << indent << "    t = r.next();\n";
        // END_OBJECT branch — verify required fields THEN return.
        if (requiredKeys.empty()) {
            os << indent << "    if (t == JsonToken.END_OBJECT) { return out; }\n";
        } else {
            os << indent << "    if (t == JsonToken.END_OBJECT) {\n";
            for (auto& prop : T->getPropertyList()) {
                if (!isJsonRequired(prop)) continue;
                std::string key = effectiveJsonKey(prop, strategy);
                os << indent << "        if (!req_seen_" << prop->getName()
                   << ") {\n";
                os << indent
                   << "            throw heap JsonParseException(\n";
                os << indent
                   << "                \"Tier-1 parse: required field '"
                   << key << "' missing\", r.position());\n";
                os << indent << "        }\n";
            }
            os << indent << "        return out;\n";
            os << indent << "    }\n";
        }
        os << indent << "    if (t != JsonToken.KEY) {\n";
        os << indent << "        throw heap JsonParseException(\n";
        os << indent << "            \"Tier-1 parse: expected key\", r.position());\n";
        os << indent << "    }\n";
        os << indent << "    int8[] kb = r.currentBytes();\n";
        os << indent << "    int32 klen = (int32) kb.count();\n";
        bool first = true;
        for (auto& prop : T->getPropertyList()) {
            // @JsonIgnore on the read side: drop the per-field arm
            // entirely so JSON input with this key falls through to
            // the unknown-key skip arm (the field stays at its
            // default). Asymmetric @JsonIgnore(onWrite=true) leaves
            // the read side untouched and only skips emission below.
            if (isJsonIgnoredOnRead(prop)) continue;
            std::string assign = readFieldAssignment(prop);
            if (assign.empty()) continue;
            // @JsonAlias adds alternate read-side keys. OR every
            // accepted key together in the arm guard. The primary
            // (effectiveJsonKey) comes first; aliases append after.
            std::string primary = effectiveJsonKey(prop, strategy);
            std::ostringstream guard;
            guard << "(" << keyBytesGuard(primary) << ")";
            for (auto& alias : jsonAliases(prop)) {
                guard << " || (" << keyBytesGuard(alias) << ")";
            }
            os << indent << "    " << (first ? "if" : "else if")
               << " (" << guard.str() << ") {\n";
            os << indent << "        " << assign;
            if (isJsonRequired(prop)) {
                os << indent << "        req_seen_" << prop->getName()
                   << " = true;\n";
            }
            os << indent << "    }\n";
            first = false;
        }
        // Unknown-key handling.
        //   - Default: recursively skip the whole value (scalar or a
        //     nested object/array subtree) via JsonReader.skipValue() —
        //     the unmapped bytes are consumed but never decoded (the
        //     on-demand skip: a struct only materializes the fields it
        //     maps).
        //   - `@JsonStrict` class-level: throw JsonParseException
        //     naming the unknown key.
        std::string unknownArmBody;
        if (strict) {
            std::ostringstream sb;
            sb << "throw heap JsonParseException("
               << "\"Tier-1 parse: unknown key (class is @JsonStrict)\", "
               << "r.position());";
            unknownArmBody = sb.str();
        } else {
            unknownArmBody = "r.skipValue();";
        }
        if (first) {
            os << indent << "    { " << unknownArmBody << " }\n";
        } else {
            os << indent << "    else { " << unknownArmBody << " }\n";
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
        //
        // The return type is `#T` (ownership-returning): the parsed
        // value `out` is freshly heap-allocated and handed to the
        // caller. Without the `#`, the multi-param borrow-return
        // check in Method::generatePrototype rejects the signature
        // — `public static T(int8[], int64)` looks like "return a
        // borrow from one of two parameters", which is exactly the
        // shape MemoryModel.md § Function signatures forbids.
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Tcanon
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
        os << "public static #" << Tcanon
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
    std::string writeFieldEmit(const StructurePropertyPtr& prop,
                                const std::string& classStrategy) {
        // @JsonIgnore on the write side: don't emit a key/value
        // pair for this field. Asymmetric @JsonIgnore(onRead=true)
        // leaves the write side untouched.
        if (isJsonIgnoredOnWrite(prop)) return "";
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        // Array-typed fields take the dedicated array path — same
        // reason as readFieldAssignment: CajetaArray inherits CajetaClass
        // and the catch-all class branch would treat them as nested
        // objects.
        std::ostringstream value;
        // @JsonRaw: pass the field's int8[] bytes verbatim. Same
        // wire-form round-trip as the read side captured via
        // currentRawBytes() — primitives only in v1.
        if (isJsonRaw(prop)) {
            value << "w.writeRaw(value." << fieldName
                  << ", (int32) value." << fieldName << ".count());\n";
        } else
        // Optional<T> field write side. The outer guard (added
        // below as guardExpr) handles the field-is-null case (key
        // omitted entirely); inside the guard we still need to
        // distinguish present-with-value from present-but-empty
        // (writeNull) and emit the inner type accordingly.
        if (isOptionalField(prop)) {
            auto inner = optionalInnerType(prop);
            if (!inner || !inner->getQName()) return "";
            const std::string& innerCanon = inner->getQName()->toCanonical();
            std::string innerWrite;
            if (innerCanon == "int32") {
                innerWrite = "w.writeNumber((int64) value." + fieldName + ".get());";
            } else if (innerCanon == "int64") {
                innerWrite = "w.writeNumber(value." + fieldName + ".get());";
            } else if (innerCanon == "boolean") {
                innerWrite = "w.writeBoolean(value." + fieldName + ".get());";
            } else if (innerCanon == "float64") {
                innerWrite = "w.writeNumber(value." + fieldName + ".get());";
            } else if (innerCanon == "cajeta.lang.String") {
                innerWrite =
                    "cajeta.lang.String os = value." + fieldName + ".get();\n"
                    "            w.writeString(os.bytes, os.byteLength);";
            } else {
                return "";
            }
            value << "if (value." << fieldName << ".isEmpty()) {\n"
                  << "                w.writeNull();\n"
                  << "            } else {\n"
                  << "                " << innerWrite << "\n"
                  << "            }\n";
        } else
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
        // @JsonInclude("NEVER") — never write. Read-only field;
        // parsing keeps the value but the writer always omits it.
        // Returning early here means the writer body emits NO key
        // and NO value for this field.
        std::string includePolicy = jsonIncludePolicy(prop);
        if (includePolicy == "NEVER") {
            return "";
        }
        // Use the @JsonProperty-renamed key when present, then the
        // class-level @JsonNamingStrategy transform, else the declared
        // name verbatim. Cajeta-level field access (`value.<fieldName>`)
        // still uses the declared name — only the wire-bytes change.
        std::string wireKey = effectiveJsonKey(prop, classStrategy);
        std::ostringstream os;
        // @JsonInclude conditional emission. Wraps the key+value
        // block in a per-policy guard.
        //
        //   - NON_NULL    — `value.f != null` (reference-typed only).
        //   - NON_DEFAULT — `value.f != <type default>`. For class
        //                   refs / arrays / Strings the default is
        //                   null, so NON_DEFAULT collapses to
        //                   NON_NULL on those. For primitives the
        //                   default is 0 / 0.0 / false.
        //   - ALWAYS / "" — no guard.
        std::string guardExpr;
        auto fieldTyName = prop->getType() && prop->getType()->getQName()
            ? prop->getType()->getQName()->toCanonical()
            : "";
        // Optional<T> fields auto-guard on `value.field != null`:
        // a null Optional ref means the field is "absent" — key
        // omitted. Inside the guard the inner emit branches on
        // isEmpty() to choose writeNull vs the inner value.
        // Explicit @JsonInclude("ALWAYS") opts out and would
        // NPE at runtime if the field is null — user beware.
        if (isOptionalField(prop) && includePolicy != "ALWAYS") {
            guardExpr = "value." + fieldName + " != null";
        } else if (includePolicy == "NON_NULL" && fieldIsReferenceTyped(prop)) {
            guardExpr = "value." + fieldName + " != null";
        } else if (includePolicy == "NON_DEFAULT") {
            if (fieldIsReferenceTyped(prop)
                    || fieldTyName == "cajeta.lang.String") {
                guardExpr = "value." + fieldName + " != null";
            } else if (fieldTyName == "boolean") {
                guardExpr = "value." + fieldName + " != false";
            } else if (fieldTyName == "float32" || fieldTyName == "float64") {
                guardExpr = "value." + fieldName + " != 0.0";
            } else {
                // Integer-family primitives (int8/16/32/64 and unsigned).
                guardExpr = "value." + fieldName + " != 0";
            }
        }
        if (!guardExpr.empty()) {
            os << "        if (" << guardExpr << ") {\n";
        } else {
            os << "        {\n";
        }
        // String-literal key emission. Pre Phase 2b-β the synthesizer
        // emitted N lines of `int8[] k = new int8[N]; k[0] = (int8)
        // 0x..; ...; w.key(k, N);` because String was an opaque
        // pointer alias. Now String is a class with a literal
        // codegen path that materializes a view-mode String over
        // .rodata bytes — one IR call, no per-byte assignment, and
        // the linker dedupes identical keys across all call sites.
        // JsonWriter.key(String) is the matching overload.
        //
        // Escapes the wire key for cajeta source: double-quote and
        // backslash become `\"` / `\\`. Wire keys typically come
        // from field names + a naming strategy transform, so they
        // rarely contain either, but be defensive — a future
        // @JsonProperty("with\"quote") would otherwise emit invalid
        // cajeta source.
        std::string escaped;
        escaped.reserve(wireKey.size() + 2);
        for (size_t i = 0; i < wireKey.size(); ++i) {
            char c = wireKey[i];
            if (c == '"' || c == '\\') escaped.push_back('\\');
            escaped.push_back(c);
        }
        os << "            w.key(\"" << escaped << "\");\n";
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
        std::string strategy = classNamingStrategy(T);
        std::ostringstream os;
        os << "    w.beginObject();\n";
        for (auto& prop : T->getPropertyList()) {
            std::string emit = writeFieldEmit(prop, strategy);
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
