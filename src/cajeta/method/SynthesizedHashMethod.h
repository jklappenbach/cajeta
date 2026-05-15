//
// Compiler-synthesized `int64 hash()` override for any class that
// doesn't manually declare one. Sister to DefaultConstructorMethod —
// follows the same pattern: subclass Method, override generateCode()
// to emit IR directly without going through a parsed BlockPtr.
//
// The synthesizer is the cut-3 implementation of the Object.hash()
// design in cajeta-docs/StandardLibrary.md §Object. Walks each
// (inherited + own) field, hashes it through the matching
// __cajeta_hash_X runtime symbol, combines results via
// __cajeta_hash_combine, seeded with __cajeta_hash_seed() so the
// hash carries the per-process random seed that defends against
// hash-flooding attacks.
//
// This first cut handles primitive fields (boolean, int8/16/32/64,
// uint8/16/32/64, float32/64) and class-reference fields (hashed
// by identity through __cajeta_hash_identity). Unsupported field
// kinds (struct, array, fp4/6/8/16/128) are skipped — their
// contribution is lost from the hash, which is correct but may
// produce more collisions until coverage widens.
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
