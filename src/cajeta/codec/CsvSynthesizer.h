// Tier-1 CSV typed-bind synthesizer: `Csv.parse<T[]>` bodies are synthesized
// per element-T at instantiation time and handed back to the normal re-parse
// path. See docs/specification/codec/Codecs.md § CSV.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Writes the synthesized body for the CSV typed-bind entry point into `out`
    // and returns true when (parent, methodName, paramTypes) names
    // cajeta.codec.csv.Csv.parse(int8[], int64) with args[0] a T[] of a class.
    bool synthesizeCsvMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
