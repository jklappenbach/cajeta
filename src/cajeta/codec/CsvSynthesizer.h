// Codec Phase 1.2b — Tier-1 CSV typed-bind synthesizer.
//
// `Csv.parse<T[]>(int8[], int64)` is a method-level template whose body is
// synthesized PER element-T at instantiation time — the compiler walks T's
// declared fields, builds a header→column index map, and emits a two-pass
// row bind over `CsvReader` (pass 1: header + row count; pass 2: fill a
// freshly-allocated `T[]`).
//
// `MethodTemplateInstantiator` calls `synthesizeCsvMethodSource` alongside the
// JSON synthesizer hook; on a recognized entry point the returned string
// replaces the captured (failsafe-throw) method source and the normal
// re-parse path takes it from there.
//
// See docs/specification/codec/Codecs.md § CSV.

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
    // (parent, methodName, paramTypes) names the CSV typed-bind entry point
    // AND `args[0]` is an array type whose element is a class. Otherwise
    // returns false and leaves `out` untouched (caller keeps the captured
    // failsafe body).
    //
    // Recognized entry point (parent must be cajeta.codec.csv.Csv):
    //   - parse(int8[], int64) with args[0] == T[]  : whole-file row bind → T[]
    bool synthesizeCsvMethodSource(
        const CajetaClassPtr& parent,
        const std::string& methodName,
        const std::vector<CajetaTypePtr>& args,
        const std::vector<CajetaTypePtr>& paramTypes,
        std::string& out);

} // namespace cajeta
