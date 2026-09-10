// Tier-1 Ion typed-bind synthesizer: `Ion.parse<T>` is a template whose body is
// synthesized PER T. Ion is self-describing, so fields bind by NAME. The facade
// is a standalone library, so the emitted body spells IonCursor fully-qualified.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaClass;
    class CajetaType;
    using CajetaClassPtr = std::shared_ptr<CajetaClass>;
    using CajetaTypePtr  = std::shared_ptr<CajetaType>;

    // Writes the synthesized body into `out` and returns true when the call names
    // the Ion typed-bind entry point, `parse(int8[], int64)` over a class T;
    // otherwise returns false and leaves `out` untouched.
    bool synthesizeIonMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
