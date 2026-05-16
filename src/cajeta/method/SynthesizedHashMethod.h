//
// Compiler-synthesized `int64 hash()` for classes annotated `@AutoHash`.
// Sister to DefaultConstructorMethod — subclasses Method and overrides
// generateCode() to emit IR directly rather than walking a parsed
// BlockPtr.
//
// Hash construction:
//   - Initial accumulator = cajeta.hash.Hash.processSeed() (the
//     per-process random seed, mixed into every field hash for
//     hash-flooding defense)
//   - For each inherited and own field, hash via the matching
//     __cajeta_hash_X primitive (primitives) or virtually dispatch
//     to field.hash() (class types, with null guard for uninitialized
//     embedded vtables)
//   - Combine each field hash into the accumulator via
//     __cajeta_hash_combine
//   - Return the final accumulator
//
// Field-kind coverage in v1:
//   - boolean / int8..int64 / uint8..uint64 / float32 / float64 → OK
//   - Class types (non-struct, non-array) → OK via vtable virtual call
//   - Struct types → COMPILE ERROR (recursive struct walk not yet
//                    implemented). User must remove @AutoHash or
//                    skip the struct field with @transient (also
//                    not yet implemented — current advice: don't).
//   - Array types → COMPILE ERROR (element walk not yet implemented).
//   - String / pointer / extended-precision floats → COMPILE ERROR.
//
// Error attribution: every diagnostic thrown by this synthesizer
// names (a) the class carrying @AutoHash, (b) the offending field
// name, (c) the field's declared type, and (d) a concrete
// remediation. Errors point at the user's annotation, not the
// compiler internals.
//

#pragma once

#include "Method.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedHashMethod : public Method {
    public:
        SynthesizedHashMethod(CajetaModulePtr module, CajetaClassPtr parent);

        void generateCode() override;
    };
}
