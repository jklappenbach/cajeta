// Phase 4b — Tier 1 JSON codegen synthesizer.
//
// `Json.parse<T>(int8[], int64)` and `Json.toBytes<T>(T)` are method-level
// templates whose bodies are synthesized PER-T at instantiation time —
// the compiler walks T's declared fields and emits a key-dispatch
// parse loop / sequential write chain.
//
// `MethodTemplateInstantiator` calls into `synthesizeJsonMethodSource`
// BEFORE its normal re-parse path; if the method is one of the JSON
// synthesizer entry points, the returned string replaces the captured
// methodSource and the existing parse pipeline takes it from there.
//
// See cajeta-docs/stdlib/codec/Json.md § Tier 1.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Returns true and writes the synthesized method body into `out` if
    // (parent, methodName, paramTypes) names a Tier-1 synthesizer entry
    // point AND `args` provides enough info to walk T. Otherwise returns
    // false and leaves `out` untouched — caller proceeds with the normal
    // template re-parse path (the captured method source becomes the
    // body, useful for overloads that wrap a synthesized variant).
    //
    // Recognized entry points (parent must be cajeta.codec.json.Json):
    //   - parse(int8[], int64) : full byte-buffer parse
    //   - parseObjectFromReader(JsonReader) : recursive nested helper
    //   - toBytes(T)                        : full T → byte writer
    //   - toBytesObjectInto(JsonWriter, T)  : recursive nested helper
    //
    // `paramTypes` excludes the implicit `this` and is matched
    // canonically so overloads (e.g. `parse<T>(String)` which delegates
    // to `parse<T>(int8[], int64)`) don't unintentionally trip the
    // synthesizer and overwrite their hand-written delegation bodies.
    bool synthesizeJsonMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
