// Codec Phase 5.1c — Tier-1 Avro typed-bind synthesizer.
//
// `Avro.parse<T>(int8[], int64)` is a method-level template synthesized per T.
// Avro is schema-driven and untagged, so decode is POSITIONAL: the synthesizer
// walks T's declared fields in order and emits an `AvroCursor`-driven read by
// Avro type (zigzag long/int, boolean, length-prefixed string/bytes; nested
// records recurse inline). For T[] it reads an Object Container File via
// `AvroContainer` (header + blocks). No annotations.
//
// `MethodTemplateInstantiator` calls `synthesizeAvroMethodSource` alongside the
// JSON/CSV/protobuf/Ion hooks. The facade lives in `dev.cajeta.codec.avro`.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Returns true and writes the synthesized body into `out` if
    // (parent, methodName, paramTypes) names the Avro typed-bind entry point.
    // parent must be dev.cajeta.codec.avro.Avro; parse(int8[], int64) with
    // args[0] == T (record → single datum) or T[] (container → array).
    bool synthesizeAvroMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
