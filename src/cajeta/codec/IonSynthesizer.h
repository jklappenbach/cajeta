// Codec Phase 3.2 — Tier-1 Ion typed-bind synthesizer.
//
// `Ion.parse<T>(int8[], int64)` is a method-level template whose body is
// synthesized PER T at instantiation time. Ion is self-describing — a struct
// carries its field NAMES (symbol IDs resolved against the symbol table) — so
// the synthesizer walks T's declared fields and binds each by NAME (no
// `@ProtoField`-style number). It emits an `IonCursor`-driven bind: look each
// field up by name (`slotOf`), and if present + non-null, decode it via the
// cursor's typed reader (`readInt`/`readBool`/`readString`/`readBytes`).
//
// `MethodTemplateInstantiator` calls `synthesizeIonMethodSource` alongside the
// JSON/CSV/protobuf hooks. The `Ion` facade lives in the standalone
// `dev.cajeta.codec.ion` library, so the emitted body uses fully-qualified
// `dev.cajeta.codec.ion.IonCursor` references.
//
// See docs/specification/codec/Codecs.md § Ion and
// agents/cajeta/codecs/codecs-plan.md § 3.2.

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
    // (parent, methodName, paramTypes) names the Ion typed-bind entry point AND
    // `args[0]` is a class (single struct → T). Otherwise returns false and
    // leaves `out` untouched (caller keeps the captured failsafe body).
    //
    // Recognized entry point (parent must be dev.cajeta.codec.ion.Ion):
    //   - parse(int8[], int64) with args[0] == T (class) : one struct → T
    bool synthesizeIonMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
