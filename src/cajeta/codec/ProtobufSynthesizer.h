// Tier-1 protobuf typed-bind synthesizer: `Protobuf.parse<T>` is a template whose
// body is synthesized PER T, binding each `@ProtoField` by its explicit number.
// The facade is a standalone library, so ProtobufCursor is spelled fully-qualified.

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
    // the protobuf typed-bind entry point, `parse(int8[], int64)` over a class T;
    // otherwise returns false and leaves `out` untouched.
    bool synthesizeProtobufMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
