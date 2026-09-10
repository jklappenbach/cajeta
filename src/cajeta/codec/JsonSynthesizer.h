// Tier-1 JSON codegen synthesizer: `Json.parse<T>` and `Json.toBytes<T>` are
// method-level templates whose bodies are synthesized PER-T at instantiation,
// walking T's fields into a key-dispatch parse loop / sequential write chain.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Writes the synthesized body into `out` and returns true when (parent, methodName,
    // paramTypes) names a Tier-1 entry point; otherwise returns false and leaves `out`
    // alone. `paramTypes` excludes `this` and is matched exactly, so overloads survive.
    bool synthesizeJsonMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
