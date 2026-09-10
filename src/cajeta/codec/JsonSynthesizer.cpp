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

    // True when the field is skipped on the READ side. A bare @JsonIgnore
    // skips both directions; once any arg is given, unspecified directions
    // default to false.
    bool isJsonIgnoredOnRead(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ann = prop->findAnnotation("JsonIgnore");
        if (!ann) return false;
        if (ann->getArgs().empty()) return true;
        return ann->getBool("onRead", false);
    }

    // True when the field is skipped on the WRITE side, under the same
    // bare-versus-argument rule as the read side.
    bool isJsonIgnoredOnWrite(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ann = prop->findAnnotation("JsonIgnore");
        if (!ann) return false;
        if (ann->getArgs().empty()) return true;
        return ann->getBool("onWrite", false);
    }

    // True only when BOTH directions are skipped — a partially ignored field
    // still has a meaningful read direction.
    bool isJsonIgnored(const StructurePropertyPtr& prop) {
        return isJsonIgnoredOnRead(prop) && isJsonIgnoredOnWrite(prop);
    }

    // True when the field's static type is `cajeta.lang.Optional<T>`. Those
    // fields take an arm that separates key-absent from a null value and from
    // a present value; inner T must be a primitive or String in v1.
    bool isOptionalField(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ty = prop->getType();
        if (!ty) return false;
        auto cls = std::dynamic_pointer_cast<CajetaClass>(ty);
        if (!cls || !cls->getQName()) return false;
        // Instantiated classes carry the template-arg suffix ("Optional<int32>"),
        // so match on the bare-name prefix.
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

    // True when @JsonRaw is set: the field's wire bytes pass through both
    // directions unchanged, so its static type must be int8[] and the bytes
    // carry their own quotes and delimiters.
    bool isJsonRaw(const StructurePropertyPtr& prop) {
        return prop && prop->findAnnotation("JsonRaw") != nullptr;
    }

    // True when @JsonRequired is set on a read-visible field; the parse body
    // throws if no key arm fired for it.
    bool isJsonRequired(const StructurePropertyPtr& prop) {
        return prop
            && !isJsonIgnoredOnRead(prop)
            && prop->findAnnotation("JsonRequired") != nullptr;
    }

    // Wire key derived from a declared field name under a class-level naming
    // strategy: SNAKE_CASE, KEBAB_CASE and PASCAL_CASE transform; CAMEL_CASE,
    // IDENTITY and the empty strategy are no-ops.
    std::string applyNamingStrategy(const std::string& strategy,
                                     const std::string& declaredName) {
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
        return declaredName;
    }

    // Effective wire key for the field: a non-empty @JsonProperty value wins,
    // otherwise the class-level strategy applied to the declared name.
    std::string effectiveJsonKey(const StructurePropertyPtr& prop,
                                  const std::string& classStrategy) {
        if (!prop) return std::string();
        if (auto ann = prop->findAnnotation("JsonProperty")) {
            std::string renamed = ann->getString("value");
            if (!renamed.empty()) return renamed;
        }
        return applyNamingStrategy(classStrategy, prop->getName());
    }

    // Class-level strategy from @JsonNamingStrategy("..."), or empty when the
    // annotation is absent.
    std::string classNamingStrategy(const CajetaClassPtr& T) {
        if (!T) return std::string();
        if (auto ann = T->findAnnotation("JsonNamingStrategy")) {
            return ann->getString("value");
        }
        return std::string();
    }

    // True when class-level @JsonStrict is set: the read side rejects unknown
    // keys instead of skipping their values.
    bool classIsStrict(const CajetaClassPtr& T) {
        return T && T->findAnnotation("JsonStrict") != nullptr;
    }

    // The @JsonInclude policy verbatim — "ALWAYS", "NON_NULL", "NEVER" or
    // "NON_DEFAULT" — or empty when the annotation is absent.
    std::string jsonIncludePolicy(const StructurePropertyPtr& prop) {
        if (!prop) return std::string();
        if (auto ann = prop->findAnnotation("JsonInclude")) {
            return ann->getString("value");
        }
        return std::string();
    }

    // True when the field's slot can hold null (class- or array-typed); the
    // @JsonInclude(NON_NULL) gate is a no-op on primitives.
    bool fieldIsReferenceTyped(const StructurePropertyPtr& prop) {
        if (!prop) return false;
        auto ty = prop->getType();
        if (!ty || !ty->getQName()) return false;
        if (std::dynamic_pointer_cast<CajetaArray>(ty)) return true;
        if (std::dynamic_pointer_cast<CajetaClass>(ty)) return true;
        return false;
    }

    // Read-side alternate keys from @JsonAlias, in the order declared. The
    // write side always uses the primary key.
    std::vector<std::string> jsonAliases(const StructurePropertyPtr& prop) {
        std::vector<std::string> out;
        if (!prop) return out;
        if (auto ann = prop->findAnnotation("JsonAlias")) {
            const auto& list = ann->getStringList("value");
            for (auto& s : list) out.push_back(s);
            if (out.empty()) {
                std::string single = ann->getString("value");
                if (!single.empty()) out.push_back(single);
            }
        }
        return out;
    }

    // If-condition source matching the current KEY's raw bytes against `key`,
    // assuming `int8[] kb` and `int32 klen` are in scope.
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

    // Escape a wire key for embedding in cajeta source: `"` and `\` become
    // `\"` and `\\`.
    std::string escapeCajetaString(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    // Index-form key guard: a JsonIndex.keyEq call comparing the key at
    // `keyPos` against the literal `key`.
    std::string keyEqGuard(const std::string& key) {
        return "JsonIndex.keyEq(b, keyPos, \"" + escapeCajetaString(key) + "\")";
    }

    // Statements appending one array element to the `tmp_<fieldName>` accumulator;
    // empty for class elements and for unsupported element types.
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
            // `#sb_` transfers the buffer into the String — without it the local's
            // drop frees what the new String points at.
            return "int8[] sb_" + fieldName + " = r.currentBytes();\n"
                   "                    int32 sl_" + fieldName +
                   " = (int32) sb_" + fieldName + ".count();\n"
                   "                    " + list + ".add(heap cajeta.lang.String("
                   "#sb_" + fieldName +
                   ", sl_" + fieldName + "));\n";
        }
        if (elementIsClass) {
            // Class elements need the START_OBJECT left unconsumed; empty here tells
            // readArrayField to use its class-element loop.
            return "";
        }
        return "";
    }

    // Value-side body for an array field: accumulate into an ArrayList<E> from
    // the value's first token through END_ARRAY, then fill a fresh E[] and
    // assign it to out.<fieldName>.
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
            // Peek rather than next(): parseObjectFromReader<E> consumes the
            // START_OBJECT itself. The result must land in a temp local —
            // `tmp.add(Json.parse...)` inline drops the add() call.
            os << "                JsonToken pt_" << fieldName
               << " = r.peek();\n";
            os << "                if (pt_" << fieldName
               << " == JsonToken.END_ARRAY) {\n";
            os << "                    r.next();\n"
               << "                    done_" << fieldName << " = true;\n";
            os << "                } else {\n";
            // `#` transfers the element into the list; otherwise the temp's drop frees
            // the object out.<field>[i] points at.
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
        // Assign the fresh array DIRECTLY into out.<field>: an intermediate local's
        // drop claims the buffer out of the live set before the class's field-drop
        // walk, and the caller then reads freed memory.
        os << "            int32 sz_" << fieldName << " = tmp_"
           << fieldName << ".count();\n";
        os << "            out." << fieldName << " #= heap " << etcanon
           << "[sz_" << fieldName << "];\n";
        os << "            int32 ii_" << fieldName << " = 0;\n";
        os << "            while (ii_" << fieldName << " < sz_"
           << fieldName << ") {\n";
        // Class elements were add(#elem)'d, so the list slots own them — `#tmp[i]`
        // takes the title; primitives and Strings keep the plain get.
        if (elementIsClass) {
            os << "                out." << fieldName << "[ii_"
               << fieldName << "] = #tmp_" << fieldName
               << "[ii_" << fieldName << "];\n";
        } else {
            os << "                out." << fieldName << "[ii_"
               << fieldName << "] = tmp_" << fieldName
               << ".get(ii_" << fieldName << ");\n";
        }
        os << "                ii_" << fieldName << " = ii_"
           << fieldName << " + 1;\n";
        os << "            }\n";
        return os.str();
    }

    // Full post-key body for one field: the statements that consume the value
    // tokens and assign into out.<field>. Empty when the field type is
    // unsupported, leaving the caller to emit a skip-value arm.
    std::string readFieldAssignment(const StructurePropertyPtr& prop) {
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        if (isJsonRaw(prop)) {
            return "t = r.next();\n            out." + fieldName +
                   " #= r.currentRawBytes();\n";
        }
        // Optional<T>: a present-but-null value becomes an empty Optional, while
        // key-absent is handled by this arm never firing.
        if (isOptionalField(prop)) {
            auto inner = optionalInnerType(prop);
            if (!inner || !inner->getQName()) return "";
            const std::string& innerCanon = inner->getQName()->toCanonical();
            std::string readInner;
            std::string defaultInner;
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
                // `#vb` transfers the buffer into the String.
                readInner =
                    "int8[] vb = r.currentBytes();\n"
                    "                int32 vl = (int32) vb.count();\n"
                    "                cajeta.lang.String v = heap cajeta.lang.String("
                    "#vb, vl);";
                // Empty Optional<String> defaults to null rather than a zero-length
                // String allocation.
                defaultInner = "null";
            } else {
                return "";
            }
            // `#v` moves the String into the Optional so the local's drop can't
            // reclaim it; primitive inner types are value-copied.
            bool innerIsRef = (innerCanon == "cajeta.lang.String");
            std::string presentArg = innerIsRef ? "#v" : "v";
            std::ostringstream os;
            os << "t = r.next();\n"
               << "            if (t == JsonToken.NULL) {\n"
               << "                out." << fieldName
               << " #= heap cajeta.lang.Optional<" << innerCanon
               << ">(false, " << defaultInner << ");\n"
               << "            } else {\n"
               << "                " << readInner << "\n"
               << "                out." << fieldName
               << " #= heap cajeta.lang.Optional<" << innerCanon
               << ">(true, " << presentArg << ");\n"
               << "            }\n";
            return os.str();
        }
        // CajetaArray inherits CajetaClass, so array fields must be matched before
        // the catch-all nested-object branch below.
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
            // `#` transfers the reader's owned bytes into the String — otherwise scope
            // exit frees the buffer it now points at.
            return "t = r.next();\n"
                   "            int8[] vbytes_" + fieldName +
                       " = r.currentBytes();\n"
                   "            int32 vlen_" + fieldName +
                       " = (int32) vbytes_" + fieldName + ".count();\n"
                   "            out." + fieldName +
                       " #= heap cajeta.lang.String("
                       "#vbytes_" + fieldName +
                       ", vlen_" + fieldName + ");\n";
        }
        // The recursive call consumes the value's START_OBJECT and END_OBJECT itself.
        // Short name `Json`: a multi-segment FQN doesn't resolve through the dot chain.
        if (auto nestedClass = std::dynamic_pointer_cast<CajetaClass>(ty)) {
            return "out." + fieldName +
                   " #= Json.parseObjectFromReader<" +
                   tcanon + ">(r);\n";
        }
        return "";
    }

    // Inner field-dispatch loop body, shared by `parse` and
    // `parseObjectFromReader`. Expects locals `T out` and `JsonReader r` in
    // scope, and consumes from START_OBJECT through END_OBJECT.
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
            // A read-side @JsonIgnore drops the arm entirely, so the key falls through
            // to the unknown-key skip.
            if (isJsonIgnoredOnRead(prop)) continue;
            std::string assign = readFieldAssignment(prop);
            if (assign.empty()) continue;
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
        // Unknown keys are skipped whole — scalar or subtree — via
        // JsonReader.skipValue(), or rejected when the class is @JsonStrict.
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

    // ===== Index-form (Tier-1 binding) read path =====
    // Scope at a matched key arm: `int8[] b`, `int32[] idx`, `int32 ci` at the
    // value's first entry, `T out`, `int64 keyPos`; ci exits at the separator.

    // Array field over the index: `ci` enters at the `[` entry and exits at the
    // separator past the `]`; ownership as in readArrayField.
    std::string readArrayFieldIndexed(const std::string& fieldName,
                                       CajetaTypePtr elementType) {
        if (!elementType || !elementType->getQName()) return "";
        const std::string& et = elementType->getQName()->toCanonical();
        bool elementIsClass =
            std::dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        const std::string f = fieldName;
        const std::string list = "tmp_" + f;
        std::string elem;
        if (et == "int32") {
            elem = list + ".add(JsonIndex.decodeI32(b, (int64) ep_" + f + ")); ci = ci + 1;\n";
        } else if (et == "int64") {
            elem = list + ".add(JsonIndex.decodeI64(b, (int64) ep_" + f + ")); ci = ci + 1;\n";
        } else if (et == "boolean") {
            elem = list + ".add(JsonIndex.decodeBool(b, (int64) ep_" + f + ")); ci = ci + 1;\n";
        } else if (et == "float64") {
            elem = list + ".add(JsonIndex.decodeF64(b, (int64) ep_" + f + ")); ci = ci + 1;\n";
        } else if (et == "cajeta.lang.String") {
            elem = "int8[] esb_" + f + " #= JsonIndex.decodeStrBytes(b, (int64) ep_" + f + ");\n"
                   "                    int32 esl_" + f + " = (int32) esb_" + f + ".count();\n"
                   "                    " + list + ".add(heap cajeta.lang.String(#esb_" + f + ", esl_" + f + ")); ci = ci + 1;\n";
        } else if (elementIsClass) {
            // Recursive call into a local, then add(#local): walkValue takes a single
            // class pointer, never array-typed params.
            elem = "jc.ci = ci;\n"
                   "                    " + et + " e_" + f + " = Json.walkElement<" + et + ">(jc);\n"
                   "                    ci = jc.ci;\n"
                   "                    " + list + ".add(#e_" + f + ");\n";
        } else {
            return "";
        }
        std::ostringstream os;
        os << "int32 ap_" << f << " = idx[ci];\n";
        os << "            int8 ac_" << f << " = b[ap_" << f << "];\n";
        os << "            if (ac_" << f << " != (int8) 91) {\n";
        os << "                throw heap JsonParseException(\n";
        os << "                    \"Tier-1 parse: expected '['\", (int64) ap_" << f << ");\n";
        os << "            }\n";
        os << "            ci = ci + 1;\n";
        os << "            cajeta.collection.ArrayList<" << et << "> " << list
           << " = heap cajeta.collection.ArrayList<" << et << ">();\n";
        os << "            boolean adone_" << f << " = false;\n";
        os << "            while (!adone_" << f << ") {\n";
        os << "                int32 ep_" << f << " = idx[ci];\n";
        os << "                int8 ec_" << f << " = b[ep_" << f << "];\n";
        os << "                if (ec_" << f << " == (int8) 93) { ci = ci + 1; adone_" << f << " = true; }\n";
        os << "                else {\n";
        os << "                    " << elem;
        os << "                    int32 es_" << f << " = idx[ci];\n";
        os << "                    int8 esc_" << f << " = b[es_" << f << "];\n";
        os << "                    if (esc_" << f << " == (int8) 44) { ci = ci + 1; }\n";
        os << "                }\n";
        os << "            }\n";
        os << "            int32 sz_" << f << " = " << list << ".count();\n";
        os << "            out." << f << " = heap " << et << "[sz_" << f << "];\n";
        os << "            int32 ii_" << f << " = 0;\n";
        os << "            while (ii_" << f << " < sz_" << f << ") {\n";
        // Same ownership rule as the reader path above: class elements are
        // owned by the list slots (add(#e)) — extract the title out.
        if (elementIsClass) {
            os << "                out." << f << "[ii_" << f << "] = #" << list << "[ii_" << f << "];\n";
        } else {
            os << "                out." << f << "[ii_" << f << "] = " << list << ".get(ii_" << f << ");\n";
        }
        os << "                ii_" << f << " = ii_" << f << " + 1;\n";
        os << "            }\n";
        return os.str();
    }

    // Index-form value decode for one field. Returns the matched-arm body.
    std::string readFieldAssignmentIndexed(const StructurePropertyPtr& prop) {
        const std::string& f = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        if (isJsonRaw(prop)) {
            return "out." + f + " #= JsonIndex.valueWireBytes(b, idx, ci);\n"
                   "            ci = JsonIndex.skipValue(b, idx, ci);\n";
        }
        if (isOptionalField(prop)) {
            auto inner = optionalInnerType(prop);
            if (!inner || !inner->getQName()) return "";
            const std::string& ic = inner->getQName()->toCanonical();
            std::string readInner;
            std::string defaultInner;
            bool innerIsRef = false;
            if (ic == "int32") {
                readInner = "int32 v = JsonIndex.decodeI32(b, (int64) vp_" + f + ");";
                defaultInner = "(int32) 0";
            } else if (ic == "int64") {
                readInner = "int64 v = JsonIndex.decodeI64(b, (int64) vp_" + f + ");";
                defaultInner = "(int64) 0";
            } else if (ic == "boolean") {
                readInner = "boolean v = JsonIndex.decodeBool(b, (int64) vp_" + f + ");";
                defaultInner = "false";
            } else if (ic == "float64") {
                readInner = "float64 v = JsonIndex.decodeF64(b, (int64) vp_" + f + ");";
                defaultInner = "(float64) 0.0";
            } else if (ic == "cajeta.lang.String") {
                readInner = "int8[] vb_" + f + " #= JsonIndex.decodeStrBytes(b, (int64) vp_" + f + ");\n"
                            "                int32 vl_" + f + " = (int32) vb_" + f + ".count();\n"
                            "                cajeta.lang.String v = heap cajeta.lang.String(#vb_" + f + ", vl_" + f + ");";
                defaultInner = "null";
                innerIsRef = true;
            } else {
                return "";
            }
            std::string presentArg = innerIsRef ? "#v" : "v";
            std::ostringstream os;
            os << "int32 vp_" << f << " = idx[ci];\n";
            os << "            int8 ob_" << f << " = b[vp_" << f << "];\n";
            os << "            if (ob_" << f << " == (int8) 110) {\n";
            os << "                out." << f << " = heap cajeta.lang.Optional<" << ic
               << ">(false, " << defaultInner << ");\n";
            os << "            } else {\n";
            os << "                " << readInner << "\n";
            os << "                out." << f << " = heap cajeta.lang.Optional<" << ic
               << ">(true, " << presentArg << ");\n";
            os << "            }\n";
            os << "            ci = ci + 1;\n";
            return os.str();
        }
        // Array fields (CajetaArray inherits CajetaClass — check first).
        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(ty)) {
            return readArrayFieldIndexed(f, arr->getElementType());
        }
        const std::string& tc = ty->getQName()->toCanonical();
        if (tc == "int32") {
            return "int32 vp_" + f + " = idx[ci];\n"
                   "            out." + f + " = JsonIndex.decodeI32(b, (int64) vp_" + f + ");\n"
                   "            ci = ci + 1;\n";
        }
        if (tc == "int64") {
            return "int32 vp_" + f + " = idx[ci];\n"
                   "            out." + f + " = JsonIndex.decodeI64(b, (int64) vp_" + f + ");\n"
                   "            ci = ci + 1;\n";
        }
        if (tc == "boolean") {
            return "int32 vp_" + f + " = idx[ci];\n"
                   "            out." + f + " = JsonIndex.decodeBool(b, (int64) vp_" + f + ");\n"
                   "            ci = ci + 1;\n";
        }
        if (tc == "float64") {
            return "int32 vp_" + f + " = idx[ci];\n"
                   "            out." + f + " = JsonIndex.decodeF64(b, (int64) vp_" + f + ");\n"
                   "            ci = ci + 1;\n";
        }
        if (tc == "cajeta.lang.String") {
            return "int32 vp_" + f + " = idx[ci];\n"
                   "            int8[] sb_" + f + " #= JsonIndex.decodeStrBytes(b, (int64) vp_" + f + ");\n"
                   "            int32 sl_" + f + " = (int32) sb_" + f + ".count();\n"
                   "            out." + f + " #= heap cajeta.lang.String(#sb_" + f + ", sl_" + f + ");\n"
                   "            ci = ci + 1;\n";
        }
        // Nested class — recurse via walkValue(jc) (single class-ptr arg,
        // #-returning). Sync the cursor through jc around the call.
        if (auto nested = std::dynamic_pointer_cast<CajetaClass>(ty)) {
            return "jc.ci = ci;\n"
                   "            out." + f + " #= Json.walkValue<" + tc + ">(jc);\n"
                   "            ci = jc.ci;\n";
        }
        return "";
    }

    // Build the index-walk object body — the stage-2 driver. Expects
    // locals `int8[] b`, `int32[] idx`, `int32 ci`, `T out` in scope; ci at
    // the object's `{` index entry on entry. Returns the cursor past `}`.
    std::string emitObjectWalkBody(const CajetaClassPtr& T,
                                   const std::string& indent) {
        std::string strategy = classNamingStrategy(T);
        bool strict = classIsStrict(T);
        std::ostringstream os;
        os << indent << "int32 op = idx[ci];\n";
        os << indent << "int8 oc = b[op];\n";
        os << indent << "if (oc != (int8) 123) {\n";
        os << indent << "    throw heap JsonParseException(\n";
        os << indent << "        \"Tier-1 parse: expected '{'\", (int64) op);\n";
        os << indent << "}\n";
        os << indent << "ci = ci + 1;\n";
        std::vector<std::string> requiredKeys;
        for (auto& prop : T->getPropertyList()) {
            if (isJsonRequired(prop)) {
                os << indent << "boolean req_seen_" << prop->getName()
                   << " = false;\n";
            }
        }
        os << indent << "boolean go = true;\n";
        os << indent << "while (go) {\n";
        os << indent << "    int32 kp = idx[ci];\n";
        os << indent << "    int8 kc = b[kp];\n";
        os << indent << "    if (kc == (int8) 125) {\n";
        for (auto& prop : T->getPropertyList()) {
            if (!isJsonRequired(prop)) continue;
            std::string key = effectiveJsonKey(prop, strategy);
            os << indent << "        if (!req_seen_" << prop->getName() << ") {\n";
            os << indent << "            throw heap JsonParseException(\n";
            os << indent << "                \"Tier-1 parse: required field '"
               << escapeCajetaString(key) << "' missing\", (int64) kp);\n";
            os << indent << "        }\n";
        }
        os << indent << "        ci = ci + 1;\n";
        os << indent << "        go = false;\n";
        os << indent << "    } else {\n";
        os << indent << "        int64 keyPos = (int64) kp;\n";
        os << indent << "        ci = ci + 1;\n";
        os << indent << "        ci = ci + 1;\n";
        bool first = true;
        for (auto& prop : T->getPropertyList()) {
            if (isJsonIgnoredOnRead(prop)) continue;
            std::string assign = readFieldAssignmentIndexed(prop);
            if (assign.empty()) continue;
            std::string primary = effectiveJsonKey(prop, strategy);
            std::ostringstream guard;
            guard << "(" << keyEqGuard(primary) << ")";
            for (auto& alias : jsonAliases(prop)) {
                guard << " || (" << keyEqGuard(alias) << ")";
            }
            os << indent << "        " << (first ? "if" : "else if")
               << " (" << guard.str() << ") {\n";
            os << indent << "            " << assign;
            if (isJsonRequired(prop)) {
                os << indent << "            req_seen_" << prop->getName()
                   << " = true;\n";
            }
            os << indent << "        }\n";
            first = false;
        }
        std::string unknownArmBody;
        if (strict) {
            unknownArmBody = "throw heap JsonParseException("
                "\"Tier-1 parse: unknown key (class is @JsonStrict)\", keyPos);";
        } else {
            unknownArmBody = "ci = JsonIndex.skipValue(b, idx, ci);";
        }
        if (first) {
            os << indent << "        { " << unknownArmBody << " }\n";
        } else {
            os << indent << "        else { " << unknownArmBody << " }\n";
        }
        // After a value, ci sits at the separator (',' or '}').
        os << indent << "        int32 spx = idx[ci];\n";
        os << indent << "        int8 scx = b[spx];\n";
        os << indent << "        if (scx == (int8) 44) { ci = ci + 1; }\n";
        os << indent << "    }\n";
        os << indent << "}\n";
        return os.str();
    }

    // Shared walk core: allocate T, lift b/idx/ci out of `jc` into stack locals
    // for the per-key loop, run the walk, sync the cursor back and return the
    // filled #T. Used by walkValue<T> and inlined by parse<T>.
    std::string emitWalkCore(const CajetaClassPtr& T) {
        const std::string& Tc = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "    " << Tc << " out #= heap " << Tc << "();\n";
        os << "    int8[] b = jc.b;\n";
        os << "    int32[] idx = jc.idx;\n";
        os << "    int32 ci = jc.ci;\n";
        os << emitObjectWalkBody(T, "    ");
        os << "    jc.ci = ci;\n";
        os << "    return out;\n";
        return os.str();
    }

    // Synthesize `walkValue<T>(JsonCursor jc)`, the recursive walk for nested
    // objects. It recurses on a single class pointer: threading array-typed
    // params through a recursive loop produced IR that crashed the backend.
    std::string synthesizeWalkValueBody(const CajetaClassPtr& T,
                                         const std::string& methodName) {
        const std::string& Tc = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Tc << " " << methodName
           << "(JsonCursor jc) {\n";
        os << emitWalkCore(T);
        os << "}\n";
        return os.str();
    }

    // Body for the synthesized `parse` method on T. The signature carries a
    // vacuous <__T> type parameter that never appears in the body — the
    // template-instantiation machinery expects one on the declaration.
    std::string synthesizeParseBody(const CajetaClassPtr& T,
                                     const std::string& methodName) {
        // Type names are fully qualified: the wrapper class lives in
        // cajeta.codec.json. The return must be `#T` — a plain multi-param return
        // reads as "borrow from one of two parameters" and is rejected.
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Tcanon
           << " " << methodName << "(int8[] bytes, int64 length) {\n";
        // `#=`, not `=`: jc is a fresh allocation owning its index scratch, and a
        // borrow bind abandoned both on every parse.
        os << "    JsonCursor jc #= heap JsonCursor(bytes, length);\n";
        os << "    int32 cnt = JsonIndex.build(bytes, length, jc.idx);\n";
        // Stage 2 inlines the walk core rather than calling walkValue<T>: the extra
        // template layer deepened the recursive-instantiation chain enough to leak
        // nested drops into this frame.
        os << emitWalkCore(T);
        os << "}\n";
        return os.str();
    }

    // Variant of synthesizeParseBody whose signature takes a JsonReader
    // instead of bytes/length; same field-dispatch loop body.
    std::string synthesizeParseFromReaderBody(const CajetaClassPtr& T,
                                                const std::string& methodName) {
        const std::string& Tcanon = T->getQName()->toCanonical();
        std::ostringstream os;
        os << "public static #" << Tcanon
           << " " << methodName << "(JsonReader r) {\n";
        os << "    " << Tcanon << " out #= heap " << Tcanon << "();\n";
        os << emitObjectLoopBody(T, "    ");
        os << "}\n";
        return os.str();
    }

    // Value-side body for an array field: begin-array, one writer call per
    // element, end-array. v1 assumes the field reference is non-null — a null
    // array trips the count() deref at runtime.
    std::string writeArrayValue(const std::string& fieldName,
                                 CajetaTypePtr elementType) {
        if (!elementType || !elementType->getQName()) return "";
        const std::string& etcanon = elementType->getQName()->toCanonical();
        bool elementIsClass =
            std::dynamic_pointer_cast<CajetaClass>(elementType) != nullptr;
        // Load the element into a typed local first: handing `value.f[wi]` straight
        // to the call passes the GEP slot pointer, which the arg-coerce path misses
        // and the JIT verifier then rejects.
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
                       "w.writeString(" + elemVar + ");";
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
        if (isJsonIgnoredOnWrite(prop)) return "";
        const std::string& fieldName = prop->getName();
        CajetaTypePtr ty = prop->getType();
        if (!ty || !ty->getQName()) return "";
        // Arrays first: CajetaArray inherits CajetaClass.
        std::ostringstream value;
        if (isJsonRaw(prop)) {
            value << "w.writeRaw(value." << fieldName
                  << ", (int32) value." << fieldName << ".count());\n";
        } else
        // guardExpr below handles the field-is-null case; inside the guard, empty
        // versus present chooses writeNull over the inner write.
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
                    "            w.writeString(os);";
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
                // String overload: view-safe, unlike a raw .bytes read on a mode-2 field.
                value << "w.writeString(value." << fieldName << ");\n";
            } else if (std::dynamic_pointer_cast<CajetaClass>(ty)) {
                // Nested class: short name `Json`, same-package resolution as the read side.
                value << "Json.toBytesObjectInto<"
                      << tcanon << ">(w, value." << fieldName << ");\n";
            } else {
                return "";
            }
        }
        std::string includePolicy = jsonIncludePolicy(prop);
        if (includePolicy == "NEVER") {
            return "";
        }
        std::string wireKey = effectiveJsonKey(prop, classStrategy);
        std::ostringstream os;
        // @JsonInclude guard: NON_NULL emits `f != null`; NON_DEFAULT compares
        // against the type default (null for refs, 0 / 0.0 / false for primitives).
        std::string guardExpr;
        auto fieldTyName = prop->getType() && prop->getType()->getQName()
            ? prop->getType()->getQName()->toCanonical()
            : "";
        // A null Optional reference means "absent", so the key is auto-guarded on
        // `f != null` unless the user explicitly asked for ALWAYS.
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
                guardExpr = "value." + fieldName + " != 0";
            }
        }
        if (!guardExpr.empty()) {
            os << "        if (" << guardExpr << ") {\n";
        } else {
            os << "        {\n";
        }
        // Keys emit as string literals through JsonWriter.key(String); escape `"`
        // and `\` so a @JsonProperty("with\"quote") still yields valid source.
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

    // Emit the `w.beginObject() ... per-field ... w.endObject()` sequence,
    // shared by `toBytes` (which wraps it in writer create + finalize) and
    // `toBytesObjectInto` (which is handed the parent's writer).
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

    // Body for the synthesized `toBytes` method on T: create a JsonWriter,
    // write the object, return its bytes.
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

    // toBytesObjectInto variant: the caller supplies the JsonWriter, so this
    // emits only the begin/end-object pair around the field writes.
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
        // Match on name AND param types: without the param check the synthesizer
        // would overwrite the hand-written bodies of same-named overloads.
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
        if (methodName == "walkValue"
                && paramTypes.size() == 1
                && paramCanonAt(paramTypes, 0) == "cajeta.codec.json.JsonCursor") {
            out = synthesizeWalkValueBody(T, methodName);
            dumpIfRequested(out);
            return true;
        }
        // The captured source carries the unsubstituted placeholder "T", so accept
        // either that or the concrete substituted canonical.
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
