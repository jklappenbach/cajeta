// Tier-1 Avro typed-bind synthesizer: `Avro.parse<T>(int8[], int64)` is synthesized
// per T. Avro is untagged, so decode is POSITIONAL over T's declared fields, and a
// T[] reads an Object Container File. The facade lives in `dev.cajeta.codec.avro`.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Writes the synthesized body into `out`, returning true when (parent, methodName,
    // paramTypes) names Avro.parse(int8[], int64) with args[0] a record T or a T[].
    bool synthesizeAvroMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
