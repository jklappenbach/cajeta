// Codec Phase 2.3 — Tier-1 protobuf typed-bind synthesizer.
//
// `Protobuf.parse<T>(int8[], int64)` is a method-level template whose body is
// synthesized PER T at instantiation time — the compiler walks T's `@ProtoField`
// fields, infers each field's wire type from its Cajeta type, and emits a
// `ProtobufCursor`-driven bind (look each field up by its explicit protobuf
// number, decode by wire type, set it on a freshly-allocated `T`).
//
// `MethodTemplateInstantiator` calls `synthesizeProtobufMethodSource` alongside
// the JSON/CSV hooks; on a recognized entry point the returned string replaces
// the captured (failsafe-throw) method source and the normal re-parse path takes
// it from there. Unlike CSV/JSON, the `Protobuf` facade lives in the standalone
// `dev.cajeta.codec.protobuf` library (not core stdlib), so the emitted body uses
// fully-qualified `dev.cajeta.codec.protobuf.ProtobufCursor` references.
//
// See docs/specification/codec/Codecs.md § Protobuf and
// agents/cajeta/codecs/codecs-plan.md § 2.3.

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
    // (parent, methodName, paramTypes) names the protobuf typed-bind entry point
    // AND `args[0]` is a class (single message → T). Otherwise returns false and
    // leaves `out` untouched (caller keeps the captured failsafe body).
    //
    // Recognized entry point (parent must be dev.cajeta.codec.protobuf.Protobuf):
    //   - parse(int8[], int64) with args[0] == T (class) : one message → T
    bool synthesizeProtobufMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
